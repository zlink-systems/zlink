using System.Diagnostics;
using System.Text;
using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.AspNetCore;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class FanoutNoDropTests
{
    private const string Topic = "fanout.nodrop";
    private const int FillerCount = 64;
    private static readonly byte[] FillerPayload = CreatePayload(0x11);
    private static readonly byte[] TargetPayload = CreatePayload(0x22);
    private static readonly ulong RecordHwm = checked(
        (ulong)(FillerPayload.Length + Encoding.UTF8.GetByteCount(Topic)));

    [Fact]
    public void DefaultNoDrop_DropsOnlySlowSubscriberRecord_AndPublishSucceeds()
    {
        using var pair = new PublisherPair(noDrop: null, TimeSpan.FromSeconds(2));

        for (var index = 0; index < FillerCount; index++)
        {
            pair.Publish(FillerPayload);
            pair.AssertReceives(pair.FastSubscriber, 0x11);
        }

        pair.Publish(TargetPayload);
        pair.AssertReceives(pair.FastSubscriber, 0x22);

        Assert.False(pair.Publisher.Options.NoDrop);
        Assert.DoesNotContain(
            pair.DrainMarkers(pair.SlowSubscriber),
            static marker => marker == 0x22);
    }

    [Fact]
    public async Task NoDrop_WaitsWithoutPartialDelivery_ThenAllSubscribersReceive()
    {
        using var pair = new PublisherPair(noDrop: true, TimeSpan.FromSeconds(2));
        pair.FillSlowSubscriberQueue();

        Assert.False(pair.TryPublish(TargetPayload));
        pair.AssertNoMessage(pair.FastSubscriber);

        var publishTask = Task.Run(() => pair.Publish(TargetPayload));
        await Task.Delay(TimeSpan.FromMilliseconds(50));
        if (publishTask.IsCompleted)
            await publishTask;

        pair.AssertNoMessage(pair.FastSubscriber);
        pair.DrainOne(pair.SlowSubscriber);

        await publishTask.WaitAsync(TimeSpan.FromSeconds(2));
        pair.AssertReceives(pair.FastSubscriber, 0x22);
        pair.AssertEventuallyReceives(pair.SlowSubscriber, 0x22);
    }

    [Fact]
    public void NoDrop_SendTimeoutMapsToDeadlineExceeded()
    {
        var sendTimeout = TimeSpan.FromMilliseconds(100);
        using var pair = new PublisherPair(noDrop: true, sendTimeout);
        pair.FillSlowSubscriberQueue();
        var started = Stopwatch.GetTimestamp();

        var bindingFailure = Assert.Throws<ZlinkSubmitException>(() =>
            pair.Publish(TargetPayload));
        var elapsed = Stopwatch.GetElapsedTime(started);
        var mapped = Assert.IsType<ZLinkFrameworkException>(
            ZLinkRequestFailureMapper.CreateSubmitException(
                bindingFailure, "Fanout publish"));

        Assert.Equal(ZLinkFrameworkErrorKind.DeadlineExceeded, mapped.Kind);
        Assert.True(
            elapsed >= TimeSpan.FromMilliseconds(75),
            $"Publish returned after {elapsed.TotalMilliseconds:F1} ms instead of waiting for the {sendTimeout.TotalMilliseconds:F0} ms send timeout.");
    }

    [Fact]
    public void NoDropWithoutPublisher_FailsStartupValidation()
    {
        var services = new ServiceCollection();

        var error = Assert.Throws<ZLinkConfigurationException>(() =>
            services.AddZLinkFramework(options =>
            {
                options.AddFanoutChannel("events")
                    .SetNoDrop()
                    .Connect("inproc://fanout-nodrop-subscriber")
                    .AddHandler<TestFanoutHandler, TestFanoutEvent>();
            }));

        Assert.Contains("NoDrop", error.Message, StringComparison.Ordinal);
        Assert.Contains("without publisher", error.Message, StringComparison.Ordinal);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void BuilderNoDrop_IsAppliedToPublisherSocketOption(bool noDrop)
    {
        var registration = CreateRegistration(noDrop);
        using var context = Systems.Zlink.Zlink.CreateContext();
        using var publisher = context.CreatePubSocket();

        ZLinkChannelBundleFactory.ApplyPublisherSocketConfig(
            publisher.Options,
            registration.Channels["events"]);

        Assert.Equal(noDrop, publisher.Options.NoDrop);
    }

    private static ZLinkFrameworkRegistration CreateRegistration(bool? noDrop)
    {
        var registration = new ZLinkFrameworkRegistration();
        var fanout = new ZLinkFrameworkOptionsBuilder(registration)
            .AddFanoutChannel("events")
            .EnablePublisher("inproc://fanout-nodrop-unused");
        if (noDrop.HasValue)
            fanout.SetNoDrop(noDrop.Value);
        ZLinkFrameworkRegistrationValidator.Validate(registration);
        return registration;
    }

    private static byte[] CreatePayload(byte marker)
    {
        var payload = new byte[65_536];
        payload[0] = marker;
        return payload;
    }

    private sealed record TestFanoutEvent;

    private sealed class TestFanoutHandler : IZLinkFanoutHandler<TestFanoutEvent>
    {
        public ValueTask HandleAsync(
            TestFanoutEvent message,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            return ValueTask.CompletedTask;
        }
    }

    private sealed class PublisherPair : IDisposable
    {
        private readonly IContext _context = Systems.Zlink.Zlink.CreateContext();
        internal readonly IXPubSocket Publisher;
        internal readonly ISubSocket FastSubscriber;
        internal readonly ISubSocket SlowSubscriber;

        internal PublisherPair(bool? noDrop, TimeSpan sendTimeout)
        {
            _context.Options.AutoHwmEnabled = false;
            Publisher = _context.CreateXPubSocket();
            FastSubscriber = _context.CreateSubSocket();
            SlowSubscriber = _context.CreateSubSocket();

            var registration = CreateRegistration(noDrop);
            registration.Channels["events"].Publisher!.SocketConfig.SendTimeout =
                sendTimeout;
            ZLinkChannelBundleFactory.ApplyPublisherSocketConfig(
                Publisher.Options,
                registration.Channels["events"]);
            Assert.Equal(sendTimeout, Publisher.Options.SendTimeout);
            Assert.Equal(noDrop ?? false, Publisher.Options.NoDrop);
            Publisher.Options.Verbose = true;
            Publisher.Options.SendHighWaterMark = RecordHwm;
            Publisher.Options.ReceiveTimeout = TimeSpan.FromSeconds(2);
            ConfigureSubscriber(FastSubscriber);
            ConfigureSubscriber(SlowSubscriber);

            var endpoint = $"inproc://framework-fanout-nodrop-{Guid.NewGuid():N}";
            Publisher.Bind(endpoint);
            FastSubscriber.Connect(endpoint);
            SlowSubscriber.Connect(endpoint);
            FastSubscriber.SetSubscription(Topic);
            SlowSubscriber.SetSubscription(Topic);
            AssertSubscription();
            AssertSubscription();
        }

        internal void FillSlowSubscriberQueue()
        {
            for (var attempt = 0; attempt < FillerCount; attempt++)
            {
                if (!TryPublish(FillerPayload))
                {
                    AssertNoMessage(FastSubscriber);
                    return;
                }

                AssertReceives(FastSubscriber, 0x11);
            }

            throw new Xunit.Sdk.XunitException(
                "Publisher queue did not report backpressure at its configured HWM.");
        }

        internal void Publish(byte[] payload)
        {
            using Message message = Message.From(payload);
            Publisher.Publish(Topic).Message(message).Flags(SendFlags.None).Submit();
        }

        internal bool TryPublish(byte[] payload)
        {
            using Message message = Message.From(payload);
            return Publisher.TryPublish(Topic).Message(message).Submit();
        }

        internal void AssertReceives(ISubSocket subscriber, byte marker)
        {
            using var received = new TopicMessage();
            Assert.True(subscriber.Subscribe(received));
            Assert.Equal(Topic, received.Topic);
            Assert.Equal(marker, received.SinglePartOrThrow().AsReadOnlySpan()[0]);
        }

        internal void AssertEventuallyReceives(ISubSocket subscriber, byte marker)
        {
            for (var attempt = 0; attempt < FillerCount + 1; attempt++)
            {
                using var received = new TopicMessage();
                Assert.True(subscriber.Subscribe(received));
                if (received.SinglePartOrThrow().AsReadOnlySpan()[0] == marker)
                    return;
            }

            throw new Xunit.Sdk.XunitException(
                $"Subscriber did not receive marker 0x{marker:X2}.");
        }

        internal IReadOnlyList<byte> DrainMarkers(ISubSocket subscriber)
        {
            var markers = new List<byte>();
            while (true)
            {
                using var received = new TopicMessage();
                if (!subscriber.Subscribe(received, RecvFlags.DontWait))
                    return markers;
                markers.Add(received.SinglePartOrThrow().AsReadOnlySpan()[0]);
            }
        }

        internal void DrainOne(ISubSocket subscriber)
        {
            using var received = new TopicMessage();
            Assert.True(subscriber.Subscribe(received));
        }

        internal void AssertNoMessage(ISubSocket subscriber)
        {
            using var received = new TopicMessage();
            Assert.False(subscriber.Subscribe(received, RecvFlags.DontWait));
        }

        private void ConfigureSubscriber(ISubSocket subscriber)
        {
            subscriber.Options.ReceiveHighWaterMark = RecordHwm;
            subscriber.Options.ReceiveTimeout = TimeSpan.FromSeconds(2);
        }

        private void AssertSubscription()
        {
            var subscription = new SubscriptionEvent();
            Assert.True(Publisher.ReceiveSubscriptionEvent(subscription));
            Assert.True(subscription.Subscribed);
            Assert.Equal(Topic, subscription.Topic);
        }

        public void Dispose()
        {
            SlowSubscriber.Dispose();
            FastSubscriber.Dispose();
            Publisher.Dispose();
            _context.Dispose();
        }
    }
}
