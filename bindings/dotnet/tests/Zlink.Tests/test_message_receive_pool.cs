using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_message_receive_pool
{
    private const int ReceiveTimeoutMs = 2000;

    [Theory]
    [InlineData(1, 8)]
    [InlineData(1, 30)]
    [InlineData(3, 8)]
    [InlineData(3, 30)]
    [InlineData(257, 8)]
    [InlineData(257, 30)]
    public void repeated_part_disposal_preserves_the_next_receive(int count, int size)
    {
        Assert.True(CoreTestSupport.IsNativeAvailable());
        using var context = Zlink.CreateContext();
        using var sender = context.CreateDealerSocket();
        using var receiver = context.CreateDealerSocket();
        var endpoint = CoreTestSupport.NewEndpoint("inproc", "message-pool-dispose");
        receiver.Bind(endpoint);
        sender.Connect(endpoint);

        void Send(byte value)
        {
            using var part = Message.From(Enumerable.Repeat(value, size).ToArray());
            var operation = sender.Send().Message(part);
            for (var index = 1; index < count; index++)
                operation.Message(part);
            operation.Submit();
        }

        using var first = Received.Create();
        Send(1);
        Assert.True(receiver.Recv(first));
        foreach (var part in first.Parts)
        {
            part.Dispose();
            part.Dispose();
        }
        first.Dispose();
        GC.Collect();
        GC.WaitForPendingFinalizers();

        using var second = Received.Create();
        Send(2);
        Assert.True(receiver.Recv(second));
        using var third = Received.Create();
        Send(3);
        Assert.True(receiver.Recv(third));
        Assert.Equal(count, second.Parts.Count);
        Assert.Equal(count, third.Parts.Count);
        for (var index = 0; index < count; index++)
        {
            Assert.NotSame(second.Parts[index], third.Parts[index]);
            Assert.Equal(Enumerable.Repeat((byte)2, size), second.Parts[index].ToArray());
            Assert.Equal(Enumerable.Repeat((byte)3, size), third.Parts[index].ToArray());
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void topic_owner_can_dispose_an_already_disposed_part_before_next_receive(bool reuse)
    {
        Assert.True(CoreTestSupport.IsNativeAvailable());
        using var context = Zlink.CreateContext();
        using var publisher = context.CreateXPubSocket();
        using var subscriber = context.CreateSubSocket();
        var endpoint = CoreTestSupport.NewEndpoint("inproc", "topic-pool-dispose");
        publisher.Bind(endpoint);
        subscriber.Connect(endpoint);
        subscriber.SetSubscription(string.Empty);
        var subscription = new SubscriptionEvent();
        Assert.True(CoreTestSupport.WaitUntil(
            () => publisher.ReceiveSubscriptionEvent(subscription, RecvFlags.DontWait),
            ReceiveTimeoutMs));

        using var first = new TopicMessage();
        CoreTestSupport.PublishWithRetry(publisher, "first", "one"u8, ReceiveTimeoutMs);
        Assert.True(CoreTestSupport.WaitUntil(
            () => subscriber.Subscribe(first, RecvFlags.DontWait), ReceiveTimeoutMs));
        first.SinglePartOrThrow().Dispose();
        if (reuse)
            first.ReleaseForReuse();
        else
            first.Dispose();
        GC.Collect();
        GC.WaitForPendingFinalizers();

        using var second = reuse ? first : new TopicMessage();
        CoreTestSupport.PublishWithRetry(publisher, "second", "two"u8, ReceiveTimeoutMs);
        Assert.True(CoreTestSupport.WaitUntil(
            () => subscriber.Subscribe(second, RecvFlags.DontWait), ReceiveTimeoutMs));
        Assert.Equal("second", second.Topic);
        Assert.Equal("two", second.SinglePartOrThrow().GetString());

        using var third = new TopicMessage();
        CoreTestSupport.PublishWithRetry(publisher, "third", "three"u8, ReceiveTimeoutMs);
        Assert.True(CoreTestSupport.WaitUntil(
            () => subscriber.Subscribe(third, RecvFlags.DontWait), ReceiveTimeoutMs));
        Assert.NotSame(second.SinglePartOrThrow(), third.SinglePartOrThrow());
        Assert.Equal("two", second.SinglePartOrThrow().GetString());
        Assert.Equal("three", third.SinglePartOrThrow().GetString());
    }

    [Theory]
    [InlineData(1)]
    [InlineData(3)]
    public void repeated_reply_disposal_preserves_the_next_completion(int count)
    {
        Assert.True(CoreTestSupport.IsNativeAvailable());
        using var context = Zlink.CreateContext();
        using var dealer = context.CreateDealerSocket();
        using var router = context.CreateRouterSocket();
        var endpoint = CoreTestSupport.NewEndpoint("inproc", "reply-pool-dispose");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        using var poller = Zlink.CreatePoller();
        poller.Add(dealer, PollEventFlags.PollCompletion, 0);
        var events = new PollEvent[1];
        using var request = Received.Create();

        IReadOnlyList<Message> Exchange(string text)
        {
            using var payload = Message.From(text);
            var pending = dealer.Request().Message(payload)
                .Timeout(TimeSpan.FromMilliseconds(ReceiveTimeoutMs)).Async().Reply;
            Assert.True(router.Recv(request));
            using var response = Message.From(text);
            var operation = router.Reply(request.RoutingId!.Value, request.ReplyToken!)
                .Message(response);
            for (var index = 1; index < count; index++)
                operation.Message(response);
            operation.Submit();
            Assert.True(CoreTestSupport.WaitUntil(() =>
            {
                poller.Wait(events, TimeSpan.Zero);
                return pending.IsCompleted;
            }, ReceiveTimeoutMs));
            return pending.GetAwaiter().GetResult();
        }

        var first = Exchange("first");
        foreach (var part in first)
        {
            part.Dispose();
            part.Dispose();
        }
        Zlink.MultipartClose(first);

        var second = Exchange("second");
        try
        {
            Assert.Equal(count, second.Count);
            Assert.All(second, part => Assert.Equal("second", part.GetString()));
        }
        finally
        {
            Zlink.MultipartClose(second);
        }
    }
}
