using Zlink.Framework.Runtime.Backend.DotNet.Wrappers;
using Zlink.Framework.Runtime.Configuration;
using Zlink.Framework.Runtime.Messaging;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class FanoutPublishAdmissionTests
{
    private const ulong RecordHwm = 65_536UL + 1_024UL;
    private static readonly byte[] LargePayload = new byte[65_536];

    [Fact]
    public void Publish_Succeeds_AfterQueueCapacityIsRestored()
    {
        using var pair = new PublisherPair(TimeSpan.FromSeconds(5));
        pair.FillLocalQueue();

        using (var received = new TopicMessage())
            Assert.True(pair.Subscriber.Subscribe(received));

        using Message pending = Message.From(LargePayload);
        pair.Publisher.Publish(pair.Topic).Message(pending).Submit();
    }

    [Fact]
    public void Publish_SendTimeoutMapsToDeadlineExceeded()
    {
        using var pair = new PublisherPair(TimeSpan.FromMilliseconds(25));
        pair.FillLocalQueue();
        using Message pending = Message.From(LargePayload);

        var bindingFailure = Assert.Throws<ZlinkSubmitException>(() =>
            pair.Publisher.Publish(pair.Topic).Message(pending).Submit());
        var mapped = Assert.IsType<ZLinkFrameworkException>(
            ZLinkRequestFailureMapper.CreateSubmitException(
                bindingFailure, "Fanout publish"));

        Assert.Equal(ZLinkFrameworkErrorKind.DeadlineExceeded, mapped.Kind);
    }

    private sealed class PublisherPair : IDisposable
    {
        private readonly IContext _context = Systems.Zlink.Zlink.CreateContext();
        internal readonly IXPubSocket Publisher;
        internal readonly ISubSocket Subscriber;
        internal string Topic { get; } = "fanout.admission";

        internal PublisherPair(TimeSpan sendTimeout)
        {
            _context.Options.AutoHwmEnabled = false;
            Publisher = _context.CreateXPubSocket();
            Subscriber = _context.CreateSubSocket();
            Publisher.Options.NoDrop = true;
            Publisher.Options.SendHighWaterMark = RecordHwm;
            ZLinkBackendSocketOptionsMapper.Apply(
                Publisher.Options,
                new ZLinkSocketConfig { SendTimeout = sendTimeout });
            Publisher.Options.ReceiveTimeout = TimeSpan.FromSeconds(5);
            Subscriber.Options.ReceiveHighWaterMark = RecordHwm;
            Subscriber.Options.ReceiveTimeout = TimeSpan.FromSeconds(5);

            var endpoint = $"inproc://framework-fanout-admission-{Guid.NewGuid():N}";
            Publisher.Bind(endpoint);
            Subscriber.Connect(endpoint);
            Subscriber.SetSubscription(Topic);
            var subscription = new SubscriptionEvent();
            Assert.True(Publisher.ReceiveSubscriptionEvent(subscription));
            Assert.True(subscription.Subscribed);
            Assert.Equal(Topic, subscription.Topic);
        }

        internal void FillLocalQueue()
        {
            for (var attempt = 0; attempt < 64; attempt++)
            {
                using Message filler = Message.From(LargePayload);
                if (!Publisher.TryPublish(Topic).Message(filler).Submit())
                    return;
            }

            throw new Xunit.Sdk.XunitException(
                "Publisher queue did not reach its configured HWM.");
        }

        public void Dispose()
        {
            Subscriber.Dispose();
            Publisher.Dispose();
            _context.Dispose();
        }
    }
}
