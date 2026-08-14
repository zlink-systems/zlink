using System.Diagnostics;
using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_routed_async_admission
{
    private const ulong RecordHwm = 65_536UL + 64UL;

    [Fact]
    public async Task dealer_multipart_send_returns_before_credit_and_resumes()
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

        await FillDealerTargetAsync(dealer);

        using Message header = Message.From("pending-header");
        using Message body = Message.From("pending-body");
        using var cancellation = new CancellationTokenSource(
            TimeSpan.FromSeconds(3));
        var started = Stopwatch.StartNew();
        Task pending = dealer.Send()
            .Messages([header, body])
            .Async(cancellation.Token);
        started.Stop();

        Assert.True(started.Elapsed < TimeSpan.FromMilliseconds(250));
        await Task.Delay(50);
        Assert.False(pending.IsCompleted);

        Received? admitted = null;
        while (!pending.IsCompleted && admitted == null)
        {
            Received received = RecvWithRetry(router);
            if (received.Parts[0].GetString() == "pending-header")
                admitted = received;
            else
                received.Dispose();
        }

        await pending.WaitAsync(TimeSpan.FromSeconds(2));
        admitted ??= RecvWithRetry(router);
        using (admitted)
            Assert.Collection(admitted.Parts,
                part => Assert.Equal("pending-header", part.GetString()),
                part => Assert.Equal("pending-body", part.GetString()));
    }

    [Fact]
    public async Task blocked_router_target_does_not_delay_another_target()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        context.Options.AutoHwmEnabled = false;
        using var router = context.CreateRouterSocket();
        using var dealerA = context.CreateDealerSocket();
        using var dealerB = context.CreateDealerSocket();
        RoutingId ridA = CoreTestSupport.RoutingIdUtf8("async-target-a");
        RoutingId ridB = CoreTestSupport.RoutingIdUtf8("async-target-b");
        dealerA.SetRoutingId(ridA);
        dealerB.SetRoutingId(ridB);
        router.Options.SendHighWaterMark = RecordHwm;
        dealerA.Options.ReceiveHighWaterMark = RecordHwm;
        dealerB.Options.ReceiveHighWaterMark = RecordHwm;

        string endpoint = CoreTestSupport.NewEndpoint(
            "inproc", "dotnet-routed-async-targets");
        router.Bind(endpoint);
        dealerA.Connect(endpoint);
        dealerB.Connect(endpoint);
        Thread.Sleep(100);

        await FillRouterTargetAsync(router, ridA);

        using Message blockedPayload = Message.From("blocked-a");
        using var cancellation = new CancellationTokenSource();
        Task blocked = router.Send(ridA)
            .Message(blockedPayload)
            .Async(cancellation.Token);
        await Task.Delay(50);
        Assert.False(blocked.IsCompleted);

        using Message header = Message.From("ready-b-header");
        using Message body = Message.From("ready-b-body");
        Task independent = router.Send(ridB)
            .Messages([header, body])
            .Async();
        await independent.WaitAsync(TimeSpan.FromSeconds(2));

        using Received received = RecvWithRetry(dealerB);
        Assert.Collection(received.Parts,
            part => Assert.Equal("ready-b-header", part.GetString()),
            part => Assert.Equal("ready-b-body", part.GetString()));
        Assert.False(blocked.IsCompleted);

        cancellation.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(async () =>
            await blocked);
    }

    [Fact]
    public async Task close_completes_a_pending_send_once()
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
            "inproc", "dotnet-routed-async-close");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        Thread.Sleep(100);
        await FillDealerTargetAsync(dealer);

        using Message payload = Message.From("pending-close");
        Task pending = dealer.Send().Message(payload).Async();
        await Task.Delay(50);
        Assert.False(pending.IsCompleted);

        dealer.Dispose();
        ZlinkSubmitException error = await Assert.ThrowsAsync<
            ZlinkSubmitException>(async () => await pending);
        Assert.Equal(ZlinkSubmitException.ErrorCode.Terminated, error.Result);
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
        await FillRouterTargetAsync(router, dealerRid);

        using Message payload = Message.From("pending-disconnect");
        Task pending = router.Send(dealerRid).Message(payload).Async();
        await Task.Delay(50);
        Assert.False(pending.IsCompleted);

        router.DisconnectRid(dealerRid);
        ZlinkSubmitException error = await Assert.ThrowsAsync<
            ZlinkSubmitException>(async () => await pending.WaitAsync(
            TimeSpan.FromSeconds(2)));
        Assert.Contains(error.Result, new[]
        {
            ZlinkSubmitException.ErrorCode.NotConnected,
            ZlinkSubmitException.ErrorCode.NotFound,
            ZlinkSubmitException.ErrorCode.Terminated
        });
    }

    [Fact]
    public async Task send_and_request_deadlines_include_admission_wait()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        context.Options.AutoHwmEnabled = false;
        using var dealer = context.CreateDealerSocket();
        using var router = context.CreateRouterSocket();
        dealer.Options.SendHighWaterMark = RecordHwm;
        dealer.Options.SendTimeout = TimeSpan.FromMilliseconds(100);
        router.Options.ReceiveHighWaterMark = RecordHwm;

        string endpoint = CoreTestSupport.NewEndpoint(
            "inproc", "dotnet-routed-request-deadline");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        Thread.Sleep(100);

        await FillDealerTargetAsync(dealer);

        using Message send = Message.From("deadline-send");
        var sendStarted = Stopwatch.StartNew();
        Task sendPending = dealer.Send().Message(send).Async();
        sendStarted.Stop();
        Assert.True(sendStarted.Elapsed < TimeSpan.FromMilliseconds(250));

        ZlinkSubmitException sendError = await Assert.ThrowsAsync<
            ZlinkSubmitException>(async () => await sendPending);
        Assert.Equal(ZlinkSubmitException.ErrorCode.Backpressured,
            sendError.Result);

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

    private static async Task FillDealerTargetAsync(IDealerSocket dealer)
    {
        bool backpressured = false;
        string payload = "filler" + new string('d', 65_536);
        TimeSpan? previousTimeout = dealer.Options.SendTimeout;
        dealer.Options.SendTimeout = TimeSpan.Zero;
        try
        {
            for (var attempt = 0; attempt < 16; attempt++)
            {
                using Message filler = Message.From(payload);
                try
                {
                    await dealer.Send().Message(filler).Async();
                }
                catch (ZlinkSubmitException error) when (
                    error.Result == ZlinkSubmitException.ErrorCode.Backpressured)
                {
                    backpressured = true;
                    break;
                }
            }
        }
        finally
        {
            dealer.Options.SendTimeout = previousTimeout;
        }

        Assert.True(backpressured);
    }

    private static async Task FillRouterTargetAsync(IRouterSocket router,
        RoutingId routingId)
    {
        bool backpressured = false;
        string payload = "filler" + new string('r', 65_536);
        TimeSpan? previousTimeout = router.Options.SendTimeout;
        router.Options.SendTimeout = TimeSpan.Zero;
        try
        {
            for (var attempt = 0; attempt < 16; attempt++)
            {
                using Message filler = Message.From(payload);
                try
                {
                    await router.Send(routingId).Message(filler).Async();
                }
                catch (ZlinkSubmitException error) when (
                    error.Result == ZlinkSubmitException.ErrorCode.Backpressured)
                {
                    backpressured = true;
                    break;
                }
            }
        }
        finally
        {
            router.Options.SendTimeout = previousTimeout;
        }

        Assert.True(backpressured);
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

    private static Received RecvWithRetry(IDealerSocket socket)
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

        throw new TimeoutException("Timed out waiting for dealer record.");
    }
}
