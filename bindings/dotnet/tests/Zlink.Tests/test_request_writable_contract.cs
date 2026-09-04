using Xunit;

namespace Systems.Zlink.Tests;

/// <summary>
///     Public REQUEST regressions for payload-free DONTWAIT wait tokens.
///     Synchronization uses socket pollers; these tests contain no sleeps or
///     timer-based retry loops.
/// </summary>
public sealed class test_request_writable_contract
{
    private const ulong RecordHwm = 65_536UL + 64UL;
    private static readonly string LargePayload =
        "request" + new string('r', 65_536);

    [Fact]
    public async Task hwm_refusal_retries_same_request_after_writable()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        context.Options.AutoHwmEnabled = false;
        using var dealer = context.CreateDealerSocket();
        using var router = context.CreateRouterSocket();
        using var completions = Zlink.CreatePoller();
        dealer.Options.SendHighWaterMark = RecordHwm;
        router.Options.ReceiveHighWaterMark = RecordHwm;
        string endpoint = CoreTestSupport.NewEndpoint(
            "inproc", "request-writable-hwm");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        Handshake(dealer, router);
        completions.Add(dealer,
            PollEventFlags.PollOut | PollEventFlags.PollCompletion, 1);

        using Message firstPart = Message.From(LargePayload + "-first");
        using Message retriedPart = Message.From(LargePayload + "-retry");
        Task<IReadOnlyList<Message>> first = dealer.Request()
            .Message(firstPart).Timeout(TimeSpan.FromSeconds(3)).Async();
        Task<IReadOnlyList<Message>> retried = dealer.Request()
            .Message(retriedPart).Timeout(TimeSpan.FromSeconds(3)).Async();

        using Received firstReceived = Receive(router);
        Assert.Equal(LargePayload + "-first",
            firstReceived.SinglePartOrThrow().GetString());
        Reply(router, firstReceived, "first-reply");
        WaitCompletion(completions);
        IReadOnlyList<Message> firstReply = await first.WaitAsync(
            TimeSpan.FromSeconds(3));
        Assert.Equal("first-reply", firstReply.Single().GetString());
        Zlink.MultipartClose(firstReply);

        using Received retriedReceived = Receive(router);
        Assert.Equal(LargePayload + "-retry",
            retriedReceived.SinglePartOrThrow().GetString());
        Reply(router, retriedReceived, "retry-reply");
        WaitCompletion(completions);
        IReadOnlyList<Message> retriedReply = await retried.WaitAsync(
            TimeSpan.FromSeconds(3));
        Assert.Equal("retry-reply", retriedReply.Single().GetString());
        Zlink.MultipartClose(retriedReply);
    }

    [Fact]
    public async Task connect_before_bind_waits_for_writable_then_replies()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        using var dealer = context.CreateDealerSocket();
        using var router = context.CreateRouterSocket();
        string endpoint = CoreTestSupport.NewEndpoint(
            "inproc", "request-writable-connect-first");
        dealer.Connect(endpoint);

        using Message part = Message.From("connect-before-bind");
        Task<IReadOnlyList<Message>> pending = dealer.Request()
            .Message(part).Timeout(TimeSpan.FromSeconds(3)).Async();
        Assert.False(pending.IsCompleted);

        router.Bind(endpoint);
        using Received request = Receive(router);
        Assert.Equal("connect-before-bind",
            request.SinglePartOrThrow().GetString());
        Reply(router, request, "connected-reply");

        IReadOnlyList<Message> reply = await pending.WaitAsync(
            TimeSpan.FromSeconds(3));
        Assert.Equal("connected-reply", reply.Single().GetString());
        Zlink.MultipartClose(reply);
    }

    [Fact]
    public async Task close_retires_request_wait_token_as_typed_failure()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        var dealer = context.CreateDealerSocket();
        string endpoint = CoreTestSupport.NewEndpoint(
            "inproc", "request-writable-close");
        dealer.Connect(endpoint);

        using Message part = Message.From("close-before-admission");
        Task<IReadOnlyList<Message>> pending = dealer.Request()
            .Message(part).Timeout(TimeSpan.FromSeconds(3)).Async();
        Assert.False(pending.IsCompleted);

        dealer.Dispose();
        ZlinkSubmitException error = await Assert.ThrowsAsync<
            ZlinkSubmitException>(() => pending);
        Assert.Equal(ZlinkSubmitException.ErrorCode.Terminated, error.Result);
    }

    [Fact]
    public async Task route_removal_retires_request_token_as_not_found()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        context.Options.AutoHwmEnabled = false;
        using var client = context.CreateRouterSocket();
        using var server = context.CreateRouterSocket();
        RoutingId clientRid = CoreTestSupport.RoutingIdUtf8(
            "request-disconnect-client");
        RoutingId serverRid = CoreTestSupport.RoutingIdUtf8(
            "request-disconnect-server");
        client.SetRoutingId(clientRid);
        server.SetRoutingId(serverRid);
        client.Options.SetConnectRoutingId(serverRid);
        client.Options.SendHighWaterMark = RecordHwm;
        server.Options.ReceiveHighWaterMark = RecordHwm;
        string endpoint = CoreTestSupport.NewEndpoint(
            "inproc", "request-writable-disconnect");
        server.Bind(endpoint);
        client.Connect(endpoint);
        Handshake(client, server, serverRid);

        using Message admittedPart = Message.From(LargePayload + "-admitted");
        using Message waitingPart = Message.From(LargePayload + "-waiting");
        Task<IReadOnlyList<Message>> admitted = client.Request(serverRid)
            .Message(admittedPart).Timeout(TimeSpan.FromSeconds(3)).Async();
        Task<IReadOnlyList<Message>> pending = client.Request(serverRid)
            .Message(waitingPart).Timeout(TimeSpan.FromSeconds(3)).Async();
        Assert.False(pending.IsCompleted);

        client.DisconnectRid(serverRid);
        ZlinkSubmitException error = await Assert.ThrowsAsync<
            ZlinkSubmitException>(() => pending.WaitAsync(
                TimeSpan.FromSeconds(3)));
        Assert.Equal(ZlinkSubmitException.ErrorCode.NotFound, error.Result);
        _ = admitted.ContinueWith(static task => _ = task.Exception,
            TaskScheduler.Default);
    }

    [Fact]
    public async Task send_and_request_tokens_share_the_completion_owner()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        using var dealer = context.CreateDealerSocket();
        using var router = context.CreateRouterSocket();
        string endpoint = CoreTestSupport.NewEndpoint(
            "inproc", "request-writable-mixed");
        dealer.Connect(endpoint);

        using Message sendPart = Message.From("mixed-send");
        using Message requestPart = Message.From("mixed-request");
        Task send = dealer.Send().Message(sendPart).Async();
        Task<IReadOnlyList<Message>> request = dealer.Request()
            .Message(requestPart).Timeout(TimeSpan.FromSeconds(3)).Async();
        Assert.False(send.IsCompleted);
        Assert.False(request.IsCompleted);

        router.Bind(endpoint);
        for (var i = 0; i < 2; i++)
        {
            using Received received = Receive(router);
            string payload = received.SinglePartOrThrow().GetString();
            if (received.MessageType == ReceivedMessageType.Request)
            {
                Assert.Equal("mixed-request", payload);
                Reply(router, received, "mixed-reply");
            }
            else
            {
                Assert.Equal(ReceivedMessageType.Raw, received.MessageType);
                Assert.Equal("mixed-send", payload);
            }
        }

        await send.WaitAsync(TimeSpan.FromSeconds(3));
        IReadOnlyList<Message> reply = await request.WaitAsync(
            TimeSpan.FromSeconds(3));
        Assert.Equal("mixed-reply", reply.Single().GetString());
        Zlink.MultipartClose(reply);
    }

    private static void Handshake(IDealerSocket dealer, IRouterSocket router)
    {
        using Message part = Message.From("handshake");
        dealer.Send().Message(part).Submit();
        using Received received = Receive(router);
        Assert.Equal("handshake", received.SinglePartOrThrow().GetString());
    }

    private static void Handshake(IRouterSocket client, IRouterSocket server,
        RoutingId serverRid)
    {
        using Message part = Message.From("handshake");
        client.Send(serverRid).Message(part).Submit();
        using Received received = Receive(server);
        Assert.Equal("handshake", received.SinglePartOrThrow().GetString());
    }

    private static Received Receive(IRouterSocket router)
    {
        using var poller = Zlink.CreatePoller();
        poller.Add(router, PollEventFlags.PollIn, 1);
        var events = new PollEvent[1];
        Assert.Equal(1, poller.Wait(events, TimeSpan.FromSeconds(3)));
        var received = Received.Create();
        Assert.True(router.Recv(received, RecvFlags.DontWait));
        return received;
    }

    private static void WaitCompletion(IPoller poller)
    {
        var events = new PollEvent[1];
        Assert.Equal(1, poller.Wait(events, TimeSpan.FromSeconds(3)));
        Assert.NotEqual(PollEventFlags.None,
            events[0].Revents & PollEventFlags.PollCompletion);
    }

    private static void Reply(IRouterSocket router, Received request,
        string payload)
    {
        Assert.Equal(ReceivedMessageType.Request, request.MessageType);
        using Message reply = Message.From(payload);
        router.Reply(request.RoutingId!.Value, request.ReplyToken!)
            .Message(reply).Submit();
    }
}
