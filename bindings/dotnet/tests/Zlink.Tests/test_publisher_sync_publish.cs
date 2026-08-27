using System.Diagnostics;
using System.Text;
using Xunit;

namespace Systems.Zlink.Tests;

/// <summary>
///     PUB/XPUB publish is synchronous-only. Default PUB semantics are lossy —
///     a subscriber at its high-water mark has its copy dropped and the
///     publisher proceeds — so publish never waits and never parks a record.
///     With NODROP the full subscriber surfaces immediately as
///     <see cref="ZlinkSubmitException" /> and the retry policy is the
///     application's.
/// </summary>
public sealed class test_publisher_sync_publish
{
    private const ulong RecordHwm = 65_536UL + 1_024UL;
    private static readonly byte[] LargePayload = new byte[65_536];

    [Fact]
    public void publish_terminal_is_synchronous_void_submit()
    {
        if (!CoreTestSupport.IsNativeAvailable()) return;

        using var context = Zlink.CreateContext();
        context.Options.AutoHwmEnabled = false;
        using var publisher = context.CreateXPubSocket();
        using var subscriber = context.CreateSubSocket();
        Configure(publisher, subscriber);
        string endpoint = CoreTestSupport.NewEndpoint(
            "inproc", "publisher-sync-fresh");
        publisher.Bind(endpoint);
        ConnectSubscription(publisher, subscriber, endpoint, "fresh");

        using (Message fresh = Message.From("fresh-payload"))
            publisher.Publish("fresh").Message(fresh).Submit();

        Assert.Equal("fresh-payload",
            CoreTestSupport.SubscribeUtf8WithTimeout(
                subscriber, out string topic, 2_000));
        Assert.Equal("fresh", topic);
    }

    [Fact]
    public void nodrop_backpressure_surfaces_immediately_on_submit()
    {
        if (!CoreTestSupport.IsNativeAvailable()) return;

        using var context = Zlink.CreateContext();
        context.Options.AutoHwmEnabled = false;
        using var publisher = context.CreateXPubSocket();
        using var subscriber = context.CreateSubSocket();
        Configure(publisher, subscriber);
        string endpoint = CoreTestSupport.NewEndpoint(
            "inproc", "publisher-sync-nodrop");
        publisher.Bind(endpoint);
        ConnectSubscription(publisher, subscriber, endpoint, "full");
        FillTarget(publisher, "full");

        using Message blocked = Message.From(LargePayload);
        var started = Stopwatch.StartNew();
        ZlinkSubmitException error = Assert.Throws<ZlinkSubmitException>(() =>
            publisher.Publish("full").Message(blocked).Submit());
        started.Stop();

        Assert.Equal(ZlinkSubmitException.ErrorCode.Backpressured,
            error.Result);
        Assert.Throws<ObjectDisposedException>(() => _ = blocked.Size);
        // The publisher must not have waited for credit.
        Assert.True(started.Elapsed < TimeSpan.FromMilliseconds(250));
    }

    [Fact]
    public void try_publish_reports_backpressure_without_throwing()
    {
        if (!CoreTestSupport.IsNativeAvailable()) return;

        using var context = Zlink.CreateContext();
        context.Options.AutoHwmEnabled = false;
        using var publisher = context.CreateXPubSocket();
        using var subscriber = context.CreateSubSocket();
        Configure(publisher, subscriber);
        string endpoint = CoreTestSupport.NewEndpoint(
            "inproc", "publisher-sync-try");
        publisher.Bind(endpoint);
        ConnectSubscription(publisher, subscriber, endpoint, "try");
        FillTarget(publisher, "try");

        using Message blocked = Message.From(LargePayload);
        var started = Stopwatch.StartNew();
        bool accepted = publisher.TryPublish("try").Message(blocked).Submit();
        started.Stop();

        Assert.False(accepted);
        Assert.Throws<ObjectDisposedException>(() => _ = blocked.Size);
        Assert.True(started.Elapsed < TimeSpan.FromMilliseconds(250));
    }

    [Fact]
    public void lossy_publisher_never_waits_on_a_full_subscriber()
    {
        if (!CoreTestSupport.IsNativeAvailable()) return;

        using var context = Zlink.CreateContext();
        context.Options.AutoHwmEnabled = false;
        using var publisher = context.CreateXPubSocket();
        using var subscriber = context.CreateSubSocket();
        publisher.Options.NoDrop = false;
        publisher.Options.SendHighWaterMark = RecordHwm;
        subscriber.Options.ReceiveHighWaterMark = RecordHwm;
        string endpoint = CoreTestSupport.NewEndpoint(
            "inproc", "publisher-sync-lossy");
        publisher.Bind(endpoint);
        ConnectSubscription(publisher, subscriber, endpoint, "lossy");

        var started = Stopwatch.StartNew();
        for (var attempt = 0; attempt < 64; attempt++)
        {
            using Message filler = Message.From(LargePayload);
            publisher.Publish("lossy").Message(filler).Submit();
        }

        started.Stop();
        Assert.True(started.Elapsed < TimeSpan.FromSeconds(2));
    }

    private static void Configure(
        IXPubSocket publisher,
        ISubSocket subscriber)
    {
        publisher.Options.NoDrop = true;
        publisher.Options.SendHighWaterMark = RecordHwm;
        subscriber.Options.ReceiveHighWaterMark = RecordHwm;
    }

    private static void ConnectSubscription(
        IXPubSocket publisher,
        ISubSocket subscriber,
        string endpoint,
        string topic)
    {
        subscriber.Connect(endpoint);
        subscriber.SetSubscription(topic);
        byte[] subscribed = CoreTestSupport
            .ReceiveSubscriptionEventWithTimeout(
                publisher, out bool active, 2_000);
        Assert.True(active);
        Assert.Equal(topic, Encoding.UTF8.GetString(subscribed));
    }

    private static void FillTarget(IXPubSocket publisher, string topic)
    {
        for (var attempt = 0; attempt < 64; attempt++)
        {
            using Message filler = Message.From(LargePayload);
            if (!publisher.TryPublish(topic).Message(filler).Submit())
                return;
        }

        throw new Xunit.Sdk.XunitException(
            $"Publisher target '{topic}' did not reach back-pressure.");
    }
}
