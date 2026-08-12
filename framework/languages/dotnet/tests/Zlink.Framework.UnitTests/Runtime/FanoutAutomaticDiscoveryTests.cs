using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Channels;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.LocationProvider;

namespace Zlink.Framework.UnitTests;

public sealed class FanoutAutomaticDiscoveryTests
    : RegistrationValidationSupport
{
    [Fact]
    public void Builder_SeparatesAutomaticAndManualSubscriberModes()
    {
        var withoutStore = new ServiceCollection();
        var missingStore = Assert.Throws<ZLinkConfigurationException>(() =>
            withoutStore.AddZLinkFramework(options =>
            {
                options.AddFanoutChannel("events")
                    .EnableSubscriber()
                    .AddHandler<TestPublishHandler, TestPublishedEvent>();
            }));
        Assert.Contains("requires a location store", missingStore.Message);

        var mixed = new ServiceCollection();
        var mixedMode = Assert.Throws<ZLinkConfigurationException>(() =>
            mixed.AddZLinkFramework(options =>
            {
                options.AddLocationStore(new ZLinkInMemoryProviderLocationStore());
                options.AddFanoutChannel("events")
                    .EnableSubscriber()
                    .Connect("tcp://127.0.0.1:7001")
                    .AddHandler<TestPublishHandler, TestPublishedEvent>();
            }));
        Assert.Contains("cannot combine automatic and manual", mixedMode.Message);
    }

    [Fact]
    public void SubscriberConnections_OwnsOnlyManualEndpointSet()
    {
        var services = new ServiceCollection();
        IZLinkEndpointConnections? connections = null;

        services.AddZLinkFramework(options =>
        {
            var fanout = options.AddFanoutChannel("events");
            fanout.Connect("tcp://127.0.0.1:7001")
                .AddHandler<TestPublishHandler, TestPublishedEvent>();
            connections = fanout.SubscriberConnections;
        });

        Assert.NotNull(connections);
        Assert.Equal(
            ["tcp://127.0.0.1:7001"],
            connections.ListConnections());
        connections.Connect("tcp://127.0.0.1:7002");
        Assert.Equal(2, connections.ListConnections().Count);
        connections.Disconnect("tcp://127.0.0.1:7001");
        Assert.Equal(
            ["tcp://127.0.0.1:7002"],
            connections.ListConnections());
    }

    [Fact]
    public async Task InMemoryStore_FencesAndListsDedicatedPublisherRows()
    {
        var store = new ZLinkInMemoryLocationStore();
        var claim = Assert.IsType<ZLinkOwnerLeaseClaimResult.Claimed>(
            await store.ClaimOwnerLeaseAsync(
            "owner-a",
            TimeSpan.FromMinutes(1)));
        var owner = claim.Token;
        var descriptor = new ZLinkFanoutPublisherDescriptor(
            "events",
            RoutingId.From("publisher-a"),
            7,
            1,
            "tcp://127.0.0.1:7001",
            ZLinkFrameworkRuntimeState.Serving,
            "plaintext",
            owner.OwnerId,
            owner.LeaseGeneration,
            default);

        var stored = await store.UpdateFanoutPublisherAsync(
            descriptor,
            ZLinkLocationWriteIntent.NewClaim);
        Assert.Equal(ZLinkLocationWriteStatus.Stored, stored.Status);
        var page = await store.ListFanoutPublishersAsync(
            "events",
            new ZLinkPageRequest(10));
        var row = Assert.Single(page.Items);
        Assert.Equal(descriptor.PublisherRid, row.PublisherRid);
        Assert.Equal(descriptor.LifecycleGeneration, row.LifecycleGeneration);

        var staleRenew = await store.UpdateFanoutPublisherAsync(
            descriptor with { DescriptorRevision = 1 },
            ZLinkLocationWriteIntent.Renew);
        Assert.Equal(
            ZLinkLocationWriteStatus.IgnoredStale,
            staleRenew.Status);
        Assert.Equal(
            ZLinkLocationWriteStatus.Stored,
            await store.RemoveFanoutPublisherAsync(
                new ZLinkFanoutPublisherDescriptorKey(
                    descriptor.ChannelName,
                    descriptor.PublisherRid),
                owner));
    }

    [Fact]
    public void RuntimeSnapshot_RejectsManualChannelAndPreservesIdentity()
    {
        var registration = new ZLinkFrameworkRegistration();
        registration.Channels.Add(
            "automatic",
            new ZLinkChannelRegistration
            {
                ChannelName = "automatic",
                AutoConnectType = ZLinkLocationAutoConnectType.Fanout,
                Subscriber = new ZLinkChannelSubscriberCapabilityRegistration
                {
                    AutomaticDiscoveryEnabled = true
                }
            });
        registration.Channels.Add(
            "manual",
            new ZLinkChannelRegistration
            {
                ChannelName = "manual",
                AutoConnectType = ZLinkLocationAutoConnectType.Fanout,
                Subscriber = new ZLinkChannelSubscriberCapabilityRegistration()
            });
        var hostLifecycle = new ZLinkFrameworkHostLifecycleState();
        hostLifecycle.TransitionTo(ZLinkFrameworkRuntimeState.Serving);
        var runtime = new ZLinkFanoutRuntimeService(registration, hostLifecycle);
        var entry = new ZLinkFanoutPublisherConnectionSnapshot(
            RoutingId.From("publisher-a"),
            9,
            3,
            "tcp://127.0.0.1:7001",
            true,
            false,
            ZLinkFanoutPublisherConnectionState.Connecting,
            null);

        runtime.RecordSnapshot(
            "automatic",
            [entry],
            new ZLinkLocationRuntimeSnapshot(
                "healthy",
                DateTimeOffset.UtcNow,
                null));

        var snapshot = runtime.GetStatus("automatic");
        Assert.Equal(0, snapshot.ReadyPublisherCount);
        Assert.Equal(
            ZLinkPeerState.Connecting,
            Assert.Single(snapshot.Publishers).State);
        Assert.Throws<ZLinkConfigurationException>(() =>
            runtime.GetStatus("manual"));

        hostLifecycle.TransitionTo(ZLinkFrameworkRuntimeState.Relocating);
        var relocating = runtime.GetStatus("automatic");
        Assert.False(relocating.IsReady);
        Assert.Equal(ZLinkTopologyState.Stopping, relocating.State);
    }

    [Fact]
    public async Task RuntimeObserverRetainsRemovalSnapshotUntilItsSourceSlotIsDelivered()
    {
        var registration = new ZLinkFrameworkRegistration();
        registration.Channels.Add(
            "automatic",
            new ZLinkChannelRegistration
            {
                ChannelName = "automatic",
                AutoConnectType = ZLinkLocationAutoConnectType.Fanout,
                Subscriber = new ZLinkChannelSubscriberCapabilityRegistration
                {
                    AutomaticDiscoveryEnabled = true
                }
            });
        var hostLifecycle = new ZLinkFrameworkHostLifecycleState();
        hostLifecycle.TransitionTo(ZLinkFrameworkRuntimeState.Serving);
        using var runtime = new ZLinkFanoutRuntimeService(
            registration,
            hostLifecycle);
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        await using var observer = runtime.ObserveAsync(
                "automatic",
                timeout.Token)
            .GetAsyncEnumerator(timeout.Token);
        var pendingInitial = observer.MoveNextAsync().AsTask();
        var location = new ZLinkLocationRuntimeSnapshot(
            "unknown",
            null,
            null);
        var source = new ZLinkFanoutPublisherConnectionSnapshot(
            RoutingId.From("publisher-a"),
            9,
            1,
            "tcp://127.0.0.1:7001",
            ConnectionIntent: true,
            Ready: false,
            ZLinkFanoutPublisherConnectionState.Connecting,
            LastFailure: null);

        runtime.RecordSnapshot("automatic", [source], location);
        Assert.True(await pendingInitial.WaitAsync(TimeSpan.FromSeconds(5)));
        Assert.Equal(
            ZLinkPeerState.Connecting,
            Assert.Single(observer.Current.Status.Publishers).State);

        runtime.RecordSnapshot("automatic", [], location);
        runtime.RecordSnapshot(
            "automatic",
            [source with
            {
                DescriptorRevision = 2,
                Ready = true,
                State = ZLinkFanoutPublisherConnectionState.Ready
            }],
            location);

        Assert.True(await observer.MoveNextAsync().AsTask()
            .WaitAsync(TimeSpan.FromSeconds(5)));
        Assert.Empty(observer.Current.Status.Publishers);
        Assert.Equal(1UL, observer.Current.Loss.CoalescedCount);

        var pendingRestart = observer.MoveNextAsync().AsTask();
        runtime.RecordSnapshot(
            "automatic",
            [source with
            {
                DescriptorRevision = 3,
                State = ZLinkFanoutPublisherConnectionState.Reconnecting
            }],
            location);
        Assert.True(await pendingRestart.WaitAsync(TimeSpan.FromSeconds(5)));
        Assert.Equal(
            ZLinkPeerState.Connecting,
            Assert.Single(observer.Current.Status.Publishers).State);
    }

    [Fact]
    public void LivenessProtocol_RecognizesOnlyExactTopicAndPayload()
    {
        using var valid = Message.From(
            ZLinkFanoutLivenessProtocol.Payload);
        using var invalid = Message.From([0x5A, 0x46, 0x01, 0x02]);

        Assert.True(ZLinkFanoutLivenessProtocol.IsValidBeacon(
            ZLinkFanoutLivenessProtocol.Topic,
            [valid]));
        Assert.False(ZLinkFanoutLivenessProtocol.IsReservedTopic(
            ZLinkFanoutLivenessProtocol.Topic + "-application"));
        Assert.False(ZLinkFanoutLivenessProtocol.IsValidBeacon(
            ZLinkFanoutLivenessProtocol.Topic,
            [invalid]));
        Assert.False(ZLinkFanoutLivenessProtocol.IsValidBeacon(
            ZLinkFanoutLivenessProtocol.Topic,
            [valid, invalid]));

        var lastActivity = DateTimeOffset.UtcNow;
        Assert.False(ZLinkFanoutLivenessProtocol.IsInboundTimedOut(
            lastActivity,
            lastActivity + TimeSpan.FromSeconds(14.999)));
        Assert.True(ZLinkFanoutLivenessProtocol.IsInboundTimedOut(
            lastActivity,
            lastActivity + TimeSpan.FromSeconds(15)));
    }

    [Fact]
    public async Task AutomaticRuntime_OwnsOneSocketPerPublisherIdentity()
    {
        var registration = new ZLinkFrameworkRegistration();
        registration.Channels.Add(
            "events",
            new ZLinkChannelRegistration
            {
                ChannelName = "events",
                AutoConnectType = ZLinkLocationAutoConnectType.Fanout,
                Subscriber = new ZLinkChannelSubscriberCapabilityRegistration
                {
                    AutomaticDiscoveryEnabled = true
                }
            });
        var hostLifecycle = new ZLinkFrameworkHostLifecycleState();
        hostLifecycle.TransitionTo(ZLinkFrameworkRuntimeState.Serving);
        var monitoring = new ZLinkFanoutRuntimeService(
            registration,
            hostLifecycle);
        var factory = new ZLinkDotNetBackendAdapterFactory();
        using var failureSink = new ZLinkRuntimeErrorSink();
        await using var context = factory.CreateRuntimeContext();
        await using var runtime = new ZLinkAutomaticFanoutSubscriberRuntime(
            "events",
            context,
            new ZLinkSocketConfig(),
            new ZLinkChannelReceiveLoop(null!, null!),
            monitoring,
            failureSink,
            CancellationToken.None);
        var first = Descriptor("publisher-a", 1, "tcp://127.0.0.1:7001");
        var second = Descriptor("publisher-b", 2, "tcp://127.0.0.1:7002");
        var location = new ZLinkLocationRuntimeSnapshot(
            "healthy",
            DateTimeOffset.UtcNow,
            null);

        await runtime.ReplaceAsync(
            [
                new ZLinkFanoutConnectionPlan(
                    first,
                    true,
                    ZLinkFanoutPublisherConnectionState.Connecting),
                new ZLinkFanoutConnectionPlan(
                    second,
                    true,
                    ZLinkFanoutPublisherConnectionState.Connecting)
            ],
            location);
        Assert.True(SpinWait.SpinUntil(
            () => runtime.SocketCreationCount >= 2,
            TimeSpan.FromSeconds(1)));
        var count = runtime.SocketCreationCount;
        await runtime.ReplaceAsync(
            [
                new ZLinkFanoutConnectionPlan(
                    first with { DescriptorRevision = 2 },
                    true,
                    ZLinkFanoutPublisherConnectionState.Connecting),
                new ZLinkFanoutConnectionPlan(
                    second with { DescriptorRevision = 2 },
                    true,
                    ZLinkFanoutPublisherConnectionState.Connecting)
            ],
            location);
        await Task.Delay(50);
        Assert.Equal(count, runtime.SocketCreationCount);
    }

    private static ZLinkFanoutPublisherDescriptor Descriptor(
        string rid,
        ulong generation,
        string endpoint) =>
        new(
            "events",
            RoutingId.From(rid),
            generation,
            1,
            endpoint,
            ZLinkFrameworkRuntimeState.Serving,
            "plaintext",
            "owner",
            1,
            default);

}
