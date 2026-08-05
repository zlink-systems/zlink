using Zlink.Framework.Contracts.Locations;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

public sealed class AutoConnectLoopTests
{
    [Fact]
    public async Task Repeated_Dispose_Callers_Share_AutoConnect_Loop_Finalization()
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryLocationStore(time);
        var options = new ZLinkLocationOptions { PollingInterval = TimeSpan.Zero };
        var runtime = new ZLinkLocationRuntime(options, store, time);
        var tracker = new ZLinkOwnerLeaseTracker(store, options, time);
        var resolvers = new ZLinkStoreLocationResolvers(
            store, tracker, new ZLinkObservedLocationGenerations());
        var local = new ZLinkAutoConnectLocal(
            ZLinkLocationAutoConnectType.ClientServer,
            "dispose",
            ZLinkLocationRole.Dealer,
            NodeRid: null,
            Endpoint: string.Empty);
        var reconciler = new ZLinkAutoConnectReconciler(
            local,
            null,
            runtime,
            resolvers,
            new NullExecutor(),
            options,
            time);
        var loop = new ZLinkAutoConnectLoop(
            reconciler, local, options, store, timeProvider: time);

        var first = loop.DisposeAsync().AsTask();
        var second = loop.DisposeAsync().AsTask();

        Assert.Same(first, second);
        await Task.WhenAll(first, second).WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task Unchanged_Change_Stamp_Skips_The_List_Query()
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryLocationStore(time);
        var options = new ZLinkLocationOptions { PollingInterval = TimeSpan.Zero };
        var runtime = new ZLinkLocationRuntime(options, store, time);
        await runtime.RenewOwnerLeaseOnceAsync();
        await store.ClaimLiveOwnerAsync("peer-owner", TimeSpan.FromMinutes(10));

        var tracker = new ZLinkOwnerLeaseTracker(store, options, time);
        var resolvers = new ZLinkStoreLocationResolvers(
            store, tracker, new ZLinkObservedLocationGenerations());
        var countingResolver = new CountingPeerResolver(resolvers);
        var local = new ZLinkAutoConnectLocal(
            ZLinkLocationAutoConnectType.ClientServer, "play", ZLinkLocationRole.Dealer,
            RoutingId.From("local"), "tcp://l:1");
        var localRow = InMemoryLocationStoreTests.MeshNode("ignored", "tcp://l:1", "local");
        var reconciler = new ZLinkAutoConnectReconciler(
            local, localRow, runtime, countingResolver, new NullExecutor(), options, time);
        var loop = new ZLinkAutoConnectLoop(
            reconciler, local, options, store, timeProvider: time);

        // First tick always reads the list (and publishes the local row,
        // which bumps the stamp), so the second tick still reads once more
        // before the stamp settles.
        await loop.TickAsync();
        await loop.TickAsync();
        var reads = countingResolver.ListCalls;

        // No writes since the last tick: the stamp is unchanged and the
        // polling tick costs one stamp read, not a full list query.
        await loop.TickAsync();
        await loop.TickAsync();
        Assert.Equal(reads, countingResolver.ListCalls);

        // A peer write bumps the stamp and the next tick reads the list.
        await store.UpdateMeshNodeAsync(
            InMemoryLocationStoreTests.MeshNode(
                "peer-owner",
                "tcp://r:1",
                "r1",
                leaseGeneration: 2),
            ZLinkLocationWriteIntent.NewClaim);
        await loop.TickAsync();
        Assert.Equal(reads + 1, countingResolver.ListCalls);
    }

    [Fact]
    public async Task Reclaimed_Owner_Token_Does_Not_Revive_A_Stale_Descriptor()
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryLocationStore(time);
        var options = new ZLinkLocationOptions { PollingInterval = TimeSpan.Zero };
        var runtime = new ZLinkLocationRuntime(options, store, time);
        await runtime.RenewOwnerLeaseOnceAsync();

        var tracker = new ZLinkOwnerLeaseTracker(store, options, time);
        var resolvers = new ZLinkStoreLocationResolvers(
            store, tracker, new ZLinkObservedLocationGenerations());
        var executor = new RecordingExecutor();
        var local = new ZLinkAutoConnectLocal(
            ZLinkLocationAutoConnectType.ClientServer, "play", ZLinkLocationRole.Dealer,
            NodeRid: null, Endpoint: string.Empty);
        var reconciler = new ZLinkAutoConnectReconciler(
            local, localRow: null, runtime, resolvers, executor, options, time);
        var loop = new ZLinkAutoConnectLoop(
            reconciler, local, options, store, timeProvider: time,
            leaseTracker: tracker);

        await store.ClaimLiveOwnerAsync(
            "late-owner",
            TimeSpan.FromSeconds(15));
        await store.UpdateMeshNodeAsync(
            InMemoryLocationStoreTests.MeshNode(
                "late-owner",
                "tcp://r:1",
                "r1",
                leaseGeneration: 2),
            ZLinkLocationWriteIntent.NewClaim);
        Assert.Equal(
            ZLinkOwnerLeaseReleaseResult.Released,
            await store.ReleaseOwnerLeaseAsync(
                new ZLinkLocationOwnerToken("late-owner", 2)));
        await loop.TickAsync();
        Assert.Empty(executor.Connected);

        // A reclaimed owner ID receives a different token. The stale
        // descriptor cannot become live merely because the owner ID matches.
        await store.ClaimLiveOwnerAsync("late-owner", TimeSpan.FromSeconds(15));
        await loop.TickAsync();
        Assert.Empty(executor.Connected);
        Assert.Empty(executor.Disconnected);
    }

    [Fact]
    public async Task Pending_Connect_Retries_When_The_Change_Stamp_Is_Unchanged()
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryLocationStore(time);
        var options = new ZLinkLocationOptions { PollingInterval = TimeSpan.Zero };
        var runtime = new ZLinkLocationRuntime(options, store, time);
        await runtime.RenewOwnerLeaseOnceAsync();
        await store.ClaimLiveOwnerAsync("peer-owner", TimeSpan.FromMinutes(1));
        await store.UpdateMeshNodeAsync(
            InMemoryLocationStoreTests.MeshNode(
                "peer-owner",
                "tcp://r:1",
                "r1",
                leaseGeneration: 2),
            ZLinkLocationWriteIntent.NewClaim);
        var tracker = new ZLinkOwnerLeaseTracker(store, options, time);
        var resolvers = new ZLinkStoreLocationResolvers(
            store, tracker, new ZLinkObservedLocationGenerations());
        var executor = new RetryExecutor();
        var local = new ZLinkAutoConnectLocal(
            ZLinkLocationAutoConnectType.ClientServer, "play", ZLinkLocationRole.Dealer,
            NodeRid: null, Endpoint: string.Empty);
        var reconciler = new ZLinkAutoConnectReconciler(
            local, null, runtime, resolvers, executor, options, time);
        var loop = new ZLinkAutoConnectLoop(
            reconciler, local, options, store, timeProvider: time,
            leaseTracker: tracker);

        await loop.TickAsync();
        await loop.TickAsync();

        Assert.Equal(2, executor.ConnectCalls);
        Assert.Single(reconciler.ActiveTargets);
    }

    [Fact]
    public async Task Failed_Stamp_Preflight_Defers_Disconnect_From_The_Recovery_List()
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryLocationStore(time);
        var options = new ZLinkLocationOptions
        {
            PollingInterval = TimeSpan.Zero,
            OwnerLeaseRenewInterval = TimeSpan.FromSeconds(1)
        };
        var runtime = new ZLinkLocationRuntime(options, store, time);
        await runtime.RenewOwnerLeaseOnceAsync();
        var peer = InMemoryLocationStoreTests.MeshNode("peer-owner", "tcp://r:1", "r1");
        var resolver = new SwitchablePeerResolver([peer]);
        var executor = new RecordingExecutor();
        var local = new ZLinkAutoConnectLocal(
            ZLinkLocationAutoConnectType.ClientServer, "play", ZLinkLocationRole.Dealer,
            NodeRid: null, Endpoint: string.Empty);
        var reconciler = new ZLinkAutoConnectReconciler(
            local, null, runtime, resolver, executor, options, time);
        var stamps = new FailingStampStore();
        var loop = new ZLinkAutoConnectLoop(
            reconciler, local, options, stamps, timeProvider: time);

        await loop.TickAsync();
        Assert.Single(reconciler.ActiveTargets);

        // The optimization read observes the outage, but the fallback list
        // succeeds just after recovery with a temporarily incomplete view.
        // That list must not immediately tear down the admitted transport.
        stamps.FailNext = true;
        resolver.Rows = [];
        await loop.TickAsync();

        Assert.Empty(executor.Disconnected);
        Assert.Single(reconciler.ActiveTargets);

        time.Advance(options.OwnerLeaseRenewInterval + TimeSpan.FromMilliseconds(1));
        await loop.TickAsync();
        Assert.Single(executor.Disconnected);
    }

    private sealed class RecordingExecutor : IZLinkAutoConnectExecutor
    {
        public List<ZLinkAutoConnectTarget> Connected { get; } = [];

        public List<ZLinkAutoConnectTarget> Disconnected { get; } = [];

        public bool Connect(ZLinkAutoConnectTarget target) { Connected.Add(target); return true; }

        public bool Disconnect(ZLinkAutoConnectTarget target) { Disconnected.Add(target); return true; }
    }

    private sealed class CountingPeerResolver(IZLinkMeshNodeLocationResolver inner)
        : IZLinkMeshNodeLocationResolver
    {
        public int ListCalls { get; private set; }

        public ValueTask<IReadOnlyList<ZLinkMeshNodeDescriptor>> ListLiveMeshNodesAsync(
            string meshName,
            CancellationToken cancellationToken = default)
        {
            ListCalls++;
            return inner.ListLiveMeshNodesAsync(meshName, cancellationToken);
        }
    }

    private sealed class SwitchablePeerResolver(IReadOnlyList<ZLinkMeshNodeDescriptor> rows)
        : IZLinkMeshNodeLocationResolver
    {
        public IReadOnlyList<ZLinkMeshNodeDescriptor> Rows { get; set; } = rows;

        public ValueTask<IReadOnlyList<ZLinkMeshNodeDescriptor>> ListLiveMeshNodesAsync(
            string meshName,
            CancellationToken cancellationToken = default) => ValueTask.FromResult(Rows);
    }

    private sealed class FailingStampStore : ZLinkLocationStoreTestDouble
    {
        public bool FailNext { get; set; }

        public override ValueTask<ulong?> GetMeshNodeChangeStampAsync(
            string meshName,
            CancellationToken cancellationToken = default)
        {
            if (!FailNext) return ValueTask.FromResult<ulong?>(1);

            FailNext = false;
            return ValueTask.FromException<ulong?>(new InvalidOperationException("stamp unavailable"));
        }
    }

    private sealed class NullExecutor : IZLinkAutoConnectExecutor
    {
        public bool Connect(ZLinkAutoConnectTarget target) => true;

        public bool Disconnect(ZLinkAutoConnectTarget target) => true;
    }

    private sealed class RetryExecutor : IZLinkAutoConnectExecutor
    {
        public int ConnectCalls { get; private set; }

        public bool Connect(ZLinkAutoConnectTarget target) => ++ConnectCalls >= 2;

        public bool Disconnect(ZLinkAutoConnectTarget target) => true;
    }
}
