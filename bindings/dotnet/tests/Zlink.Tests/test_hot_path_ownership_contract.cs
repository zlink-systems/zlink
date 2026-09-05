using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_hot_path_ownership_contract
{
    [Theory]
    [InlineData(1)]
    [InlineData(2)]
    [InlineData(9)]
    [InlineData(33)]
    public async Task repeated_part_preserves_each_wire_part_and_consumes_source_once(int count)
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;
        using var context = Zlink.CreateContext();
        using var sender = context.CreateDealerSocket();
        using var receiver = context.CreateDealerSocket();
        var endpoint = CoreTestSupport.NewEndpoint("inproc", "scratch-alias");
        receiver.Bind(endpoint);
        sender.Connect(endpoint);
        using Message source = Message.From("shared-part");
        var operation = sender.Send().Message(source);
        for (var i = 1; i < count; i++)
            operation.Message(source);
        await operation.Async();
        Assert.Throws<ObjectDisposedException>(() => source.GetString());
        using Received received = Received.Create();
        Assert.True(receiver.Recv(received));
        Assert.Equal(count, received.Parts.Count);
        Assert.All(received.Parts, part => Assert.Equal("shared-part", part.GetString()));
    }

    [Theory]
    [InlineData(2)]
    [InlineData(9)]
    [InlineData(33)]
    public void invalid_last_part_preserves_prefix_and_does_not_stage_it(int count)
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;
        using var context = Zlink.CreateContext();
        using var sender = context.CreateDealerSocket();
        using var receiver = context.CreateDealerSocket();
        var endpoint = CoreTestSupport.NewEndpoint("inproc", "scratch-invalid");
        receiver.Bind(endpoint);
        sender.Connect(endpoint);
        using Message source = Message.From("prefix");
        using Message invalid = Message.From("invalid");
        invalid.Dispose();
        var operation = sender.Send().Message(source);
        for (var i = 1; i < count - 1; i++)
            operation.Message(source);
        operation.Message(invalid);
        Assert.Throws<ObjectDisposedException>(() => operation.TrySubmit());
        Assert.Equal("prefix", source.GetString());
        sender.Send().Message(source).Submit();
        using Received received = Received.Create();
        Assert.True(receiver.Recv(received));
        Assert.Equal("prefix", received.SinglePartOrThrow().GetString());
    }

    [Theory]
    [InlineData(2)]
    [InlineData(9)]
    [InlineData(33)]
    public void native_final_failure_preserves_all_originals(int count)
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;
        using var context = Zlink.CreateContext();
        using var router = context.CreateRouterSocket();
        router.Options.Mandatory = true;
        using Message source = Message.From("preserved");
        var operation = router.Send(RoutingId.From("missing"u8)).Message(source);
        for (var i = 1; i < count; i++)
            operation.Message(source);
        var error = Assert.Throws<ZlinkSubmitException>(() => operation.TrySubmit());
        Assert.Equal(ZlinkSubmitException.ErrorCode.NotConnected, error.Result);
        Assert.Equal("preserved", source.GetString());
    }
    [Theory]
    [InlineData(1)]
    [InlineData(2)]
    [InlineData(9)]
    [InlineData(33)]
    public async Task repeated_reply_part_consumes_source_once_and_rejects_spent_token(int count)
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;
        using var context = Zlink.CreateContext();
        using var dealer = context.CreateDealerSocket();
        using var router = context.CreateRouterSocket();
        var endpoint = CoreTestSupport.NewEndpoint("inproc", "reply-scratch-alias");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        using Message request = Message.From("request");
        var pending = dealer.Request().Message(request)
            .Timeout(TimeSpan.FromSeconds(2)).Async();
        using Received received = Received.Create();
        Assert.True(router.Recv(received));
        var rid = received.RoutingId!.Value;
        var token = received.ReplyToken!;
        using Message source = Message.From("shared-reply");
        var operation = router.Reply(rid, token).Message(source);
        for (var i = 1; i < count; i++)
            operation.Message(source);
        operation.Submit();
        Assert.Throws<ObjectDisposedException>(() => source.GetString());
        var reply = await pending.WaitAsync(TimeSpan.FromSeconds(2));
        try
        {
            Assert.Equal(count, reply.Count);
            Assert.All(reply, part => Assert.Equal("shared-reply", part.GetString()));
            using Message rejected = Message.From("preserved");
            var stale = router.Reply(rid, token).Message(rejected);
            for (var i = 1; i < count; i++)
                stale.Message(rejected);
            var error = Assert.Throws<ZlinkSubmitException>(() => stale.Submit());
            Assert.Equal(ZlinkSubmitException.ErrorCode.NotFound, error.Result);
            Assert.Equal("preserved", rejected.GetString());
            Assert.All(reply, part => Assert.Equal("shared-reply", part.GetString()));
        }
        finally
        {
            Zlink.MultipartClose(reply);
        }
    }

    [Theory]
    [InlineData(2)]
    [InlineData(9)]
    [InlineData(33)]
    public async Task invalid_reply_tail_preserves_prefix_and_token_for_valid_reply(int count)
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;
        using var context = Zlink.CreateContext();
        using var dealer = context.CreateDealerSocket();
        using var router = context.CreateRouterSocket();
        var endpoint = CoreTestSupport.NewEndpoint("inproc", "reply-scratch-invalid");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        using Message request = Message.From("request");
        var pending = dealer.Request().Message(request)
            .Timeout(TimeSpan.FromSeconds(2)).Async();
        using Received received = Received.Create();
        Assert.True(router.Recv(received));
        using Message source = Message.From("prefix");
        using Message invalid = Message.From("invalid");
        invalid.Dispose();
        var operation = router.Reply(received.RoutingId!.Value, received.ReplyToken!)
            .Message(source);
        for (var i = 1; i < count - 1; i++)
            operation.Message(source);
        operation.Message(invalid);
        Assert.Throws<ObjectDisposedException>(() => operation.Submit());
        Assert.Equal("prefix", source.GetString());
        received.Reply().Message(source).Submit();
        var reply = await pending.WaitAsync(TimeSpan.FromSeconds(2));
        try
        {
            Assert.Equal("prefix", Assert.Single(reply).GetString());
        }
        finally
        {
            Zlink.MultipartClose(reply);
        }
    }

    [Theory]
    [InlineData(2)]
    [InlineData(9)]
    [InlineData(33)]
    public void reused_receive_storage_does_not_reanimate_previous_collection(int count)
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;
        using var context = Zlink.CreateContext();
        using var sender = context.CreateDealerSocket();
        using var receiver = context.CreateDealerSocket();
        var endpoint = CoreTestSupport.NewEndpoint("inproc", "receive-storage");
        receiver.Bind(endpoint);
        sender.Connect(endpoint);
        using var received = Received.Create();

        void Send(string text)
        {
            using var part = Message.From(text);
            var operation = sender.Send().Message(part);
            for (var i = 1; i < count; i++)
                operation.Message(part);
            operation.Submit();
        }

        Send("first");
        Assert.True(receiver.Recv(received));
        var previous = received.Parts;
        using var iterator = previous.GetEnumerator();
        Assert.True(iterator.MoveNext());
        Assert.Equal("first", iterator.Current.GetString());

        Send("second");
        Assert.True(receiver.Recv(received));
        Assert.NotSame(previous, received.Parts);
        Assert.Equal(count, previous.Count);
        Assert.Throws<ObjectDisposedException>(() => previous[0]);
        Assert.Throws<ObjectDisposedException>(() => iterator.MoveNext());
        Assert.All(received.Parts, part => Assert.Equal("second", part.GetString()));
    }
    [Fact]
    public async Task completed_request_keeps_its_task_and_result_across_later_requests()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;
        using var context = Zlink.CreateContext();
        using var dealer = context.CreateDealerSocket();
        using var router = context.CreateRouterSocket();
        var endpoint = CoreTestSupport.NewEndpoint("inproc", "request-identity");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        using var received = Received.Create();

        Task<IReadOnlyList<Message>> Exchange(string text)
        {
            using var part = Message.From(text);
            var pending = dealer.Request().Message(part)
                .Timeout(TimeSpan.FromSeconds(2)).Async();
            Assert.True(router.Recv(received));
            router.Reply(received.RoutingId!.Value, received.ReplyToken!)
                .Message(received.Parts[0]).Submit();
            return pending;
        }

        var first = Exchange("first");
        var firstResult = await first;
        try
        {
            var second = Exchange("second");
            var secondResult = await second;
            try
            {
                Assert.NotSame(first, second);
                Assert.Same(firstResult, await first);
                Assert.Equal("first", firstResult[0].GetString());
                Assert.Equal("second", secondResult[0].GetString());
            }
            finally
            {
                Zlink.MultipartClose(secondResult);
            }
        }
        finally
        {
            Zlink.MultipartClose(firstResult);
        }
    }
    [Fact]
    public async Task concurrent_public_drain_joins_backpressured_send_publication()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;
        const int count = 256;
        using var context = Zlink.CreateContext();
        context.Options.AutoHwmEnabled = false;
        using var sender = context.CreateDealerSocket();
        using var receiver = context.CreateDealerSocket();
        sender.Options.SendHighWaterMark = 65_536 + 128;
        receiver.Options.ReceiveHighWaterMark = 65_536 + 128;
        receiver.Options.ReceiveTimeout = TimeSpan.FromSeconds(3);
        var endpoint = CoreTestSupport.NewEndpoint("inproc", "send-publication");
        receiver.Bind(endpoint);
        sender.Connect(endpoint);
        using var completions = Zlink.CreatePoller();
        completions.Add(sender, PollEventFlags.PollCompletion, 1);

        var sending = Task.Run(async () =>
        {
            for (var i = 0; i < count; i++)
            {
                using var sequence = Message.From(i.ToString());
                using var body = Message.Allocate(65_536);
                await sender.Send().Message(sequence).Message(body).Async();
            }
        });
        var receiving = Task.Run(() =>
        {
            using var received = Received.Create();
            for (var i = 0; i < count; i++)
            {
                Assert.True(receiver.Recv(received));
                Assert.Equal(2, received.Parts.Count);
                Assert.Equal(i.ToString(), received.Parts[0].GetString());
            }
        });
        var events = new PollEvent[1];
        while (!sending.IsCompleted && !receiving.IsFaulted)
        {
            if (completions.Wait(events, TimeSpan.FromSeconds(3)) == 0)
                Assert.True(sending.IsCompleted);
        }
        await Task.WhenAll(sending, receiving).WaitAsync(TimeSpan.FromSeconds(3));
    }
}
