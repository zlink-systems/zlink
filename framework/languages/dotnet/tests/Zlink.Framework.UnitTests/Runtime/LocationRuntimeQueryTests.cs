using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

/// <summary>
/// The runtime query surface reads the registered store directly — no
/// cache, no freshness — and excludes rows of expired owners from every
/// success result. Spot and Actor rows are resolve-only store records, so
/// topology and summaries project MeshNode descriptors only.
/// </summary>
public sealed class LocationRuntimeQueryTests
{
    private const string LiveOwner = "live-owner";
    private const string DeadOwner = "dead-owner";
    private static readonly TimeSpan ShortLease = TimeSpan.FromSeconds(15);
    private static readonly string[] RegisteredMeshes = ["play"];

    [Fact]
    public async Task Readiness_Returns_False_When_Query_Fails()
    {
        var readiness = new ZLinkLocationReadiness(new FailingRuntimeQuery());

        var ready = await readiness.IsPeerReadyAsync(
            "play",
            ZLinkLocationRole.Router,
            RoutingId.From("node-1"));

        Assert.False(ready);
    }

    [Fact]
    public async Task Descriptor_List_Excludes_Rows_Of_Expired_Owners()
    {
        var fixture = await FixtureAsync();
        await SeedRowsAsync(fixture.Store, LiveOwner, "1");
        await SeedRowsAsync(fixture.Store, DeadOwner, "2");

        // The dead owner stops heartbeating and its short lease runs out.
        fixture.Time.Advance(ShortLease + TimeSpan.FromSeconds(1));

        var descriptors = await fixture.Query.ListMeshNodeDescriptorsAsync("play");

        Assert.Equal(LiveOwner, Assert.Single(descriptors.Items).OwnerId);
    }

    [Fact]
    public async Task Actor_Resolve_Presence_Excludes_Rows_Of_Expired_Owners()
    {
        var fixture = await FixtureAsync();
        await SeedRowsAsync(fixture.Store, DeadOwner, "2");

        // Storage cleanup may lag behind lease expiry. The stale row must be a
        // confirmed miss, not a transient live-owner publication window.
        fixture.Time.Advance(ShortLease + TimeSpan.FromSeconds(1));

        var (row, rowPresent) = await fixture.Resolvers.ResolveActorRowWithPresenceAsync(
            new ZLinkActorLocationKey("actor-2"));

        Assert.Null(row);
        Assert.False(rowPresent);
    }

    [Fact]
    public async Task Queries_Read_The_Store_Directly()
    {
        var fixture = await FixtureAsync();
        await AuthorityLocationTestFixture.PublishActorAsync(
            fixture.Store,
            ActorLocation(LiveOwner, "1"));

        var key = new ZLinkActorLocationKey("actor-1");
        Assert.Equal(LiveOwner, (await fixture.Resolvers.ResolveActorRowAsync(key))!.OwnerId);

        await AuthorityLocationTestFixture.PublishActorAsync(
            fixture.Store,
            ActorLocation(DeadOwner, "2"),
            replace: true);

        // Without a resolver cache the takeover is visible immediately; the
        // resolve surface reads the authority store directly.
        Assert.Equal(DeadOwner, (await fixture.Resolvers.ResolveActorRowAsync(key))!.OwnerId);
    }

    [Fact]
    public async Task Actor_Resolve_Drops_Views_Older_Than_An_Observed_Membership_Epoch()
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryLocationStore(time);
        await store.ClaimLiveOwnerAsync(LiveOwner, TimeSpan.FromMinutes(5));
        var options = new ZLinkLocationOptions
        {
            PollingInterval = TimeSpan.Zero,
            RouteCacheMaxAge = TimeSpan.Zero
        };
        var tracker = new ZLinkOwnerLeaseTracker(store, options, time);

        // A replica that first serves membership epoch 2 and then lags back
        // to epoch 1.
        var lease = Assert.IsType<ZLinkOwnerLeaseReadResult.Found>(
            await store.ReadOwnerLeaseAsync(LiveOwner));
        var lagging = new ScriptedAuthorityStore(
            ActorAuthorityRead(ActorLocation(LiveOwner, "1"), lease.Token, 2, time),
            ActorAuthorityRead(ActorLocation(LiveOwner, "1"), lease.Token, 1, time));
        var observed = new ZLinkObservedLocationGenerations();
        var resolvers = new ZLinkStoreLocationResolvers(
            lagging, tracker, observed, options: options, timeProvider: time);

        var key = new ZLinkActorLocationKey("actor-1");
        var first = await resolvers.ResolveActorRowAsync(key);
        Assert.Equal(2UL, first!.MembershipEpoch);

        // The lagging read must not roll the runtime's view backwards.
        time.Advance(TimeSpan.FromTicks(1));
        var second = await resolvers.ResolveActorRowAsync(key);
        Assert.Null(second);
    }

    [Fact]
    public async Task Topology_And_Service_Summaries_Drop_Older_Descriptor_Revisions()
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryLocationStore(time);
        await store.ClaimLiveOwnerAsync(LiveOwner, TimeSpan.FromMinutes(5));
        var current = InMemoryLocationStoreTests.MeshNode(LiveOwner) with { DescriptorRevision = 2 };
        var stale = current with { DescriptorRevision = 1 };
        var peers = new ScriptedMeshNodeListStore([current], [stale]);
        var options = new ZLinkLocationOptions
        {
            PollingInterval = TimeSpan.Zero,
            RouteCacheMaxAge = TimeSpan.Zero
        };
        var tracker = new ZLinkOwnerLeaseTracker(store, options, time);
        var observed = new ZLinkObservedLocationGenerations();
        var runtime = new ZLinkLocationRuntime(options, store, time);
        var query = new ZLinkLocationRuntimeQueryService(
            options, peers, RegisteredMeshes, tracker, runtime, observed);

        var topology = await query.ListTopologyAsync(new ZLinkLocationTopologyFilter());
        var summaries = await query.ListServiceSummariesAsync(
            new ZLinkLocationServiceSummaryFilter());

        Assert.Single(topology.Items);
        Assert.Empty(summaries.Items);
    }

    [Fact]
    public async Task Topology_Applies_Mesh_And_State_Filters()
    {
        var fixture = await FixtureAsync();
        await SeedRowsAsync(fixture.Store, LiveOwner, "1");

        var ready = await fixture.Query.ListTopologyAsync(
            new ZLinkLocationTopologyFilter(
                MeshName: "play",
                State: ZLinkLocationTopologyState.Ready));
        var lost = await fixture.Query.ListTopologyAsync(
            new ZLinkLocationTopologyFilter(
                MeshName: "play",
                State: ZLinkLocationTopologyState.Lost));
        var otherMesh = await fixture.Query.ListTopologyAsync(
            new ZLinkLocationTopologyFilter(MeshName: "other"));

        Assert.Single(ready.Items);
        Assert.Empty(lost.Items);
        Assert.Empty(otherMesh.Items);
    }

    [Fact]
    public async Task Topology_Projects_Lost_State_For_Expired_Owners()
    {
        var fixture = await FixtureAsync();
        await SeedRowsAsync(fixture.Store, DeadOwner, "9");
        fixture.Time.Advance(ShortLease + TimeSpan.FromSeconds(1));

        var topology = await fixture.Query.ListTopologyAsync(
            new ZLinkLocationTopologyFilter(State: ZLinkLocationTopologyState.Lost));

        Assert.Single(topology.Items);
        Assert.Equal(ZLinkLocationTopologyState.Lost, topology.Items[0].State);
    }

    [Fact]
    public async Task Status_Reports_Shared_Read_Failures_Through_Health()
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryLocationStore(time);
        var options = new ZLinkLocationOptions { PollingInterval = TimeSpan.Zero };
        var tracker = new ZLinkOwnerLeaseTracker(store, options, time);
        var runtime = new ZLinkLocationRuntime(options, store, time);
        var health = new ZLinkLocationStoreHealth(time);
        var query = new ZLinkLocationRuntimeQueryService(
            options,
            store,
            RegisteredMeshes,
            tracker,
            runtime,
            new ZLinkObservedLocationGenerations(),
            storeHealth: health);
        health.ReportFailure("mesh-node-query-read", new InvalidOperationException("read unavailable"));

        var status = await query.GetStatusAsync();

        Assert.False(status.StoreHealthy);
        Assert.Contains("read unavailable", health.GetSnapshot().LastError,
            StringComparison.Ordinal);
    }

    [Fact]
    public void Page_Request_Policy_Normalizes_Default_And_Rejects_Invalid_Sizes()
    {
        Assert.Equal(100, ZLinkPageRequestPolicy.Normalize(default).PageSize);
        Assert.Equal(100, ZLinkPageRequestPolicy.Normalize(new ZLinkPageRequest(0)).PageSize);
        Assert.Equal(1000, ZLinkPageRequestPolicy.Normalize(new ZLinkPageRequest(1000)).PageSize);
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            ZLinkPageRequestPolicy.Normalize(new ZLinkPageRequest(-1)));
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            ZLinkPageRequestPolicy.Normalize(new ZLinkPageRequest(1001)));
    }

    [Fact]
    public async Task Status_Reports_The_Most_Recent_Success_Across_Reads_And_Lease_Renewal()
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryLocationStore(time);
        var options = new ZLinkLocationOptions();
        var tracker = new ZLinkOwnerLeaseTracker(store, options, time);
        var runtime = new ZLinkLocationRuntime(
            options, store, time);
        var health = new ZLinkLocationStoreHealth(time);
        var query = new ZLinkLocationRuntimeQueryService(
            options,
            store,
            RegisteredMeshes,
            tracker,
            runtime,
            new ZLinkObservedLocationGenerations(),
            storeHealth: health);
        health.ReportSuccess("mesh-node-query-read");
        var readSuccessAt = health.GetSnapshot().LastSuccessAt;
        time.Advance(TimeSpan.FromDays(30));
        var renewed = await runtime.RenewOwnerLeaseOnceAsync();
        Assert.True(renewed, $"owner lease renewal failed: {runtime.LastError}");

        var status = await query.GetStatusAsync();

        Assert.True(
            status.LastRefreshAt > readSuccessAt,
            $"lastRefresh={status.LastRefreshAt:O} readSuccess={readSuccessAt:O} "
            + $"leaseRenewed={status.OwnerLeaseRenewedAt:O} now={time.GetUtcNow():O}");
        Assert.Equal(time.GetUtcNow(), status.LastRefreshAt);
    }

    [Fact]
    public async Task Store_Health_Distinguishes_Caller_Cancellation_From_Internal_Cancellation()
    {
        var callerHealth = new ZLinkLocationStoreHealth();
        using var canceled = new CancellationTokenSource();
        canceled.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(async () =>
            await ZLinkLocationStoreRead.ExecuteAsync<int>(
                callerHealth,
                "caller",
                canceled.Token,
                _ => ValueTask.FromException<int>(new OperationCanceledException())));

        var internalHealth = new ZLinkLocationStoreHealth();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(async () =>
            await ZLinkLocationStoreRead.ExecuteAsync<int>(
                internalHealth,
                "internal",
                CancellationToken.None,
                _ => ValueTask.FromException<int>(new OperationCanceledException("store timeout"))));

        Assert.True(callerHealth.GetSnapshot().Healthy);
        Assert.False(internalHealth.GetSnapshot().Healthy);
    }

    private sealed class ScriptedMeshNodeListStore(params ZLinkMeshNodeDescriptor[][] pages)
        : ZLinkLocationStoreTestDouble
    {
        private readonly Queue<ZLinkMeshNodeDescriptor[]> _pages = new(pages);

        public override ValueTask<ZLinkLocationPage<ZLinkMeshNodeDescriptor>> ListMeshNodesAsync(
            string meshName,
            ZLinkPageRequest page,
            CancellationToken cancellationToken = default) =>
            ValueTask.FromResult(new ZLinkLocationPage<ZLinkMeshNodeDescriptor>(
                _pages.Count > 0 ? _pages.Dequeue() : [],
                null));

    }

    private sealed class ScriptedAuthorityStore(params ZLinkAuthorityReadResult[] reads)
        : ZLinkLocationStoreTestDouble
    {
        private readonly Queue<ZLinkAuthorityReadResult> _reads = new(reads);

        public override ValueTask<ZLinkAuthorityReadResult> ReadAuthorityAsync(
            ZLinkAuthorityKey key,
            CancellationToken cancellationToken = default) =>
            ValueTask.FromResult(_reads.Dequeue());

    }

    private sealed class FailingRuntimeQuery : IZLinkLocationRuntimeQuery
    {
        public ValueTask<ZLinkLocationRuntimeStatus> GetStatusAsync(
            CancellationToken cancellationToken = default) =>
            throw new InvalidOperationException("store unavailable");

        public ValueTask<ZLinkLocationPage<ZLinkLocationTopologyEntry>> ListTopologyAsync(
            ZLinkLocationTopologyFilter filter,
            ZLinkPageRequest page = default,
            CancellationToken cancellationToken = default) =>
            throw new InvalidOperationException("store unavailable");

        public ValueTask<ZLinkLocationPage<ZLinkLocationServiceSummary>>
            ListServiceSummariesAsync(
            ZLinkLocationServiceSummaryFilter filter,
            ZLinkPageRequest page = default,
            CancellationToken cancellationToken = default) =>
            throw new InvalidOperationException("store unavailable");
    }

    private static async Task SeedRowsAsync(ZLinkInMemoryLocationStore store, string owner, string suffix)
    {
        await AuthorityLocationTestFixture.PublishSpotAsync(
            store,
            InMemoryLocationStoreTests.Spot(owner, $"spot-{suffix}") with
            {
                OwnerNodeRid = RoutingId.From($"node-{suffix}"),
                OwnerNodeGeneration = 1,
                SpotGeneration = 1
            });
        await AuthorityLocationTestFixture.PublishActorAsync(
            store,
            ActorLocation(owner, suffix, $"actor-{suffix}"));
    }

    private static ZLinkResolvedActorLocation ActorLocation(
        string owner,
        string suffix,
        string actorId = "actor-1") =>
        InMemoryLocationStoreTests.Actor(owner, actorId) with
        {
            OwnerNodeRid = RoutingId.From($"node-{suffix}"),
            OwnerNodeGeneration = 1,
            SpotId = $"entry-{suffix}",
            SpotGeneration = 1,
            MembershipEpoch = 1
        };

    private static ZLinkAuthorityReadResult ActorAuthorityRead(
        ZLinkResolvedActorLocation row,
        ZLinkLocationOwnerToken owner,
        ulong membershipEpoch,
        ManualTimeProvider time)
    {
        var payload = ZLinkActorAuthorityPayloadCodec.Encode(
            new ZLinkActorAuthorityPayload(
                ZLinkActorAuthorityState.Ready,
                row.ActorType,
                row.ActorId,
                row.SpotId,
                row.SpotGeneration,
                row.SpotKind,
                owner.OwnerId,
                checked((ulong)owner.LeaseGeneration),
                row.MeshName,
                row.OwnerNodeRid,
                row.OwnerNodeGeneration));
        return new ZLinkAuthorityReadResult.Found(
            new ZLinkAuthoritySnapshot(
                membershipEpoch.ToString(),
                payload,
                row.ActorRef.ObjectGeneration,
                membershipEpoch,
                owner.OwnerId,
                owner.LeaseGeneration,
                new ZLinkPlacementAllocation(
                    ZLinkPlacementAllocationState.Active,
                    ZLinkPlacementObjectKind.Actor,
                    row.ActorType,
                    new ZLinkMeshNodeDescriptorKey(row.MeshName, row.OwnerNodeRid),
                    row.OwnerNodeGeneration,
                    new ZLinkCapacityVector(1, 0, null)),
                null,
                time.GetUtcNow()));
    }

    private static async Task<QueryFixture> FixtureAsync()
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryLocationStore(time);
        await store.ClaimLiveOwnerAsync(LiveOwner, TimeSpan.FromMinutes(5));
        await store.ClaimLiveOwnerAsync(DeadOwner, ShortLease);

        var options = new ZLinkLocationOptions
        {
            PollingInterval = TimeSpan.Zero,
            RouteCacheMaxAge = TimeSpan.Zero
        };
        var tracker = new ZLinkOwnerLeaseTracker(store, options, time);
        var observed = new ZLinkObservedLocationGenerations();
        var resolvers = new ZLinkStoreLocationResolvers(
            store, tracker, observed, options: options, timeProvider: time);
        var runtime = new ZLinkLocationRuntime(options, store, time);
        var query = new ZLinkLocationRuntimeQueryService(
            options, store, RegisteredMeshes, tracker, runtime, observed);
        return new QueryFixture(store, resolvers, query, time);
    }

    private sealed record QueryFixture(
        ZLinkInMemoryLocationStore Store,
        ZLinkStoreLocationResolvers Resolvers,
        ZLinkLocationRuntimeQueryService Query,
        ManualTimeProvider Time);
}
