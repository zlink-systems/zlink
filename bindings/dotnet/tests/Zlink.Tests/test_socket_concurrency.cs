using System.Collections.Concurrent;
using System.Diagnostics;
using System.Text;
using Xunit;
using Xunit.Abstractions;

namespace Systems.Zlink.Tests;

public sealed class test_socket_concurrency
{
    private readonly ITestOutputHelper _output;

    public test_socket_concurrency(ITestOutputHelper output)
    {
        _output = output;
    }

    [Fact]
    public async Task dealer_and_router_allow_concurrent_public_sends()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        using var router = context.CreateRouterSocket();
        using var dealer = context.CreateDealerSocket();
        string endpoint = CoreTestSupport.NewEndpoint(
            "inproc", "concurrent-public-sends");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        Thread.Sleep(50);

        const int producerCount = 4;
        const int messagesPerProducer = 32;
        const int messageCount = producerCount * messagesPerProducer;

        var dealerStart = new Barrier(producerCount);
        Task[] dealerSends = Enumerable.Range(0, producerCount)
            .Select(producer => Task.Run(async () =>
            {
                dealerStart.SignalAndWait();
                for (var messageIndex = 0;
                     messageIndex < messagesPerProducer;
                     messageIndex++)
                {
                    using Message header = Message.From(
                        $"dealer-{producer}-{messageIndex}-header");
                    using Message body = Message.From(
                        $"dealer-{producer}-{messageIndex}-body");
                    await dealer.Send()
                        .Message(header)
                        .Message(body)
                        .Async();
                }
            }))
            .ToArray();
        await Task.WhenAll(dealerSends).WaitAsync(TimeSpan.FromSeconds(10));
        dealerStart.Dispose();

        using var received = Received.Create();
        RoutingId? dealerRoutingId = null;
        var receivedDealerMessages = 0;
        var dealerPayloads = new HashSet<string>(StringComparer.Ordinal);
        while (receivedDealerMessages < messageCount)
        {
            Assert.True(CoreTestSupport.WaitUntil(
                () => router.Recv(received, RecvFlags.DontWait), 2000));
            dealerRoutingId ??= received.RoutingId;
            Assert.Equal(2, received.Parts.Count);
            dealerPayloads.Add(string.Join("|", received.Parts.Select(
                part => Encoding.UTF8.GetString(part.AsReadOnlySpan()))));
            receivedDealerMessages++;
        }

        Assert.Equal(messageCount, dealerPayloads.Count);
        Assert.All(dealerPayloads, payload =>
        {
            string[] parts = payload.Split('|');
            Assert.Equal(2, parts.Length);
            Assert.EndsWith("-header", parts[0], StringComparison.Ordinal);
            Assert.Equal(parts[0][..^"-header".Length] + "-body", parts[1]);
        });
        RoutingId target = dealerRoutingId
            ?? throw new InvalidOperationException(
                "Concurrent dealer send did not expose a routing id.");

        var routerStart = new Barrier(producerCount);
        Task[] routerSends = Enumerable.Range(0, producerCount)
            .Select(producer => Task.Run(async () =>
            {
                routerStart.SignalAndWait();
                for (var messageIndex = 0;
                     messageIndex < messagesPerProducer;
                     messageIndex++)
                {
                    using Message header = Message.From(
                        $"router-{producer}-{messageIndex}-header");
                    using Message body = Message.From(
                        $"router-{producer}-{messageIndex}-body");
                    await router.Send(target)
                        .Message(header)
                        .Message(body)
                        .Async();
                }
            }))
            .ToArray();
        await Task.WhenAll(routerSends).WaitAsync(TimeSpan.FromSeconds(10));
        routerStart.Dispose();

        var receivedRouterMessages = 0;
        var routerPayloads = new HashSet<string>(StringComparer.Ordinal);
        while (receivedRouterMessages < messageCount)
        {
            Assert.True(CoreTestSupport.WaitUntil(
                () => dealer.Recv(received, RecvFlags.DontWait), 2000));
            Assert.Equal(2, received.Parts.Count);
            routerPayloads.Add(string.Join("|", received.Parts.Select(
                part => Encoding.UTF8.GetString(part.AsReadOnlySpan()))));
            receivedRouterMessages++;
        }

        Assert.Equal(messageCount, routerPayloads.Count);
        Assert.All(routerPayloads, payload =>
        {
            string[] parts = payload.Split('|');
            Assert.Equal(2, parts.Length);
            Assert.EndsWith("-header", parts[0], StringComparison.Ordinal);
            Assert.Equal(parts[0][..^"-header".Length] + "-body", parts[1]);
        });
    }

    [Fact]
    public void concurrent_multipart_rejection_consumes_submitted_caller_parts()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        context.Options.AutoHwmEnabled = false;
        using var sender = context.CreatePairSocket();
        using var receiver = context.CreatePairSocket();
        sender.Options.SendHighWaterMark = 0;
        receiver.Options.ReceiveHighWaterMark = 0;
        string endpoint = CoreTestSupport.NewEndpoint(
            "inproc", "concurrent-multipart-ownership");
        sender.Bind(endpoint);
        receiver.Connect(endpoint);
        Thread.Sleep(50);

        const int producerCount = 8;
        const int attemptsPerProducer = 5_000;
        const int totalAttempts = producerCount * attemptsPerProducer;
        using var start = new ManualResetEventSlim(false);
        var unexpected = new ConcurrentQueue<Exception>();
        var succeeded = 0;
        var rejected = 0;
        var receivedCount = 0;
        var badWireRecordCount = 0;
        var badOwnershipCount = 0;
        var producersDone = 0;

        var receiveThread = new Thread(() =>
        {
            try
            {
                using var received = Received.Create();
                while (Volatile.Read(ref producersDone) == 0
                    || Volatile.Read(ref receivedCount)
                    < Volatile.Read(ref succeeded))
                {
                    if (!receiver.Recv(received, RecvFlags.DontWait))
                    {
                        Thread.Yield();
                        continue;
                    }

                    if (received.Parts.Count != 2)
                    {
                        Interlocked.Increment(ref badWireRecordCount);
                    }
                    else
                    {
                        string header = received.Parts[0].GetString();
                        string body = received.Parts[1].GetString();
                        if (!header.EndsWith(":header", StringComparison.Ordinal)
                            || body != header[..^":header".Length] + ":body")
                            Interlocked.Increment(ref badWireRecordCount);
                    }
                    Interlocked.Increment(ref receivedCount);
                }
            }
            catch (Exception exception)
            {
                unexpected.Enqueue(exception);
            }
        }) { IsBackground = true };
        receiveThread.Start();

        Thread[] producers = Enumerable.Range(0, producerCount)
            .Select(producer => new Thread(() =>
            {
                try
                {
                    start.Wait();
                    for (var attempt = 0;
                         attempt < attemptsPerProducer;
                         attempt++)
                    {
                        string prefix = $"{producer}:{attempt}";
                        Message header = Message.From($"{prefix}:header");
                        Message body = Message.From($"{prefix}:body");
                        try
                        {
                            sender.Send().Message(header).Message(body).Submit();
                            Interlocked.Increment(ref succeeded);
                        }
                        catch (ZlinkSubmitException exception) when (
                            exception.Result ==
                            ZlinkSubmitException.ErrorCode.InvalidArgument)
                        {
                            // The first part was passed to Core and is consumed
                            // for both success and ordinary failure. The second
                            // part is either consumed if Core saw it or restored
                            // unchanged if the first submit rejected the record.
                            try
                            {
                                string readableHeader = header.GetString();
                                Interlocked.Increment(ref badOwnershipCount);
                                unexpected.Enqueue(new InvalidOperationException(
                                    $"submitted header remained readable: '{readableHeader}'"));
                            }
                            catch (ObjectDisposedException)
                            {
                                // Expected: the first part reached Core.
                            }

                            try
                            {
                                string restoredBody = body.GetString();
                                if (restoredBody != $"{prefix}:body")
                                {
                                    Interlocked.Increment(ref badOwnershipCount);
                                    unexpected.Enqueue(new InvalidOperationException(
                                        $"unsubmitted body='{restoredBody}' expected='{prefix}:body'"));
                                }
                            }
                            catch (ObjectDisposedException)
                            {
                                // The second submit was attempted and consumed.
                            }
                            Interlocked.Increment(ref rejected);
                        }
                        finally
                        {
                            header.Dispose();
                            body.Dispose();
                        }
                    }
                }
                catch (Exception exception)
                {
                    unexpected.Enqueue(exception);
                }
            }) { IsBackground = true })
            .ToArray();

        foreach (Thread producer in producers)
            producer.Start();
        start.Set();
        foreach (Thread producer in producers)
            Assert.True(producer.Join(TimeSpan.FromSeconds(30)));
        Volatile.Write(ref producersDone, 1);
        Assert.True(receiveThread.Join(TimeSpan.FromSeconds(30)));

        _output.WriteLine(
            $"attempts={totalAttempts} succeeded={succeeded} rejected={rejected} received={receivedCount} bad_wire_record_count={badWireRecordCount} bad_ownership_count={badOwnershipCount}");
        Assert.Empty(unexpected);
        Assert.Equal(totalAttempts, succeeded + rejected);
        Assert.True(succeeded > 0);
        Assert.True(rejected > 0);
        Assert.Equal(succeeded, receivedCount);
        Assert.Equal(0, badWireRecordCount);
        Assert.Equal(0, badOwnershipCount);
    }

    [Fact]
    public void busy_close_preserves_socket_until_retry_succeeds()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        context.Options.AutoHwmEnabled = false;
        var sender = context.CreatePairSocket();
        using var receiver = context.CreatePairSocket();
        sender.Options.SendHighWaterMark = 1;
        sender.Options.SendTimeout = TimeSpan.FromMilliseconds(750);
        receiver.Options.ReceiveHighWaterMark = 1;
        string endpoint = CoreTestSupport.NewEndpoint(
            "inproc", "close-busy-preserves-handle");
        sender.Bind(endpoint);
        receiver.Connect(endpoint);
        Thread.Sleep(50);

        byte[] bytes = new byte[64 * 1024];
        while (true)
        {
            using Message filler = Message.From(bytes);
            if (!sender.Send().Message(filler).Flags(SendFlags.DontWait)
                    .Submit())
                break;
        }

        using var sendStarted = new ManualResetEventSlim(false);
        Exception? sendError = null;
        var sendThread = new Thread(() =>
        {
            using Message blocked = Message.From(bytes);
            sendStarted.Set();
            try
            {
                sender.Send().Message(blocked).Submit();
            }
            catch (Exception exception)
            {
                sendError = exception;
            }
        }) { IsBackground = true };
        sendThread.Start();
        Assert.True(sendStarted.Wait(TimeSpan.FromSeconds(2)));
        Thread.Sleep(50);

        var elapsed = Stopwatch.StartNew();
        ZlinkCloseException closeError = Assert.Throws<ZlinkCloseException>(
            () => sender.Close());
        elapsed.Stop();
        Assert.Equal(ZlinkCloseException.ErrorCode.Busy, closeError.Result);
        Assert.True(elapsed.Elapsed < TimeSpan.FromMilliseconds(500));

        // EBUSY did not destroy the handle or mutate the managed lifecycle.
        Assert.Equal(TimeSpan.FromMilliseconds(750),
            sender.Options.SendTimeout);
        Assert.True(sendThread.Join(TimeSpan.FromSeconds(3)));
        Assert.IsType<ZlinkSubmitException>(sendError);

        sender.Close();
        _output.WriteLine(
            $"close_busy_elapsed_us={elapsed.Elapsed.TotalMicroseconds:F0} retry=success");
    }

    [Fact]
    public void single_multipart_and_close_race_exposes_only_core_outcomes()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        context.Options.AutoHwmEnabled = false;
        var sender = context.CreatePairSocket();
        using var receiver = context.CreatePairSocket();
        sender.Options.SendHighWaterMark = 0;
        receiver.Options.ReceiveHighWaterMark = 0;
        string endpoint = CoreTestSupport.NewEndpoint(
            "inproc", "mixed-send-close-race");
        sender.Bind(endpoint);
        receiver.Connect(endpoint);
        Thread.Sleep(50);

        const int producerCount = 8;
        const int attemptsPerProducer = 6_250;
        const int totalAttempts = producerCount * attemptsPerProducer;
        using var start = new ManualResetEventSlim(false);
        var unexpected = new ConcurrentQueue<Exception>();
        var attempted = 0;
        var succeeded = 0;
        var multipartRejected = 0;
        var shutdown = 0;
        var closeBusy = 0;
        var closeAccepted = 0;

        Thread[] producers = Enumerable.Range(0, producerCount)
            .Select(producer => new Thread(() =>
            {
                start.Wait();
                for (var attempt = 0; attempt < attemptsPerProducer; attempt++)
                {
                    Interlocked.Increment(ref attempted);
                    Message first = Message.From($"{producer}:{attempt}:first");
                    Message? second = (attempt & 1) == 0
                        ? Message.From($"{producer}:{attempt}:second")
                        : null;
                    try
                    {
                        SendSubmitOperation operation = sender.Send().Message(first);
                        if (second != null)
                            operation = operation.Message(second);
                        operation.Submit();
                        Interlocked.Increment(ref succeeded);
                    }
                    catch (ZlinkSubmitException exception) when (
                        exception.Result ==
                        ZlinkSubmitException.ErrorCode.InvalidArgument)
                    {
                        Interlocked.Increment(ref multipartRejected);
                    }
                    catch (ZlinkSubmitException exception) when (
                        exception.Result is ZlinkSubmitException.ErrorCode.Terminated
                            or ZlinkSubmitException.ErrorCode.InvalidHandle
                            or ZlinkSubmitException.ErrorCode.NotConnected)
                    {
                        Interlocked.Increment(ref shutdown);
                    }
                    catch (ObjectDisposedException)
                    {
                        Interlocked.Increment(ref shutdown);
                    }
                    catch (Exception exception)
                    {
                        unexpected.Enqueue(exception);
                    }
                    finally
                    {
                        first.Dispose();
                        second?.Dispose();
                    }
                }
            }) { IsBackground = true })
            .ToArray();

        var closeThread = new Thread(() =>
        {
            start.Wait();
            while (Volatile.Read(ref attempted) < 20_000)
                Thread.Yield();
            while (true)
            {
                try
                {
                    sender.Close();
                    Interlocked.Exchange(ref closeAccepted, 1);
                    return;
                }
                catch (ZlinkCloseException exception) when (
                    exception.Result == ZlinkCloseException.ErrorCode.Busy)
                {
                    Interlocked.Increment(ref closeBusy);
                    Thread.Yield();
                }
                catch (Exception exception)
                {
                    unexpected.Enqueue(exception);
                    return;
                }
            }
        }) { IsBackground = true };

        foreach (Thread producer in producers)
            producer.Start();
        closeThread.Start();
        start.Set();
        foreach (Thread producer in producers)
            Assert.True(producer.Join(TimeSpan.FromSeconds(30)));
        Assert.True(closeThread.Join(TimeSpan.FromSeconds(5)));

        _output.WriteLine(
            $"attempts={totalAttempts} succeeded={succeeded} multipart_rejected={multipartRejected} shutdown={shutdown} close_busy={closeBusy} close_accepted={closeAccepted}");
        Assert.Empty(unexpected);
        Assert.Equal(totalAttempts,
            succeeded + multipartRejected + shutdown);
        Assert.True(succeeded > 0);
        Assert.True(multipartRejected > 0);
        Assert.Equal(1, closeAccepted);
    }

}
