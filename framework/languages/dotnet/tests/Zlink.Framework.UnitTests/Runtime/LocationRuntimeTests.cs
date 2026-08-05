using System.Security.Cryptography;
using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

public sealed class LocationRuntimeTests
{
    [Fact]
    public async Task Owner_Lease_Renew_Tracks_Health_And_Recovers_After_Store_Failure()
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryLocationStore(time);
        var flaky = new FlakyOwnerLeaseStore(store);
        var runtime = NewRuntime(flaky, time);

        Assert.True(await runtime.RenewOwnerLeaseOnceAsync());
        Assert.True(runtime.GetHealthSnapshot().Healthy);
        Assert.Null(runtime.LastError);

        // Fail-static: a store outage is recorded, never thrown, and the
        // next successful renew clears the error.
        flaky.Fail = true;
        Assert.False(await runtime.RenewOwnerLeaseOnceAsync());
        Assert.False(runtime.GetHealthSnapshot().Healthy);
        Assert.NotNull(runtime.LastError);

        flaky.Fail = false;
        Assert.True(await runtime.RenewOwnerLeaseOnceAsync());
        Assert.True(runtime.GetHealthSnapshot().Healthy);
        Assert.Null(runtime.LastError);
    }

    [Fact]
    public async Task Owner_Admission_Uses_The_Monotonic_Fencing_Deadline()
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryLocationStore(time);
        var options = new ZLinkLocationOptions
        {
            OwnerLeaseTtl = TimeSpan.FromSeconds(15),
            OwnerLeaseFencingMargin = TimeSpan.FromSeconds(5)
        };
        var runtime = new ZLinkLocationRuntime(
            options,
            store,
            time);

        Assert.True(await runtime.RenewOwnerLeaseOnceAsync());
        Assert.True(runtime.IsOwnerAdmissionOpen);
        var cleanupToken = runtime.OwnerToken;

        time.AdvanceMonotonicOnly(TimeSpan.FromSeconds(10));
        Assert.False(runtime.IsOwnerAdmissionOpen);
        Assert.Throws<InvalidOperationException>(
            () => _ = runtime.AdmissionOwnerToken);
        Assert.Equal(cleanupToken, runtime.OwnerToken);

        // A later exact-token renewal establishes a new shared deadline.
        Assert.True(await runtime.RenewOwnerLeaseOnceAsync());
        Assert.True(runtime.IsOwnerAdmissionOpen);

        // Wall-clock changes cannot lengthen or shorten the local fence.
        time.AdvanceWallClockOnly(TimeSpan.FromHours(1));
        Assert.True(runtime.IsOwnerAdmissionOpen);
    }

    [Fact]
    public async Task Owner_Lease_Renew_Timeout_Applies_To_Fixed_Routing_Ids()
    {
        var store = new ZLinkInMemoryLocationStore();
        var hanging = new HangingOwnerLeaseStore(store);
        var runtime = new ZLinkLocationRuntime(
            new ZLinkLocationOptions
            {
                OwnerLeaseRenewTimeout = TimeSpan.FromMilliseconds(25)
            },
            hanging);

        var renewed = await runtime.RenewOwnerLeaseOnceAsync().AsTask()
            .WaitAsync(TimeSpan.FromSeconds(1));

        Assert.False(renewed);
        Assert.False(runtime.GetHealthSnapshot().Healthy);
        Assert.Contains("timeout", runtime.LastError, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public async Task Authority_Reservation_Race_Gives_One_Winner_Across_Runtimes()
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryLocationStore(time);
        var runtimeA = NewRuntime(store, time);
        var runtimeB = NewRuntime(store, time);
        await runtimeA.RenewOwnerLeaseOnceAsync();
        await runtimeB.RenewOwnerLeaseOnceAsync();

        await RegisterObjectNodeAsync(runtimeA);
        var requestA = AuthorityReservation(
            runtimeA,
            "actor:play:actor-1",
            ZLinkPlacementObjectKind.Actor,
            "player");
        var requestB = requestA with
        {
            CreationIntentReference = "intent:actor:play:actor-1:b",
            CreationIntentHash = SHA256.HashData(
                System.Text.Encoding.UTF8.GetBytes(
                    "intent:actor:play:actor-1:b")),
            TargetOwner = runtimeB.OwnerToken
        };

        var winner = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await store.ReserveAsync(requestA));
        var rejected = Assert.IsType<ZLinkObjectReserveResult.Conflict>(
            await store.ReserveAsync(requestB));
        var current = Assert.IsType<ZLinkAuthorityReadResult.Found>(
            rejected.Current).Snapshot;
        Assert.Equal(
            ZLinkPlacementAllocationState.Reserved,
            current.Allocation.State);
        Assert.Equal(runtimeA.OwnerId, current.OwnerId);
        var committed = Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await store.CommitAsync(winner.Reservation, new byte[] { 0x21 }));

        Assert.Equal(runtimeA.OwnerId, committed.Snapshot.OwnerId);
    }

    [Fact]
    public async Task Authority_Cas_Rejects_A_Stale_Writer_After_A_Newer_Commit()
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryLocationStore(time);
        var runtime = NewRuntime(store, time);
        await runtime.RenewOwnerLeaseOnceAsync();
        await RegisterObjectNodeAsync(runtime);
        var reserved = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await store.ReserveAsync(
                AuthorityReservation(
                    runtime,
                    "actor:play:actor-1",
                    ZLinkPlacementObjectKind.Actor,
                    "player")));
        var committed = Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await store.CommitAsync(reserved.Reservation, new byte[] { 0x31 }));
        Assert.IsType<ZLinkAuthorityCompareExchangeResult.Stored>(
            await store.CompareExchangeAuthorityAsync(
                reserved.Reservation.Key,
                committed.Snapshot.StoreVersion,
                new ZLinkAuthorityMutation.Put(
                    new byte[] { 0x32 },
                    ZLinkAuthorityGenerationTransition.Preserve,
                    null,
                    null)));

        Assert.IsType<ZLinkAuthorityCompareExchangeResult.Conflict>(
            await store.CompareExchangeAuthorityAsync(
                reserved.Reservation.Key,
                committed.Snapshot.StoreVersion,
                new ZLinkAuthorityMutation.Put(
                    new byte[] { 0x33 },
                    ZLinkAuthorityGenerationTransition.Preserve,
                    null,
                    null)));
    }

    [Fact]
    public async Task Shutdown_Removes_Owner_Lease_Then_Bulk_Removes_Rows()
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryLocationStore(time);
        var runtime = NewRuntime(store, time);
        await runtime.StartAsync(RoutingId.From("node-1"));
        await RegisterObjectNodeAsync(runtime);
        await CommitAuthorityAsync(
            runtime, "actor:play:actor-1", ZLinkPlacementObjectKind.Actor, "player");
        await CommitAuthorityAsync(
            runtime, "spot:play:spot-1", ZLinkPlacementObjectKind.UserSpot, "game");

        await runtime.StopAsync();

        Assert.IsType<ZLinkOwnerLeaseReadResult.Missing>(
            await store.ReadOwnerLeaseAsync(runtime.OwnerId));
        Assert.IsType<ZLinkAuthorityReadResult.Found>(
            await store.ReadAuthorityAsync(new ZLinkAuthorityKey("actor:play:actor-1")));
        Assert.IsType<ZLinkAuthorityReadResult.Found>(
            await store.ReadAuthorityAsync(new ZLinkAuthorityKey("spot:play:spot-1")));
        Assert.Empty((await store.ListMeshNodesAsync("play", default)).Items);
    }

    [Fact]
    public async Task StopAsync_Propagates_Caller_Cancellation_And_Remains_Safe_To_Dispose()
    {
        var inner = new ZLinkInMemoryLocationStore();
        var ownerStore = new CancelingOwnerCleanupStore(inner);
        var runtime = new ZLinkLocationRuntime(
            new ZLinkLocationOptions
            {
                OwnerLeaseRenewInterval = TimeSpan.FromHours(1),
                OwnerLeaseTtl = TimeSpan.FromHours(2)
            },
            ownerStore);
        await runtime.StartAsync(RoutingId.From("cancel-stop-node"));
        using var cancellation = new CancellationTokenSource();

        var stop = runtime.StopAsync(cancellation.Token).AsTask();
        await ownerStore.RemovalStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        cancellation.Cancel();

        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => stop);
        Assert.False(runtime.GetHealthSnapshot().Healthy);
        Assert.Equal(0, ownerStore.RenewCalls);

        await runtime.StopAsync(CancellationToken.None);
        await runtime.DisposeAsync();
        Assert.Equal(0, ownerStore.RenewCalls);
    }

    [Fact]
    public async Task Drain_Cleanup_Failure_Keeps_Lease_Heartbeat_Until_Retry_Succeeds()
    {
        var inner = new ZLinkInMemoryLocationStore();
        var store = new FailOnceOwnerCleanupStore(inner);
        var options = new ZLinkLocationOptions
        {
            OwnerLeaseRenewInterval = TimeSpan.FromMilliseconds(10),
            OwnerLeaseTtl = TimeSpan.FromSeconds(15)
        };
        var runtime = new ZLinkLocationRuntime(
            options,
            store);
        await runtime.StartAsync(RoutingId.From("node-1"));

        await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await runtime.CleanupOwnerForDrainAsync(CancellationToken.None));
        await store.HeartbeatAfterFailure.Task.WaitAsync(TimeSpan.FromSeconds(1));
        Assert.IsType<ZLinkOwnerLeaseReadResult.Found>(
            await inner.ReadOwnerLeaseAsync(runtime.OwnerId));

        await runtime.CleanupOwnerForDrainAsync(CancellationToken.None);
        Assert.IsType<ZLinkOwnerLeaseReadResult.Missing>(
            await inner.ReadOwnerLeaseAsync(runtime.OwnerId));
        await runtime.StopAsync();
    }

    [Fact]
    public async Task Drain_Cleanup_Removes_Every_Owner_Row_And_The_Owner_Lease()
    {
        var store = new ZLinkInMemoryLocationStore();
        var runtime = new ZLinkLocationRuntime(
            new ZLinkLocationOptions(),
            store);
        await runtime.StartAsync(RoutingId.From("node-drain"));
        await RegisterObjectNodeAsync(runtime);
        await CommitAuthorityAsync(
            runtime, "actor:play:actor-1", ZLinkPlacementObjectKind.Actor, "player");
        await CommitAuthorityAsync(
            runtime, "spot:play:spot-drain", ZLinkPlacementObjectKind.UserSpot, "game");

        await runtime.CleanupOwnerForDrainAsync(CancellationToken.None);

        Assert.IsType<ZLinkOwnerLeaseReadResult.Missing>(
            await store.ReadOwnerLeaseAsync(runtime.OwnerId));
        Assert.IsType<ZLinkAuthorityReadResult.Found>(
            await store.ReadAuthorityAsync(new ZLinkAuthorityKey("actor:play:actor-1")));
        Assert.IsType<ZLinkAuthorityReadResult.Found>(
            await store.ReadAuthorityAsync(new ZLinkAuthorityKey("spot:play:spot-drain")));
        Assert.Empty((await store.ListMeshNodesAsync("play", default)).Items);
        await runtime.StopAsync();
    }

    [Fact]
    public async Task Startup_RequiresOwnerLease_AndCanRetryAfterTheStoreRecovers()
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryLocationStore(time);
        var flaky = new FlakyOwnerLeaseStore(store) { Fail = true };
        var runtime = NewRuntime(flaky, time);

        await Assert.ThrowsAsync<InvalidOperationException>(() =>
            runtime.StartAsync(RoutingId.From("node-1")).AsTask());
        Assert.IsType<ZLinkOwnerLeaseReadResult.Missing>(
            await store.ReadOwnerLeaseAsync(runtime.OwnerId));

        flaky.Fail = false;
        await runtime.StartAsync(RoutingId.From("node-1"));
        Assert.IsType<ZLinkOwnerLeaseReadResult.Found>(
            await store.ReadOwnerLeaseAsync(runtime.OwnerId));
        await runtime.StopAsync();
    }

    [Fact]
    public async Task Restart_UsesFreshOwnerGeneration_AndReportsStoppedLeaseUnhealthy()
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryLocationStore(time);
        var runtime = NewRuntime(store, time);

        await runtime.StartAsync(RoutingId.From("stable-node"));
        var firstOwner = runtime.OwnerId;
        Assert.True(runtime.GetHealthSnapshot().Healthy);

        await runtime.StopAsync();
        Assert.False(runtime.GetHealthSnapshot().Healthy);

        await runtime.StartAsync(RoutingId.From("stable-node"));
        Assert.NotEqual(firstOwner, runtime.OwnerId);
        Assert.True(runtime.GetHealthSnapshot().Healthy);
        await runtime.StopAsync();
    }

    [Fact]
    public async Task DisposeAsync_WaitsForInFlightHeartbeatBeforeCleaningOwnerResources()
    {
        var inner = new ZLinkInMemoryLocationStore();
        var store = new BlockingHeartbeatStore(inner);
        var runtime = new ZLinkLocationRuntime(
            new ZLinkLocationOptions
            {
                OwnerLeaseRenewInterval = TimeSpan.FromMilliseconds(5),
                OwnerLeaseTtl = TimeSpan.FromSeconds(15)
            },
            store);
        await runtime.StartAsync(RoutingId.From("dispose-node"));
        await store.HeartbeatStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var firstDispose = runtime.DisposeAsync().AsTask();
        var secondDispose = runtime.DisposeAsync().AsTask();
        await Task.Delay(25);
        Assert.False(firstDispose.IsCompleted);
        Assert.False(secondDispose.IsCompleted);

        store.ReleaseHeartbeat.TrySetResult();
        await Task.WhenAll(firstDispose, secondDispose).WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Equal(2, store.RenewCalls);
        Assert.IsType<ZLinkOwnerLeaseReadResult.Missing>(
            await inner.ReadOwnerLeaseAsync(runtime.OwnerId));
    }

    [Fact]
    public async Task DisposeAsync_PreventsQueuedStartFromCreatingANewRuntimeGeneration()
    {
        var inner = new ZLinkInMemoryLocationStore();
        var store = new BlockingHeartbeatStore(inner);
        var runtime = new ZLinkLocationRuntime(
            new ZLinkLocationOptions
            {
                OwnerLeaseRenewInterval = TimeSpan.FromMilliseconds(5),
                OwnerLeaseTtl = TimeSpan.FromSeconds(15)
            },
            store);
        await runtime.StartAsync(RoutingId.From("dispose-race-node"));
        await store.HeartbeatStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var stop = runtime.StopAsync().AsTask();
        await Task.Delay(25);
        Assert.False(stop.IsCompleted);
        var queuedStart = runtime.StartAsync(RoutingId.From("forbidden-generation")).AsTask();
        var dispose = runtime.DisposeAsync().AsTask();
        var repeatedDispose = runtime.DisposeAsync().AsTask();
        Assert.Same(dispose, repeatedDispose);

        store.ReleaseHeartbeat.TrySetResult();
        await stop.WaitAsync(TimeSpan.FromSeconds(5));
        await Assert.ThrowsAsync<ObjectDisposedException>(() => queuedStart);
        await dispose.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Equal(2, store.RenewCalls);
        Assert.IsType<ZLinkOwnerLeaseReadResult.Missing>(
            await inner.ReadOwnerLeaseAsync(runtime.OwnerId));
        await Assert.ThrowsAsync<ObjectDisposedException>(async () =>
            await runtime.StartAsync(RoutingId.From("after-dispose")));
    }

    [Fact]
    public async Task SynchronousServiceProviderDispose_CannotSilentlyPartiallyDisposeLocationRuntime()
    {
        var store = new ZLinkInMemoryLocationStore();
        var runtime = new ZLinkLocationRuntime(
            new ZLinkLocationOptions(),
            store);
        var provider = new ServiceCollection()
            .AddSingleton(_ => runtime)
            .BuildServiceProvider();
        _ = provider.GetRequiredService<ZLinkLocationRuntime>();

        var failure = Assert.Throws<InvalidOperationException>(provider.Dispose);
        Assert.Contains("IAsyncDisposable", failure.Message, StringComparison.Ordinal);

        await runtime.DisposeAsync();
    }

    [Fact]
    public async Task InMemory_Registration_Resolves_Resolvers_Query_And_Shared_Store()
    {
        var services = new ServiceCollection();
        services.AddZLinkFramework(options => options.UseTestLocationStore());
        await using var provider = services.BuildServiceProvider();

        Assert.NotNull(provider.GetRequiredService<IZLinkMeshNodeLocationResolver>());
        Assert.NotNull(provider.GetRequiredService<ZLinkLocationAddressResolvers>());
        Assert.NotNull(provider.GetRequiredService<IZLinkLocationRuntimeQuery>());

        // The unified store is registered as one shared service instance.
        var first = provider.GetRequiredService<IZLinkLocationRepository>();
        var second = provider.GetRequiredService<IZLinkLocationRepository>();
        Assert.Same(first, second);
    }

    private static async ValueTask RegisterObjectNodeAsync(
        ZLinkLocationRuntime runtime)
    {
        var descriptor = InMemoryLocationStoreTests.MeshNode(
            runtime.OwnerId,
            nodeRid: "node-1") with
        {
            ObjectRole = ZLinkMeshNodeObjectRole.Server,
            EntrySpotId = "node-1-entry-test",
            ObjectCapabilities =
            [
                new ZLinkObjectCapability(
                    ZLinkPlacementObjectKind.Actor,
                    "player",
                    ZLinkObjectMaintenancePolicyKind.Recreate,
                    HasSnapshotAdapter: false,
                    Limit: 0),
                new ZLinkObjectCapability(
                    ZLinkPlacementObjectKind.UserSpot,
                    "game",
                    ZLinkObjectMaintenancePolicyKind.Recreate,
                    HasSnapshotAdapter: false,
                    Limit: 0)
            ],
            Capacity = new ZLinkPlacementCapacity(
                new ZLinkPopulationCapacity(0, 0, 0),
                new ZLinkPopulationCapacity(0, 0, 0),
                [
                    new ZLinkSpotTypeCapacity(
                        ZLinkPlacementObjectKind.UserSpot,
                        "game",
                        0,
                        0,
                        0)
                ])
        };
        Assert.Equal(
            ZLinkLocationWriteStatus.Stored,
            (await runtime.WriteDescriptorAsync(
                descriptor,
                ZLinkLocationWriteIntent.NewClaim)).Status);
    }

    private static ZLinkObjectReservationRequest AuthorityReservation(
        ZLinkLocationRuntime runtime,
        string key,
        ZLinkPlacementObjectKind kind,
        string stableType)
    {
        var encoded = System.Text.Encoding.UTF8.GetBytes(key);
        return new ZLinkObjectReservationRequest(
            kind,
            new ZLinkAuthorityKey(key),
            stableType,
            $"intent:{key}",
            SHA256.HashData(encoded),
            encoded.Length,
            new ZLinkMeshNodeDescriptorKey("play", RoutingId.From("node-1")),
            1,
            runtime.OwnerToken,
            new byte[] { 0x10 },
            kind == ZLinkPlacementObjectKind.Actor
                ? new ZLinkCapacityVector(1, 0, null)
                : new ZLinkCapacityVector(
                    0,
                    1,
                    new ZLinkSpotTypeCapacityDelta(kind, stableType, 1)));
    }

    private static async ValueTask CommitAuthorityAsync(
        ZLinkLocationRuntime runtime,
        string key,
        ZLinkPlacementObjectKind kind,
        string stableType)
    {
        var reserved = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await runtime.Store.ReserveAsync(
                AuthorityReservation(runtime, key, kind, stableType)));
        Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await runtime.Store.CommitAsync(
                reserved.Reservation,
                new byte[] { 0x20 }));
    }

    private static ZLinkLocationRuntime NewRuntime(
        IZLinkLocationRepository store,
        ManualTimeProvider time) =>
        new(new ZLinkLocationOptions(), store, time);

    private sealed class FlakyOwnerLeaseStore(IZLinkLocationRepository inner)
        : ZLinkLocationStoreTestDouble
    {
        public bool Fail { get; set; }

        public override ValueTask<ZLinkOwnerLeaseClaimResult> ClaimOwnerLeaseAsync(
            string ownerId,
            TimeSpan leaseTtl,
            CancellationToken cancellationToken = default) =>
            Fail
                ? throw new InvalidOperationException("store unreachable")
                : inner.ClaimOwnerLeaseAsync(ownerId, leaseTtl, cancellationToken);
        public override ValueTask<ZLinkOwnerLeaseReadResult> ReadOwnerLeaseAsync(
            string ownerId,
            CancellationToken cancellationToken = default) =>
            inner.ReadOwnerLeaseAsync(ownerId, cancellationToken);
        public override ValueTask<ZLinkOwnerLeaseRenewResult> RenewOwnerLeaseAsync(
            ZLinkLocationOwnerToken token,
            TimeSpan leaseTtl,
            CancellationToken cancellationToken = default) =>
            Fail
                ? throw new InvalidOperationException("store unreachable")
                : inner.RenewOwnerLeaseAsync(token, leaseTtl, cancellationToken);
        public override ValueTask<ZLinkOwnerLeaseReleaseResult> ReleaseOwnerLeaseAsync(
            ZLinkLocationOwnerToken token,
            CancellationToken cancellationToken = default) =>
            inner.ReleaseOwnerLeaseAsync(token, cancellationToken);
    }

    private sealed class HangingOwnerLeaseStore(IZLinkLocationRepository inner)
        : ZLinkLocationStoreTestDouble
    {
        private readonly TaskCompletionSource<ZLinkOwnerLeaseClaimResult> claim =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        private readonly TaskCompletionSource<ZLinkOwnerLeaseRenewResult> renewal =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public override ValueTask<ZLinkOwnerLeaseClaimResult> ClaimOwnerLeaseAsync(
            string ownerId,
            TimeSpan leaseTtl,
            CancellationToken cancellationToken = default) =>
            new(claim.Task);
        public override ValueTask<ZLinkOwnerLeaseReadResult> ReadOwnerLeaseAsync(
            string ownerId,
            CancellationToken cancellationToken = default) =>
            inner.ReadOwnerLeaseAsync(ownerId, cancellationToken);
        public override ValueTask<ZLinkOwnerLeaseRenewResult> RenewOwnerLeaseAsync(
            ZLinkLocationOwnerToken token,
            TimeSpan leaseTtl,
            CancellationToken cancellationToken = default) =>
            new(renewal.Task);
        public override ValueTask<ZLinkOwnerLeaseReleaseResult> ReleaseOwnerLeaseAsync(
            ZLinkLocationOwnerToken token,
            CancellationToken cancellationToken = default) =>
            inner.ReleaseOwnerLeaseAsync(token, cancellationToken);
    }

    private sealed class BlockingHeartbeatStore(IZLinkLocationRepository inner)
        : ZLinkLocationStoreTestDouble
    {
        private int _renewCalls;

        public TaskCompletionSource HeartbeatStarted { get; } = new(
            TaskCreationOptions.RunContinuationsAsynchronously);

        public TaskCompletionSource ReleaseHeartbeat { get; } = new(
            TaskCreationOptions.RunContinuationsAsynchronously);

        public int RenewCalls => Volatile.Read(ref _renewCalls);

        public override ValueTask<long> RemoveAllByOwnerAsync(
            ZLinkLocationOwnerToken owner,
            CancellationToken cancellationToken = default) =>
            inner.RemoveAllByOwnerAsync(owner, cancellationToken);

        public override ValueTask<ZLinkOwnerLeaseClaimResult> ClaimOwnerLeaseAsync(
            string ownerId,
            TimeSpan leaseTtl,
            CancellationToken cancellationToken = default) =>
            inner.ClaimOwnerLeaseAsync(ownerId, leaseTtl, cancellationToken);
        public override ValueTask<ZLinkOwnerLeaseReadResult> ReadOwnerLeaseAsync(
            string ownerId,
            CancellationToken cancellationToken = default) =>
            inner.ReadOwnerLeaseAsync(ownerId, cancellationToken);
        public override async ValueTask<ZLinkOwnerLeaseRenewResult> RenewOwnerLeaseAsync(
            ZLinkLocationOwnerToken token,
            TimeSpan leaseTtl,
            CancellationToken cancellationToken = default)
        {
            if (Interlocked.Increment(ref _renewCalls) == 2)
            {
                HeartbeatStarted.TrySetResult();
                await ReleaseHeartbeat.Task.ConfigureAwait(false);
            }

            return await inner.RenewOwnerLeaseAsync(token, leaseTtl, cancellationToken)
                .ConfigureAwait(false);
        }
        public override ValueTask<ZLinkOwnerLeaseReleaseResult> ReleaseOwnerLeaseAsync(
            ZLinkLocationOwnerToken token,
            CancellationToken cancellationToken = default) =>
            inner.ReleaseOwnerLeaseAsync(token, cancellationToken);
    }

    private sealed class CancelingOwnerCleanupStore(IZLinkLocationRepository inner)
        : ZLinkLocationStoreTestDouble
    {
        private int _renewCalls;

        public TaskCompletionSource RemovalStarted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public int RenewCalls => Volatile.Read(ref _renewCalls);

        public override ValueTask<long> RemoveAllByOwnerAsync(
            ZLinkLocationOwnerToken owner,
            CancellationToken cancellationToken = default) =>
            inner.RemoveAllByOwnerAsync(owner, cancellationToken);

        public override ValueTask<ZLinkOwnerLeaseClaimResult> ClaimOwnerLeaseAsync(
            string ownerId,
            TimeSpan leaseTtl,
            CancellationToken cancellationToken = default) =>
            inner.ClaimOwnerLeaseAsync(ownerId, leaseTtl, cancellationToken);
        public override ValueTask<ZLinkOwnerLeaseReadResult> ReadOwnerLeaseAsync(
            string ownerId,
            CancellationToken cancellationToken = default) =>
            inner.ReadOwnerLeaseAsync(ownerId, cancellationToken);
        public override ValueTask<ZLinkOwnerLeaseRenewResult> RenewOwnerLeaseAsync(
            ZLinkLocationOwnerToken token,
            TimeSpan leaseTtl,
            CancellationToken cancellationToken = default)
        {
            Interlocked.Increment(ref _renewCalls);
            return inner.RenewOwnerLeaseAsync(token, leaseTtl, cancellationToken);
        }

        public override async ValueTask<ZLinkOwnerLeaseReleaseResult> ReleaseOwnerLeaseAsync(
            ZLinkLocationOwnerToken token,
            CancellationToken cancellationToken = default)
        {
            RemovalStarted.TrySetResult();
            await Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken).ConfigureAwait(false);
            return ZLinkOwnerLeaseReleaseResult.Stale;
        }
    }

    private sealed class FailOnceOwnerCleanupStore(IZLinkLocationRepository inner)
        : ZLinkLocationStoreTestDouble
    {
        private int _fail = 1;
        private int _renewals;

        public TaskCompletionSource HeartbeatAfterFailure { get; } = new(
            TaskCreationOptions.RunContinuationsAsynchronously);

        public override ValueTask<long> RemoveAllByOwnerAsync(
            ZLinkLocationOwnerToken owner,
            CancellationToken cancellationToken = default) =>
            Interlocked.Exchange(ref _fail, 0) != 0
                ? throw new InvalidOperationException("owner cleanup unavailable")
                : inner.RemoveAllByOwnerAsync(owner, cancellationToken);

        public override ValueTask<ZLinkLocationWriteResult> UpdateMeshNodeAsync(
            ZLinkMeshNodeDescriptor descriptor, ZLinkLocationWriteIntent intent,
            CancellationToken cancellationToken = default) =>
            inner.UpdateMeshNodeAsync(descriptor, intent, cancellationToken);

        public override ValueTask<ZLinkLocationWriteStatus> RemoveMeshNodeAsync(
            ZLinkMeshNodeDescriptorKey key, ZLinkLocationOwnerToken owner,
            CancellationToken cancellationToken = default) =>
            inner.RemoveMeshNodeAsync(key, owner, cancellationToken);

        public override ValueTask<ZLinkLocationPage<ZLinkMeshNodeDescriptor>> ListMeshNodesAsync(
            string meshName,
            ZLinkPageRequest page,
            CancellationToken cancellationToken = default) =>
            inner.ListMeshNodesAsync(meshName, page, cancellationToken);

        public override ValueTask<ZLinkOwnerLeaseClaimResult> ClaimOwnerLeaseAsync(
            string ownerId, TimeSpan leaseTtl,
            CancellationToken cancellationToken = default) =>
            inner.ClaimOwnerLeaseAsync(ownerId, leaseTtl, cancellationToken);

        public override ValueTask<ZLinkOwnerLeaseReadResult> ReadOwnerLeaseAsync(
            string ownerId,
            CancellationToken cancellationToken = default) =>
            inner.ReadOwnerLeaseAsync(ownerId, cancellationToken);

        public override ValueTask<ZLinkOwnerLeaseRenewResult> RenewOwnerLeaseAsync(
            ZLinkLocationOwnerToken token, TimeSpan leaseTtl,
            CancellationToken cancellationToken = default)
        {
            if (Interlocked.Increment(ref _renewals) >= 2)
                HeartbeatAfterFailure.TrySetResult();
            return inner.RenewOwnerLeaseAsync(token, leaseTtl, cancellationToken);
        }

        public override ValueTask<ZLinkOwnerLeaseReleaseResult> ReleaseOwnerLeaseAsync(
            ZLinkLocationOwnerToken token,
            CancellationToken cancellationToken = default) =>
            inner.ReleaseOwnerLeaseAsync(token, cancellationToken);

        public override ValueTask<ZLinkAuthorityReadResult> ReadAuthorityAsync(
            ZLinkAuthorityKey key,
            CancellationToken cancellationToken = default) =>
            inner.ReadAuthorityAsync(key, cancellationToken);

        public override ValueTask<ZLinkAuthorityCompareExchangeResult>
            CompareExchangeAuthorityAsync(
                ZLinkAuthorityKey key,
                string expectedStoreVersion,
                ZLinkAuthorityMutation mutation,
                CancellationToken cancellationToken = default) =>
            inner.CompareExchangeAuthorityAsync(
                key, expectedStoreVersion, mutation, cancellationToken);

        public override ValueTask<ZLinkAuthorityScanResult> ListAuthoritiesAsync(
            string prefix,
            ZLinkAuthorityScanCursor? cursor,
            int limit,
            CancellationToken cancellationToken = default) =>
            inner.ListAuthoritiesAsync(prefix, cursor, limit, cancellationToken);

        public override ValueTask<ZLinkObjectReserveResult> ReserveAsync(
            ZLinkObjectReservationRequest request,
            CancellationToken cancellationToken = default) =>
            inner.ReserveAsync(request, cancellationToken);

        public override ValueTask<ZLinkObjectCommitResult> CommitAsync(
            ZLinkObjectReservation reservation,
            ReadOnlyMemory<byte> readyPayload,
            CancellationToken cancellationToken = default) =>
            inner.CommitAsync(reservation, readyPayload, cancellationToken);

        public override ValueTask<ZLinkObjectCreationCompleteResult> CompleteCreationAsync(
            ZLinkObjectReservation reservation,
            ZLinkObjectCreationCompletion completion,
            CancellationToken cancellationToken = default) =>
            inner.CompleteCreationAsync(reservation, completion, cancellationToken);

        public override ValueTask<ZLinkCreationTerminalReadResult> ReadCreationTerminalAsync(
            ZLinkCreationOperationId operation,
            CancellationToken cancellationToken = default) =>
            inner.ReadCreationTerminalAsync(operation, cancellationToken);

        public override ValueTask<ZLinkObjectAbortResult> AbortAsync(
            ZLinkObjectReservation reservation,
            CancellationToken cancellationToken = default) =>
            inner.AbortAsync(reservation, cancellationToken);

        public override ValueTask<ZLinkRelocationCapacityReserveResult>
            ReserveRelocationCapacityAsync(
                ZLinkRelocationCapacityReservationRequest request,
                CancellationToken cancellationToken = default) =>
            inner.ReserveRelocationCapacityAsync(request, cancellationToken);

        public override ValueTask<ZLinkRelocationCapacityAbortResult>
            AbortRelocationCapacityAsync(
                ZLinkRelocationCapacityFence fence,
                CancellationToken cancellationToken = default) =>
            inner.AbortRelocationCapacityAsync(fence, cancellationToken);

        public override ValueTask<ZLinkAggregatePrepareResult> PrepareAggregateAsync(
            ZLinkAggregatePrepareRequest request,
            CancellationToken cancellationToken = default) =>
            inner.PrepareAggregateAsync(request, cancellationToken);

        public override ValueTask<ZLinkAggregateCommitResult> CommitAggregateAsync(
            ZLinkAggregateFence fence,
            CancellationToken cancellationToken = default) =>
            inner.CommitAggregateAsync(fence, cancellationToken);

        public override ValueTask<ZLinkAggregateAbortResult> AbortAggregateAsync(
            ZLinkAggregateFence fence,
            CancellationToken cancellationToken = default) =>
            inner.AbortAggregateAsync(fence, cancellationToken);
    }
}
