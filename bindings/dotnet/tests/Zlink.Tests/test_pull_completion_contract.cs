using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_pull_completion_contract
{
    [Fact]
    public async Task reply_token_rejects_different_router_before_message_consumption()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var dealer = ctx.CreateDealerSocket();
        using var owner = ctx.CreateRouterSocket();
        using var other = ctx.CreateRouterSocket();
        string endpoint = CoreTestSupport.NewEndpoint("inproc", "reply-owner");
        owner.Bind(endpoint);
        dealer.Connect(endpoint);

        using Message request = Message.From("request");
        Task<IReadOnlyList<Message>> replyTask = dealer.Request()
            .Message(request).Timeout(TimeSpan.FromSeconds(2)).Async();
        using Received received = Receive(owner);
        ReplyToken token = received.ReplyToken
            ?? throw new InvalidOperationException("missing reply token");
        RoutingId source = received.RoutingId
            ?? throw new InvalidOperationException("missing source route");

        using Message rejected = Message.From("wrong-owner");
        ZlinkSubmitException error = Assert.Throws<ZlinkSubmitException>(() =>
            other.Reply(source, token).Message(rejected).Submit());
        Assert.Equal(ZlinkSubmitException.ErrorCode.InvalidArgument,
            error.Result);
        Assert.Equal("wrong-owner", rejected.GetString());

        using Message accepted = Message.From("accepted");
        owner.Reply(source, token).Message(accepted).Submit();
        IReadOnlyList<Message> reply = await replyTask.WaitAsync(
            TimeSpan.FromSeconds(2));
        Assert.Equal("accepted", Assert.Single(reply).GetString());
        Zlink.MultipartClose(reply);
    }

    [Fact]
    public async Task public_poller_owns_completion_and_transfers_it_back()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var dealer = ctx.CreateDealerSocket();
        using var router = ctx.CreateRouterSocket();
        using var poller = Zlink.CreatePoller();
        string endpoint = CoreTestSupport.NewEndpoint("inproc", "poll-owner");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        poller.Add(dealer, PollEventFlags.PollCompletion, 17);

        Task responder = Task.Run(() => ReplyOnce(router, "one"));
        using Message request = Message.From("one");
        Task<IReadOnlyList<Message>> pending = dealer.Request().Message(request)
            .Timeout(TimeSpan.FromSeconds(2)).Async();
        var events = new PollEvent[1];
        Assert.Equal(1, poller.Wait(events, TimeSpan.FromSeconds(2)));
        Assert.Equal((nuint)17, events[0].Slot);
        Assert.NotEqual(PollEventFlags.None,
            events[0].Revents & PollEventFlags.PollCompletion);
        IReadOnlyList<Message> reply = await pending;
        Zlink.MultipartClose(reply);
        await responder;

        poller.Modify(dealer, PollEventFlags.PollIn);
        poller.Modify(dealer, PollEventFlags.PollCompletion);
        Assert.True(poller.Remove(dealer));

        Task responderAfterTransfer = Task.Run(() => ReplyOnce(router, "two"));
        using Message second = Message.From("two");
        IReadOnlyList<Message> secondReply = await dealer.Request()
            .Message(second).Timeout(TimeSpan.FromSeconds(2)).Async();
        Zlink.MultipartClose(secondReply);
        await responderAfterTransfer;
    }

    [Fact]
    public async Task immediate_completions_join_after_submit_publish()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var sender = ctx.CreatePairSocket();
        using var receiver = ctx.CreatePairSocket();
        string endpoint = CoreTestSupport.NewEndpoint("inproc", "pre-return");
        receiver.Bind(endpoint);
        sender.Connect(endpoint);

        const int count = 256;
        Task drain = Task.Run(() =>
        {
            for (var i = 0; i < count; i++)
            {
                using Received item = Receive(receiver);
                Assert.Equal(i.ToString(), item.SinglePartOrThrow().GetString());
            }
        });
        for (var i = 0; i < count; i++)
        {
            using Message message = Message.From(i.ToString());
            await sender.Send().Message(message).Async()
                .WaitAsync(TimeSpan.FromSeconds(2));
        }
        await drain.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task dropped_request_results_do_not_block_late_completion_cleanup()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var dealer = ctx.CreateDealerSocket();
        using var router = ctx.CreateRouterSocket();
        string endpoint = CoreTestSupport.NewEndpoint("inproc", "late-cleanup");
        router.Bind(endpoint);
        dealer.Connect(endpoint);

        const int droppedCount = 32;
        Task responder = Task.Run(() =>
        {
            for (var i = 0; i < droppedCount; i++)
                ReplyOnce(router, $"drop-{i}");
            ReplyOnce(router, "probe");
        });
        for (var i = 0; i < droppedCount; i++)
        {
            using Message request = Message.From($"drop-{i}");
            _ = dealer.Request().Message(request)
                .Timeout(TimeSpan.FromSeconds(2)).Async();
        }

        using Message probe = Message.From("probe");
        IReadOnlyList<Message> probeReply = await dealer.Request()
            .Message(probe).Timeout(TimeSpan.FromSeconds(2)).Async()
            .WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal("probe", Assert.Single(probeReply).GetString());
        Zlink.MultipartClose(probeReply);
        await responder;
    }

    [Fact]
    public async Task non_ok_request_surfaces_typed_error_only()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var dealer = ctx.CreateDealerSocket();
        using var router = ctx.CreateRouterSocket();
        string endpoint = CoreTestSupport.NewEndpoint("inproc", "typed-timeout");
        router.Bind(endpoint);
        dealer.Connect(endpoint);

        using Message request = Message.From("no-reply");
        ZlinkRequestException error = await Assert.ThrowsAsync<ZlinkRequestException>(
            () => dealer.Request().Message(request)
                .Timeout(TimeSpan.FromMilliseconds(25)).Async());
        Assert.Equal(ZlinkRequestException.ErrorCode.TimedOut, error.Result);
    }

    private static Received Receive(IReceivingMessageSocket socket)
    {
        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(3);
        while (DateTime.UtcNow < deadline)
        {
            var received = Received.Create();
            if (socket.Recv(received, RecvFlags.DontWait))
                return received;
            received.Dispose();
            Thread.Sleep(1);
        }
        throw new TimeoutException("receive timed out");
    }

    private static void ReplyOnce(IRouterSocket router, string expected)
    {
        using Received received = Receive(router);
        Assert.Equal(expected, received.SinglePartOrThrow().GetString());
        using Message reply = Message.From(expected);
        router.Reply(received.RoutingId!.Value, received.ReplyToken!)
            .Message(reply).Submit();
    }
}
