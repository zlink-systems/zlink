using System;
using System.Threading;
using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_pubsub
{
    [Theory]
    [InlineData("tcp")]
    [InlineData("ipc")]
    [InlineData("inproc")]
    public void pubsub_basic(string transport)
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;
        if (!CoreTestSupport.IsTransportSupported(transport))
            return;

        using var ctx = Zlink.CreateContext();
        using var publisher = ctx.CreatePubSocket();
        using var subscriber = ctx.CreateSubSocket();

        string endpoint = CoreTestSupport.NewEndpoint(transport, "pubsub");
        publisher.Bind(endpoint);
        subscriber.Connect(endpoint);
        subscriber.SetSubscription(string.Empty);
        Thread.Sleep(100);

        CoreTestSupport.PublishWithRetry(publisher, string.Empty, "test"u8,
            2000);
        Assert.Equal("test", CoreTestSupport.SubscribeUtf8WithTimeout(subscriber,
            out string topic, 2000));
        Assert.Equal(string.Empty, topic);
    }

    [Theory]
    [InlineData("tcp")]
    [InlineData("ipc")]
    [InlineData("inproc")]
    public void pubsub_filtering_by_topic(string transport)
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;
        if (!CoreTestSupport.IsTransportSupported(transport))
            return;

        using var ctx = Zlink.CreateContext();
        using var publisher = ctx.CreatePubSocket();
        using var subscriber = ctx.CreateSubSocket();

        string endpoint = CoreTestSupport.NewEndpoint(transport, "pubsub-filter");
        publisher.Bind(endpoint);
        subscriber.Connect(endpoint);
        subscriber.SetSubscription("topicA");
        Thread.Sleep(100);

        CoreTestSupport.PublishWithRetry(publisher, "topicA", "hello"u8, 2000);
        CoreTestSupport.PublishWithRetry(publisher, "topicB", "world"u8, 2000);
        CoreTestSupport.PublishWithRetry(publisher, "topicA", "test"u8, 2000);

        Assert.Equal("hello", CoreTestSupport.SubscribeUtf8WithTimeout(subscriber,
            out string topic0, 2000));
        Assert.Equal("topicA", topic0);
        Assert.Equal("test", CoreTestSupport.SubscribeUtf8WithTimeout(subscriber,
            out string topic1, 2000));
        Assert.Equal("topicA", topic1);
        Assert.True(CoreTestSupport.ExpectNoSubscribedMessage(subscriber, 150));
    }

    [Theory]
    [InlineData("tcp")]
    [InlineData("ipc")]
    [InlineData("inproc")]
    public void xpub_xsub_roundtrip(string transport)
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;
        if (!CoreTestSupport.IsTransportSupported(transport))
            return;

        using var ctx = Zlink.CreateContext();
        using var xpub = ctx.CreateXPubSocket();
        using var xsub = ctx.CreateXSubSocket();

        string endpoint = CoreTestSupport.NewEndpoint(transport, "xpub-xsub");
        xpub.Bind(endpoint);
        xsub.Connect(endpoint);

        xsub.SetSubscription(string.Empty);
        byte[] topic = CoreTestSupport.ReceiveSubscriptionEventWithTimeout(xpub,
            out bool subscribed, 2000);
        Assert.True(subscribed);
        Assert.Empty(topic);
        Thread.Sleep(50);

        CoreTestSupport.PublishWithRetry(xpub, string.Empty, "xpub_xsub_test"u8,
            2000);
        Assert.Equal("xpub_xsub_test", CoreTestSupport.SubscribeUtf8WithTimeout(
            xsub, out string recvTopic, 2000));
        Assert.Equal(string.Empty, recvTopic);
    }

    [Fact]
    public void pubsub_reused_utf8_topic_roundtrips_after_cached_publish_encoding()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;
        if (!CoreTestSupport.IsTransportSupported("inproc"))
            return;

        using var ctx = Zlink.CreateContext();
        using var publisher = ctx.CreatePubSocket();
        using var subscriber = ctx.CreateSubSocket();

        string endpoint = CoreTestSupport.NewEndpoint("inproc",
            "pubsub-utf8-topic-cache");
        const string topic = "\uD1A0\uD53D.alpha";
        publisher.Bind(endpoint);
        subscriber.Connect(endpoint);
        subscriber.SetSubscription(topic);
        Thread.Sleep(100);

        CoreTestSupport.PublishWithRetry(publisher, topic, "one"u8, 2000);
        Assert.Equal("one", CoreTestSupport.SubscribeUtf8WithTimeout(
            subscriber, out string firstTopic, 2000));
        Assert.Equal(topic, firstTopic);

        CoreTestSupport.PublishWithRetry(publisher, topic, "two"u8, 2000);
        Assert.Equal("two", CoreTestSupport.SubscribeUtf8WithTimeout(
            subscriber, out string secondTopic, 2000));
        Assert.Equal(topic, secondTopic);
    }

    [Fact]
    public void pubsub_direct_single_message_publish_roundtrips()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;
        if (!CoreTestSupport.IsTransportSupported("inproc"))
            return;

        using var ctx = Zlink.CreateContext();
        using var publisher = ctx.CreatePubSocket();
        using var subscriber = ctx.CreateSubSocket();

        string endpoint = CoreTestSupport.NewEndpoint("inproc",
            "pubsub-direct-single-publish");
        const string topic = "direct";
        publisher.Bind(endpoint);
        subscriber.Connect(endpoint);
        subscriber.SetSubscription(topic);
        Thread.Sleep(100);

        using Message message = Message.From("direct-payload");
        Assert.True(publisher.Publish(topic).Message(message).Submit());
        Assert.Equal("direct-payload", CoreTestSupport.SubscribeUtf8WithTimeout(
            subscriber, out string receivedTopic, 2000));
        Assert.Equal(topic, receivedTopic);
    }

    [Fact]
    public void pubsub_single_part_messages_do_not_alias_after_double_dispose()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;
        if (!CoreTestSupport.IsTransportSupported("inproc"))
            return;

        using var ctx = Zlink.CreateContext();
        using var publisher = ctx.CreatePubSocket();
        using var subscriber = ctx.CreateSubSocket();

        string endpoint = CoreTestSupport.NewEndpoint("inproc",
            "pubsub-message-pool");
        publisher.Bind(endpoint);
        subscriber.Connect(endpoint);
        subscriber.SetSubscription(string.Empty);
        Thread.Sleep(100);

        CoreTestSupport.PublishWithRetry(publisher, string.Empty, "first"u8,
            2000);
        TopicMessage first = SubscribeMessageWithTimeout(subscriber, 2000);
        Message firstPart = first.SinglePartOrThrow();
        Assert.Equal("first", firstPart.GetString());
        firstPart.Dispose();
        first.Dispose();

        CoreTestSupport.PublishWithRetry(publisher, string.Empty, "second"u8,
            2000);
        TopicMessage second = SubscribeMessageWithTimeout(subscriber, 2000);
        try
        {
            CoreTestSupport.PublishWithRetry(publisher, string.Empty,
                "third"u8, 2000);
            using TopicMessage third = SubscribeMessageWithTimeout(subscriber,
                2000);

            Assert.NotSame(second.SinglePartOrThrow(),
                third.SinglePartOrThrow());
            Assert.Equal("second", second.SinglePartOrThrow().GetString());
            Assert.Equal("third", third.SinglePartOrThrow().GetString());
        }
        finally
        {
            second.Dispose();
        }
    }

    private static TopicMessage SubscribeMessageWithTimeout(
        ISubscriberSocket socket, int timeoutMs)
    {
        DateTime deadline = DateTime.UtcNow.AddMilliseconds(timeoutMs);
        var subscribed = new TopicMessage();
        while (DateTime.UtcNow < deadline)
        {
            if (socket.Subscribe(subscribed, RecvFlags.DontWait))
                return subscribed;
            Thread.Sleep(10);
        }

        subscribed.Dispose();
        throw new TimeoutException("subscribe timeout");
    }
}
