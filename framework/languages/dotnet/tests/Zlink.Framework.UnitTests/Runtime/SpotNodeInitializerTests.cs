using Zlink.Framework.Contracts.Locations;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests;

public sealed class SpotNodeInitializerTests
{
    private const string MeshName = "objects";
    private const string PeerEndpoint = "tcp://127.0.0.1:7199";

    [Fact]
    public async Task Initialization_Ignores_Expired_Owner_Descriptor_And_Adopts_Live_Peer()
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryProviderLocationStore(time);
        var repository = new ZLinkProviderLocationRepository(store);
        var expiredOwner = await repository.ClaimLiveOwnerAsync(
            "expired-owner",
            TimeSpan.FromSeconds(1));
        var liveOwner = await repository.ClaimLiveOwnerAsync(
            "live-owner",
            TimeSpan.FromMinutes(1));
        await PublishDescriptorAsync(
            repository,
            expiredOwner,
            RoutingId.From("aaa-expired"));
        await PublishDescriptorAsync(
            repository,
            liveOwner,
            RoutingId.From("zzz-live"));
        time.Advance(TimeSpan.FromSeconds(2));

        var peerRoutingIds = await ResolveAsync(store, time);

        Assert.Equal(
            RoutingId.From("zzz-live"),
            peerRoutingIds[PeerEndpoint]);
    }

    [Fact]
    public async Task Initialization_Adopts_Live_Owner_Descriptor()
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryProviderLocationStore(time);
        var repository = new ZLinkProviderLocationRepository(store);
        var liveOwner = await repository.ClaimLiveOwnerAsync(
            "live-owner",
            TimeSpan.FromMinutes(1));
        await PublishDescriptorAsync(
            repository,
            liveOwner,
            RoutingId.From("live-peer"));

        var peerRoutingIds = await ResolveAsync(store, time);

        Assert.Equal(
            RoutingId.From("live-peer"),
            peerRoutingIds[PeerEndpoint]);
    }

    [Fact]
    public async Task Initialization_Rejects_Owner_Lease_Without_Expiry()
    {
        var time = new ManualTimeProvider();
        var inner = new ZLinkInMemoryProviderLocationStore(time);
        var repository = new ZLinkProviderLocationRepository(inner);
        var owner = await repository.ClaimLiveOwnerAsync(
            "corrupt-owner",
            TimeSpan.FromMinutes(1));
        await PublishDescriptorAsync(
            repository,
            owner,
            RoutingId.From("corrupt-peer"));
        var store = new MissingOwnerLeaseExpiryStore(
            inner,
            ZLinkProviderLocationRepository.OwnerKey(owner.OwnerId));

        var error = await Assert.ThrowsAsync<InvalidDataException>(
            async () => await ResolveAsync(store, time));

        Assert.Equal(
            "The Location Store owner lease record is invalid.",
            error.Message);
    }

    private static async ValueTask PublishDescriptorAsync(
        IZLinkLocationRepository repository,
        ZLinkLocationOwnerToken owner,
        RoutingId routingId)
    {
        var result = await repository.UpdateMeshNodeAsync(
            InMemoryLocationStoreTests.MeshNode(
                owner.OwnerId,
                PeerEndpoint,
                routingId.ToString(),
                MeshName,
                owner.LeaseGeneration),
            ZLinkLocationWriteIntent.NewClaim);
        Assert.Equal(ZLinkLocationWriteStatus.Stored, result.Status);
    }

    private static async Task<IReadOnlyDictionary<string, RoutingId>> ResolveAsync(
        IZLinkLocationStore store,
        TimeProvider time)
    {
        var repository = new ZLinkProviderLocationRepository(store);
        var options = new ZLinkLocationOptions { PollingInterval = TimeSpan.Zero };
        var leaseTracker = new ZLinkOwnerLeaseTracker(repository, options, time);
        var locationRuntime = new ZLinkLocationRuntime(options, repository, time);
        var lifecycle = new ZLinkLocationLifecycle(
            locationRuntime,
            new ZLinkStoreLocationResolvers(
                repository,
                leaseTracker,
                new ZLinkObservedLocationGenerations(),
                options: options,
                timeProvider: time));
        var router = new ZLinkSpotRouterCapabilityRegistration();
        router.ManualConnections.Connect(PeerEndpoint);
        var node = new ZLinkSpotNodeRegistration
        {
            SpotNodeName = MeshName,
            ObjectRole = ZLinkMeshNodeObjectRole.Client,
            ObjectRoleSelected = true,
            Router = router
        };
        var registration = new ZLinkFrameworkRegistration();
        await ZLinkSpotNodeInitializer.ResolveManualPeerRoutingIdsAsync(
            node,
            MeshName,
            RoutingId.From("local"),
            registration,
            lifecycle,
            leaseTracker);
        return router.PeerRoutingIds;
    }

    private sealed class MissingOwnerLeaseExpiryStore(
        IZLinkLocationStore inner,
        ZLinkStoreKey ownerKey) : IZLinkLocationStore
    {
        public async ValueTask<ZLinkStoreReadResult> ReadAsync(
            ZLinkStoreKey key,
            CancellationToken cancellationToken = default)
        {
            var read = await inner.ReadAsync(key, cancellationToken)
                .ConfigureAwait(false);
            return key == ownerKey && read is ZLinkStoreReadResult.Found found
                ? new ZLinkStoreReadResult.Found(found.Value with { ExpiresAt = null })
                : read;
        }

        public ValueTask<ZLinkStoreWriteResult> WriteAsync(
            ZLinkStoreWriteRequest request,
            CancellationToken cancellationToken = default) =>
            inner.WriteAsync(request, cancellationToken);

        public ValueTask<ZLinkStoreScanResult> ScanAsync(
            ZLinkStoreScanRequest request,
            CancellationToken cancellationToken = default) =>
            inner.ScanAsync(request, cancellationToken);
    }
}
