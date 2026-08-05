using System;
using System.Threading;
using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_xpub_verbose
{
    private static readonly byte[] TopicA = { (byte)'A' };
    private static readonly byte[] TopicB = { (byte)'B' };

    [Fact]
    public void xpub_verbose_one_subscriber_duplicate_subscription_visible()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var xpub = ctx.CreateXPubSocket();
        using var sub = ctx.CreateSubSocket();

        string endpoint = CoreTestSupport.NewEndpoint("inproc", "xpub-verbose");
        xpub.Bind(endpoint);
        sub.Connect(endpoint);

        sub.SetSubscription("A");
        Assert.Equal(TopicA,
            CoreTestSupport.ReceiveSubscriptionEventWithTimeout(xpub,
                out bool subscribedA0, 2000));
        Assert.True(subscribedA0);

        sub.SetSubscription("B");
        Assert.Equal(TopicB,
            CoreTestSupport.ReceiveSubscriptionEventWithTimeout(xpub,
                out bool subscribedB0, 2000));
        Assert.True(subscribedB0);

        sub.SetSubscription("A");
        Thread.Sleep(30);
        Assert.True(CoreTestSupport.ExpectNoSubscriptionEvent(xpub, 150));

        xpub.Options.Verbose = true;
        sub.SetSubscription("A");
        Assert.Equal(TopicA,
            CoreTestSupport.ReceiveSubscriptionEventWithTimeout(xpub,
                out bool subscribedA1, 2000));
        Assert.True(subscribedA1);

        CoreTestSupport.PublishWithRetry(xpub, "A", ReadOnlySpan<byte>.Empty,
            2000);
        CoreTestSupport.PublishWithRetry(xpub, "B", ReadOnlySpan<byte>.Empty,
            2000);

        Assert.Equal(string.Empty, CoreTestSupport.SubscribeUtf8WithTimeout(sub,
            out string topicA, 2000));
        Assert.Equal("A", topicA);
        Assert.Equal(string.Empty, CoreTestSupport.SubscribeUtf8WithTimeout(sub,
            out string topicB, 2000));
        Assert.Equal("B", topicB);
    }

    [Fact]
    public void xpub_verbose_two_subscribers_duplicate_subscription_visible()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var xpub = ctx.CreateXPubSocket();
        using var sub0 = ctx.CreateSubSocket();
        using var sub1 = ctx.CreateSubSocket();

        string endpoint = CoreTestSupport.NewEndpoint("inproc", "xpub-two");
        xpub.Bind(endpoint);
        sub0.Connect(endpoint);
        sub1.Connect(endpoint);

        sub0.SetSubscription("A");
        Assert.Equal(TopicA,
            CoreTestSupport.ReceiveSubscriptionEventWithTimeout(xpub,
                out bool subscribedA0, 2000));
        Assert.True(subscribedA0);

        sub1.SetSubscription("A");
        Assert.True(CoreTestSupport.ExpectNoSubscriptionEvent(xpub, 150));

        sub0.SetSubscription("B");
        Assert.Equal(TopicB,
            CoreTestSupport.ReceiveSubscriptionEventWithTimeout(xpub,
                out bool subscribedB0, 2000));
        Assert.True(subscribedB0);

        xpub.Options.Verbose = true;
        sub1.SetSubscription("A");
        Assert.Equal(TopicA,
            CoreTestSupport.ReceiveSubscriptionEventWithTimeout(xpub,
                out bool subscribedA1, 2000));
        Assert.True(subscribedA1);

        CoreTestSupport.PublishWithRetry(xpub, "A", ReadOnlySpan<byte>.Empty,
            2000);
        CoreTestSupport.PublishWithRetry(xpub, "B", ReadOnlySpan<byte>.Empty,
            2000);

        Assert.Equal(string.Empty, CoreTestSupport.SubscribeUtf8WithTimeout(sub0,
            out string topicA0, 2000));
        Assert.Equal("A", topicA0);
        Assert.Equal(string.Empty, CoreTestSupport.SubscribeUtf8WithTimeout(sub1,
            out string topicA1, 2000));
        Assert.Equal("A", topicA1);
        Assert.Equal(string.Empty, CoreTestSupport.SubscribeUtf8WithTimeout(sub0,
            out string topicB0, 2000));
        Assert.Equal("B", topicB0);
    }

    [Fact]
    public void xpub_verboser_one_subscriber_unsubscribe_visibility()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var xpub = ctx.CreateXPubSocket();
        using var sub = ctx.CreateSubSocket();

        string endpoint = CoreTestSupport.NewEndpoint("inproc", "xpub-verboser1");
        xpub.Bind(endpoint);
        sub.Connect(endpoint);

        sub.UnsetSubscription("A");
        Assert.True(CoreTestSupport.ExpectNoSubscriptionEvent(xpub, 100));

        sub.SetSubscription("A");
        Assert.Equal(TopicA,
            CoreTestSupport.ReceiveSubscriptionEventWithTimeout(xpub,
                out bool subscribedA0, 2000));
        Assert.True(subscribedA0);

        sub.SetSubscription("A");
        Assert.True(CoreTestSupport.ExpectNoSubscriptionEvent(xpub, 100));

        sub.UnsetSubscription("A");
        sub.UnsetSubscription("A");
        Assert.Equal(TopicA,
            CoreTestSupport.ReceiveSubscriptionEventWithTimeout(xpub,
                out bool subscribedA1, 2000));
        Assert.False(subscribedA1);
        Assert.True(CoreTestSupport.ExpectNoSubscriptionEvent(xpub, 100));

        xpub.Options.Verboser = true;
        sub.SetSubscription("A");
        Assert.Equal(TopicA,
            CoreTestSupport.ReceiveSubscriptionEventWithTimeout(xpub,
                out bool subscribedA2, 2000));
        Assert.True(subscribedA2);

        sub.UnsetSubscription("A");
        Assert.Equal(TopicA,
            CoreTestSupport.ReceiveSubscriptionEventWithTimeout(xpub,
                out bool subscribedA3, 2000));
        Assert.False(subscribedA3);
    }

    [Fact]
    public void xpub_verboser_two_subscribers_unsubscribe_visibility()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var xpub = ctx.CreateXPubSocket();
        using var sub0 = ctx.CreateSubSocket();
        using var sub1 = ctx.CreateSubSocket();

        string endpoint = CoreTestSupport.NewEndpoint("inproc", "xpub-verboser2");
        xpub.Bind(endpoint);
        sub0.Connect(endpoint);
        sub1.Connect(endpoint);

        sub0.SetSubscription("A");
        Assert.Equal(TopicA,
            CoreTestSupport.ReceiveSubscriptionEventWithTimeout(xpub,
                out bool subscribedA0, 2000));
        Assert.True(subscribedA0);

        sub1.SetSubscription("A");
        Assert.True(CoreTestSupport.ExpectNoSubscriptionEvent(xpub, 150));

        sub0.UnsetSubscription("A");
        Assert.True(CoreTestSupport.ExpectNoSubscriptionEvent(xpub, 150));

        sub1.UnsetSubscription("A");
        Assert.Equal(TopicA,
            CoreTestSupport.ReceiveSubscriptionEventWithTimeout(xpub,
                out bool subscribedA1, 2000));
        Assert.False(subscribedA1);
        Assert.True(CoreTestSupport.ExpectNoSubscriptionEvent(xpub, 100));

        xpub.Options.Verboser = true;
        sub0.SetSubscription("A");
        Assert.Equal(TopicA,
            CoreTestSupport.ReceiveSubscriptionEventWithTimeout(xpub,
                out bool subscribedA2, 2000));
        Assert.True(subscribedA2);
        sub1.SetSubscription("A");
        Assert.Equal(TopicA,
            CoreTestSupport.ReceiveSubscriptionEventWithTimeout(xpub,
                out bool subscribedA3, 2000));
        Assert.True(subscribedA3);

        sub1.UnsetSubscription("A");
        Assert.Equal(TopicA,
            CoreTestSupport.ReceiveSubscriptionEventWithTimeout(xpub,
                out bool subscribedA4, 2000));
        Assert.False(subscribedA4);
        sub0.UnsetSubscription("A");
        Assert.Equal(TopicA,
            CoreTestSupport.ReceiveSubscriptionEventWithTimeout(xpub,
                out bool subscribedA5, 2000));
        Assert.False(subscribedA5);
    }
}
