using System.Diagnostics;
using Xunit;

namespace Systems.Zlink.Tests;

/// <summary>
///     The routed asynchronous send terminal is a thin wrapper over the Core
///     0.13.0 send-completion contract: one complete record goes to
///     <c>zlink_send_async</c>, and exactly one Core completion resolves the
///     Task. The binding parks nothing, retries nothing and times nothing.
/// </summary>
public sealed class test_routed_async_admission
{
    private const ulong RecordHwm = 65_536UL + 64UL;
    private static readonly string FillerPayload =
        "filler" + new string('d', 65_536);

    [Fact]
    public async Task admitted_send_completes_without_occupying_the_caller()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        using var dealer = context.CreateDealerSocket();
        using var router = context.CreateRouterSocket();
        string endpoint = CoreTestSupport.NewEndpoint(
            "inproc", "dotnet-routed-inline-admission");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        Thread.Sleep(100);

        using Message payload = Message.From("inline-payload");
        var started = Stopwatch.StartNew();
        Task admitted = dealer.Send().Message(payload).Async();
        started.Stop();

        // The submit hands the record to Core and returns; whether Core runs
        // the completion inline or on its own dispatch context, the caller is
        // never occupied waiting for credit.
        Assert.True(started.Elapsed < TimeSpan.FromMilliseconds(250));
        await admitted.WaitAsync(TimeSpan.FromSeconds(3));

        using Received received = RecvWithRetry(router);
        Assert.Collection(received.Parts,
            part => Assert.Equal("inline-payload", part.GetString()));
    }

    [Fact]
    public async Task multipart_record_admission()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        using var dealer = context.CreateDealerSocket();
        using var router = context.CreateRouterSocket();
        string endpoint = CoreTestSupport.NewEndpoint(
            "inproc", "dotnet-routed-multipart-admission");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        Thread.Sleep(100);

        using Message header = Message.From("mp-header");
        using Message body = Message.From("mp-body");
        await dealer.Send().Messages([header, body]).Async()
            .WaitAsync(TimeSpan.FromSeconds(3));

        using Received received = RecvWithRetry(router);
        Assert.Collection(received.Parts,
            part => Assert.Equal("mp-header", part.GetString()),
            part => Assert.Equal("mp-body", part.GetString()));
    }

    [Fact]
    public async Task backpressured_send_returns_immediately_and_core_completes_it()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        context.Options.AutoHwmEnabled = false;
        using var dealer = context.CreateDealerSocket();
        using var router = context.CreateRouterSocket();
        dealer.Options.SendHighWaterMark = RecordHwm;
        router.Options.ReceiveHighWaterMark = RecordHwm;

        string endpoint = CoreTestSupport.NewEndpoint(
            "inproc", "dotnet-routed-async-multipart");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        Thread.Sleep(100);

        Task parked = FillDealerTarget(dealer, out List<Task> filler);

        using Message payload = Message.From("pending-payload");
        var started = Stopwatch.StartNew();
        Task pending = dealer.Send().Message(payload).Async();
        started.Stop();

        // The submit never waits for credit, whether or not it parks.
        Assert.True(started.Elapsed < TimeSpan.FromMilliseconds(250));
        Assert.False(parked.IsCompleted);

        // Draining the receiver returns credit and Core completes every
        // parked record in submit order.
        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(10);
        while (!pending.IsCompleted && DateTimeOffset.UtcNow < deadline)
        {
            using var received = Received.Create();
            if (!router.Recv(received, RecvFlags.DontWait))
                await Task.Delay(1);
        }

        await pending.WaitAsync(TimeSpan.FromSeconds(2));
        await Task.WhenAll(filler).WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task cancellation_completes_a_parked_send_once()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        context.Options.AutoHwmEnabled = false;
        using var dealer = context.CreateDealerSocket();
        using var router = context.CreateRouterSocket();
        dealer.Options.SendHighWaterMark = RecordHwm;
        router.Options.ReceiveHighWaterMark = RecordHwm;

        string endpoint = CoreTestSupport.NewEndpoint(
            "inproc", "dotnet-routed-async-cancel");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        Thread.Sleep(100);
        _ = FillDealerTarget(dealer, out List<Task> filler);

        using var cancellation = new CancellationTokenSource();
        using Message payload = Message.From("pending-cancel");
        Task pending = dealer.Send()
            .Message(payload)
            .Async(cancellation.Token);
        await Task.Delay(50);
        Assert.False(pending.IsCompleted);

        cancellation.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(async () =>
            await pending.WaitAsync(TimeSpan.FromSeconds(3)));
        // Exactly-once: the terminal stays cancelled.
        Assert.True(pending.IsCanceled);
        SwallowAll(filler);
    }

    [Fact]
    public async Task close_completes_a_pending_send_once()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        context.Options.AutoHwmEnabled = false;
        var dealer = context.CreateDealerSocket();
        using var router = context.CreateRouterSocket();
        dealer.Options.SendHighWaterMark = RecordHwm;
        router.Options.ReceiveHighWaterMark = RecordHwm;

        string endpoint = CoreTestSupport.NewEndpoint(
            "inproc", "dotnet-routed-async-close");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        Thread.Sleep(100);
        _ = FillDealerTarget(dealer, out List<Task> filler);

        using Message payload = Message.From("pending-close");
        Task pending = dealer.Send().Message(payload).Async();
        await Task.Delay(50);
        Assert.False(pending.IsCompleted);

        dealer.Dispose();
        ZlinkSubmitException error = await Assert.ThrowsAsync<
            ZlinkSubmitException>(async () =>
            await pending.WaitAsync(TimeSpan.FromSeconds(3)));
        Assert.Equal(ZlinkSubmitException.ErrorCode.Terminated, error.Result);
        SwallowAll(filler);
    }

    [Fact]
    public async Task close_completes_an_accepted_request_once()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        using var dealer = context.CreateDealerSocket();
        using var router = context.CreateRouterSocket();
        string endpoint = CoreTestSupport.NewEndpoint(
            "inproc", "dotnet-routed-async-close-request");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        Thread.Sleep(100);

        using Message request = Message.From("accepted-before-close");
        Task<IReadOnlyList<Message>> pending = dealer.Request()
            .Message(request)
            .Timeout(TimeSpan.FromSeconds(2))
            .Async();
        using Received accepted = RecvWithRetry(router);
        Assert.Equal(ReceivedMessageType.Request, accepted.MessageType);

        dealer.Dispose();
        ZlinkRequestException error = await Assert.ThrowsAsync<
            ZlinkRequestException>(async () => await pending);
        Assert.Equal(ZlinkRequestException.ErrorCode.Terminated, error.Result);
    }

    [Fact]
    public async Task disconnect_terminates_the_selected_generation()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        context.Options.AutoHwmEnabled = false;
        using var router = context.CreateRouterSocket();
        using var dealer = context.CreateDealerSocket();
        RoutingId dealerRid = CoreTestSupport.RoutingIdUtf8(
            "async-disconnect-target");
        dealer.SetRoutingId(dealerRid);
        router.Options.SendHighWaterMark = RecordHwm;
        dealer.Options.ReceiveHighWaterMark = RecordHwm;

        string endpoint = CoreTestSupport.NewEndpoint(
            "inproc", "dotnet-routed-async-disconnect");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        Thread.Sleep(100);
        _ = FillRouterTarget(router, dealerRid, out List<Task> filler);

        using Message payload = Message.From("pending-disconnect");
        Task pending = router.Send(dealerRid).Message(payload).Async();
        await Task.Delay(50);
        Assert.False(pending.IsCompleted);

        router.DisconnectRid(dealerRid);
        ZlinkSubmitException error = await Assert.ThrowsAsync<
            ZlinkSubmitException>(async () => await pending.WaitAsync(
            TimeSpan.FromSeconds(3)));
        Assert.Contains(error.Result, new[]
        {
            ZlinkSubmitException.ErrorCode.NotConnected,
            ZlinkSubmitException.ErrorCode.NotFound,
            ZlinkSubmitException.ErrorCode.Terminated
        });
        SwallowAll(filler);
    }

    [Fact]
    public async Task request_timeout_is_core_owned()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        using var dealer = context.CreateDealerSocket();
        using var router = context.CreateRouterSocket();
        string endpoint = CoreTestSupport.NewEndpoint(
            "inproc", "dotnet-routed-request-deadline");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        Thread.Sleep(100);

        using Message request = Message.From("deadline-request");
        var started = Stopwatch.StartNew();
        Task<IReadOnlyList<Message>> pending = dealer.Request()
            .Message(request)
            .Timeout(TimeSpan.FromMilliseconds(100))
            .Async();
        started.Stop();
        Assert.True(started.Elapsed < TimeSpan.FromMilliseconds(250));

        ZlinkRequestException error = await Assert.ThrowsAsync<
            ZlinkRequestException>(async () => await pending);
        Assert.Equal(ZlinkRequestException.ErrorCode.TimedOut, error.Result);
    }

    /// <summary>
    ///     Submits large records until one parks, and returns that parked task.
    ///     `zlink_send_async` never reports HWM back-pressure to the caller: a
    ///     record that cannot be admitted becomes a Core pending operation.
    /// </summary>
    private static Task FillDealerTarget(IDealerSocket dealer,
        out List<Task> submitted)
    {
        submitted = new List<Task>();
        for (var attempt = 0; attempt < 16; attempt++)
        {
            using Message filler = Message.From(FillerPayload);
            Task task = dealer.Send().Message(filler).Async();
            submitted.Add(task);
            if (!task.IsCompleted)
                return task;
        }

        throw new Xunit.Sdk.XunitException(
            "The DEALER target never parked a record.");
    }

    private static Task FillRouterTarget(IRouterSocket router,
        RoutingId routingId, out List<Task> submitted)
    {
        submitted = new List<Task>();
        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(3);
        for (var attempt = 0; attempt < 64; attempt++)
        {
            using Message filler = Message.From(FillerPayload);
            Task task;
            try
            {
                task = router.Send(routingId).Message(filler).Async();
            }
            catch (ZlinkSubmitException) when (DateTimeOffset.UtcNow < deadline)
            {
                // The ROUTER has not admitted the route yet. Retrying is the
                // application's policy: the binding never retries.
                Thread.Sleep(10);
                continue;
            }

            submitted.Add(task);
            if (!task.IsCompleted)
                return task;
        }

        throw new Xunit.Sdk.XunitException(
            "The ROUTER target never parked a record.");
    }

    private static void SwallowAll(IEnumerable<Task> tasks)
    {
        foreach (Task task in tasks)
            _ = task.ContinueWith(static completed => _ = completed.Exception,
                TaskScheduler.Default);
    }

    private static Received RecvWithRetry(IRouterSocket socket)
    {
        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(3);
        while (DateTimeOffset.UtcNow < deadline)
        {
            var received = Received.Create();
            if (socket.Recv(received, RecvFlags.DontWait))
                return received;
            received.Dispose();
            Thread.Sleep(1);
        }

        throw new TimeoutException("Timed out waiting for routed record.");
    }
}
