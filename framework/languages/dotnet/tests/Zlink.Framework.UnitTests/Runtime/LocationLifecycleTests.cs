using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

public sealed class LocationLifecycleTests
{
    private const string MeshName = "play";
    private const string ActorType = "player";
    private const string ActorId = "actor-1";
    private static readonly string[] RegisteredMeshes = [MeshName, "mesh"];

    [Fact]
    public async Task Actor_Ready_Authority_Is_Activated_Only_By_Its_Owner()
    {
        await using var fixture = await LifecycleFixture.CreateAsync();
        var nodeA = await fixture.NodeAsync("node-a");
        var nodeB = await fixture.NodeAsync("node-b");
        var activatedB = 0;
        var key = new ZLinkActorLocationKey(ActorId);

        var winner = await nodeA.ActorOwnership.ExecuteActorClaimThenActivateAsync(
            MeshName,
            ActorType,
            ActorId,
            RoutingId.From("node-a"),
            deactivate: null,
            activate: async cancellationToken =>
            {
                // ActorManager completed reservation before the local
                // ownership coordinator invokes the factory.
                var row = await nodeA.Resolvers.ResolveActorRowAsync(
                    key,
                    cancellationToken);
                Assert.NotNull(row);
                Assert.Equal(nodeA.Runtime.OwnerId, row.OwnerId);
                Assert.Equal(1UL, row.ActorRef.ObjectGeneration);
                return "instance-a";
            },
            CancellationToken.None);

        Assert.Equal("instance-a", winner.Activated);
        Assert.Null(winner.ExistingLocation);

        var loser = await nodeB.ActorOwnership.ExecuteActorClaimThenActivateAsync<string>(
            MeshName,
            ActorType,
            ActorId,
            RoutingId.From("node-b"),
            deactivate: null,
            activate: _ =>
            {
                activatedB++;
                return ValueTask.FromResult("instance-b");
            },
            CancellationToken.None);

        // The non-owner must never activate a second local instance.
        Assert.Null(loser.Activated);
        Assert.Equal(0, activatedB);
        Assert.NotNull(loser.ExistingLocation);
        Assert.Equal(RoutingId.From("node-a"), loser.ExistingLocation.OwnerNodeRid);

        var claimed = await nodeA.Resolvers.ResolveActorRowAsync(key);
        Assert.NotNull(claimed);
        Assert.Equal(nodeA.Runtime.OwnerId, claimed.OwnerId);
        Assert.Equal(1UL, claimed.ActorRef.ObjectGeneration);

        var resolved = await nodeB.Resolvers.ResolveActorRowAsync(key);
        Assert.NotNull(resolved);
        Assert.Equal(RoutingId.From("node-a"), resolved.OwnerNodeRid);
        Assert.Equal(nodeA.Runtime.OwnerId, resolved.OwnerId);
        Assert.Equal(ActorId, resolved.ActorRef.ActorId);
    }

    [Fact]
    public async Task Actor_Ready_Authority_Activation_Does_Not_Reserve_Again()
    {
        await using var fixture = await LifecycleFixture.CreateAsync();
        var controlled = new ControlledActorStore(fixture.Store)
        {
            RejectNewClaimCount = 1
        };
        var node = await fixture.NodeAsync("node-b", controlled);
        var activated = 0;

        var result = await node.ActorOwnership.ExecuteActorClaimThenActivateAsync(
            MeshName,
            ActorType,
            ActorId,
            RoutingId.From("node-b"),
            deactivate: null,
            activate: _ =>
            {
                activated++;
                return ValueTask.FromResult("instance-b");
            },
            CancellationToken.None);

        Assert.Equal("instance-b", result.Activated);
        Assert.Equal(1, activated);
        Assert.Equal(1, controlled.RejectNewClaimCount);
    }

    [Fact]
    public async Task Actor_Activation_Failure_Rolls_The_Claim_Back()
    {
        await using var fixture = await LifecycleFixture.CreateAsync();
        var node = await fixture.NodeAsync("node-a");

        await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await node.ActorOwnership.ExecuteActorClaimThenActivateAsync<string>(
                MeshName,
                ActorType,
                ActorId,
                RoutingId.From("node-a"),
                deactivate: null,
                activate: _ => throw new InvalidOperationException("factory failed"),
                CancellationToken.None));

        // Manager-level creation owns the next reservation attempt. The
        // lifecycle coordinator only rolls back its failed Ready authority.
        Assert.Null(await node.Resolvers.ResolveActorRowAsync(
            new ZLinkActorLocationKey(ActorId)));
        Assert.False(node.ActorOwnership.OwnsActor(ActorId));
    }

    [Fact]
    public async Task Actor_Activation_Failure_Reconciles_A_Failed_Claim_Remove()
    {
        await using var fixture = await LifecycleFixture.CreateAsync();
        var actorStore = new ControlledActorStore(fixture.Store)
        {
            RemoveFailure = new InvalidOperationException("remove failed")
        };
        var node = await fixture.NodeAsync("node-a", actorStore);
        await using var lifecycle = node.Lifecycle;

        var failure = await Assert.ThrowsAsync<AggregateException>(async () =>
            await node.ActorOwnership.ExecuteActorClaimThenActivateAsync<string>(
                MeshName,
                ActorType,
                ActorId,
                RoutingId.From("node-a"),
                deactivate: null,
                activate: _ => throw new InvalidOperationException("factory failed"),
                CancellationToken.None));

        Assert.Contains(
            failure.InnerExceptions,
            exception => exception is InvalidOperationException { Message: "factory failed" });
        for (var attempt = 0; attempt < 50 && node.ActorOwnership.OwnsActor(ActorId); attempt++)
            await Task.Delay(20);

        Assert.False(node.ActorOwnership.OwnsActor(ActorId));
        Assert.Null(await node.Resolvers.ResolveActorRowAsync(
            new ZLinkActorLocationKey(ActorId)));
    }

    [Fact]
    public async Task Actor_AlreadyOwned_Does_Not_Activate_A_Second_Instance()
    {
        await using var fixture = await LifecycleFixture.CreateAsync();
        var node = await fixture.NodeAsync("node-a");
        var key = new ZLinkActorLocationKey(ActorId);

        var first = await node.ActorOwnership.ExecuteActorClaimThenActivateAsync(
            MeshName,
            ActorType,
            ActorId,
            RoutingId.From("node-a"),
            deactivate: null,
            activate: _ => ValueTask.FromResult("instance-a"),
            CancellationToken.None);
        Assert.Equal("instance-a", first.Activated);

        var secondActivated = 0;
        var second = await node.ActorOwnership.ExecuteActorClaimThenActivateAsync<string>(
            MeshName,
            ActorType,
            ActorId,
            RoutingId.From("node-a"),
            deactivate: null,
            activate: _ =>
            {
                secondActivated++;
                throw new InvalidOperationException("duplicate activation");
            },
            CancellationToken.None);

        Assert.Null(second.Activated);
        Assert.Null(second.ExistingLocation);
        Assert.Equal(0, secondActivated);
        Assert.True(node.ActorOwnership.OwnsActor(ActorId));
        Assert.NotNull(await node.Resolvers.ResolveActorRowAsync(key));
    }

    [Fact]
    public async Task Actor_Join_And_Leave_Renew_Membership_With_A_Monotonic_Epoch()
    {
        await using var fixture = await LifecycleFixture.CreateAsync();
        var node = await fixture.NodeAsync("node-a");
        var key = new ZLinkActorLocationKey(ActorId);

        await CreateTrackedActorAsync(node);

        await node.ActorOwnership.NotifyActorJoinedSpotAsync(
            ActorId, "spot-1", spotGeneration: 4);

        var joined = await node.Resolvers.ResolveActorRowAsync(key);
        Assert.Equal(ZLinkSpotKind.User, joined!.SpotKind);
        Assert.Equal("spot-1", joined.SpotId);
        Assert.Equal(4UL, joined.SpotGeneration);
        Assert.Equal(ActorId, joined.ActorRef.ActorId);
        Assert.Equal(1UL, joined.MembershipEpoch);

        await node.ActorOwnership.NotifyActorLeftSpotAsync(ActorId);

        var left = await node.Resolvers.ResolveActorRowAsync(key);
        Assert.Equal(ZLinkSpotKind.Entry, left!.SpotKind);
        Assert.Equal(CreateEntrySpotId(MeshName, node.NodeRid), left.SpotId);
        Assert.Equal(joined.MembershipEpoch, left.MembershipEpoch);
    }

    [Fact]
    public async Task Reserved_Actor_Adopts_Committed_Authority_Before_First_Join()
    {
        await using var fixture = await LifecycleFixture.CreateAsync();
        var node = await fixture.NodeAsync("node-a");
        var actorRef = new ActorRef(
            ActorId,
            1,
            MeshName,
            RoutingId.From("node-a"));

        // ActorManager committed the reservation, but this target runtime did
        // not run the ordinary claim path that installs local ownership.
        Assert.False(node.ActorOwnership.OwnsActor(ActorId));

        await node.ActorOwnership.AdoptCommittedActorAuthorityAsync(
            ActorId,
            ActorType,
            actorRef,
            deactivate: null);
        await node.ActorOwnership.NotifyActorJoinedSpotAsync(
            ActorId,
            "spot-after-create",
            spotGeneration: 9);

        Assert.True(node.ActorOwnership.OwnsActor(ActorId));
        var joined = await node.Resolvers.ResolveActorRowAsync(
            new ZLinkActorLocationKey(ActorId));
        Assert.NotNull(joined);
        Assert.Equal("spot-after-create", joined.SpotId);
        Assert.Equal(9UL, joined.SpotGeneration);
        Assert.Equal(actorRef, joined.ActorRef);
    }

    [Fact]
    public async Task Actor_Release_And_Spot_Release_Remove_Their_Rows_With_The_Owner_Token()
    {
        await using var fixture = await LifecycleFixture.CreateAsync();
        var node = await fixture.NodeAsync("node-a");
        const string spotId = "spot-7";

        await CreateTrackedActorAsync(node);
        await node.ActorOwnership.ReleaseActorAsync(ActorId);
        Assert.Null(await node.Resolvers.ResolveActorRowAsync(
            new ZLinkActorLocationKey(ActorId)));

        var spot = await PublishReadySpotAsync(fixture, node, spotId, 7);
        var status = await node.SpotLocations.ClaimAsync(
            "mesh",
            spotId,
            7,
            "game",
            RoutingId.From("node-a"),
            1,
            ZLinkSpotKind.User,
            authorityOwnerGeneration: spot.AuthorityOwnerGeneration,
            deactivate: null);
        Assert.Equal(ZLinkLocationWriteStatus.Stored, status);
        Assert.NotNull(await node.Resolvers.ResolveSpotRowAsync(
            new ZLinkSpotLocationKey(spotId)));

        await node.SpotLocations.ReleaseAsync("mesh", spotId);
        Assert.Null(await node.Resolvers.ResolveSpotRowAsync(
            new ZLinkSpotLocationKey(spotId)));
    }

    [Fact]
    public async Task Actor_Concurrent_Releases_Await_The_Same_Store_Outcome()
    {
        await using var fixture = await LifecycleFixture.CreateAsync();
        var controlled = new ControlledActorStore(fixture.Store)
        {
            RemoveGate = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously)
        };
        var node = await fixture.NodeAsync("node-a", controlled);
        await CreateTrackedActorAsync(node);

        var first = node.ActorOwnership.ReleaseActorAsync(ActorId).AsTask();
        await controlled.RemoveStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var second = node.ActorOwnership.ReleaseActorAsync(ActorId).AsTask();

        Assert.Equal(1, controlled.RemoveCalls);
        Assert.False(first.IsCompleted);
        Assert.False(second.IsCompleted);

        controlled.RemoveGate.SetResult();
        await Task.WhenAll(first, second);

        Assert.Equal(1, controlled.RemoveCalls);
        Assert.False(node.ActorOwnership.OwnsActor(ActorId));
    }

    [Fact]
    public async Task Actor_Failed_Renew_Does_Not_Become_The_Base_Of_The_Next_Write()
    {
        await using var fixture = await LifecycleFixture.CreateAsync();
        var controlled = new ControlledActorStore(fixture.Store);
        var node = await fixture.NodeAsync("node-a", controlled);
        await CreateTrackedActorAsync(node);

        controlled.RejectNextRenew = true;
        await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await node.ActorOwnership.NotifyActorJoinedSpotAsync(
                ActorId,
                "spot-rejected",
                spotGeneration: 1));
        controlled.RejectNextRenew = false;

        var notTracked = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await node.ActorOwnership.NotifyActorJoinedSpotAsync(
                ActorId,
                "spot-1",
                spotGeneration: 1));
        Assert.Equal(ZLinkFrameworkErrorKind.NotFound, notTracked.Kind);

        var row = await node.Resolvers.ResolveActorRowAsync(
            new ZLinkActorLocationKey(ActorId));
        Assert.NotNull(row);
        // The rejected membership update is not part of the committed base.
        Assert.Equal(1UL, row.ActorRef.ObjectGeneration);
        Assert.Equal(ZLinkSpotKind.Entry, row.SpotKind);
        Assert.Equal(CreateEntrySpotId(MeshName, node.NodeRid), row.SpotId);
    }

    [Fact]
    public async Task Actor_Release_Waits_For_The_Preceding_Renew_And_Uses_Its_Committed_Generation()
    {
        await using var fixture = await LifecycleFixture.CreateAsync();
        var controlled = new ControlledActorStore(fixture.Store)
        {
            RenewGate = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously)
        };
        var node = await fixture.NodeAsync("node-a", controlled);
        await CreateTrackedActorAsync(node);

        var renew = node.ActorOwnership.NotifyActorJoinedSpotAsync(
            ActorId,
            "spot-1",
            spotGeneration: 1).AsTask();
        await controlled.RenewStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var release = node.ActorOwnership.ReleaseActorAsync(ActorId).AsTask();

        Assert.Equal(0, controlled.RemoveCalls);
        controlled.RenewGate.SetResult();
        await renew;
        await release;

        Assert.Equal(1, controlled.RemoveCalls);
        Assert.NotNull(controlled.LastRemoveOwner);
        Assert.True(controlled.LastRemoveOwner.Value.Generation > 0);
        Assert.Null(await node.Resolvers.ResolveActorRowAsync(
            new ZLinkActorLocationKey(ActorId)));
    }

    [Fact]
    public async Task Spot_Restart_On_Same_Node_Takes_Over_The_Live_Row()
    {
        await using var fixture = await LifecycleFixture.CreateAsync();
        var original = await fixture.NodeAsync("node-a");
        const string spotId = "spot-7";
        var key = new ZLinkSpotLocationKey(spotId);

        var originalSpot = await PublishReadySpotAsync(
            fixture, original, spotId, 7);
        var first = await original.SpotLocations.ClaimAsync(
            "mesh",
            spotId,
            7,
            "game",
            RoutingId.From("node-a"),
            1,
            ZLinkSpotKind.User,
            authorityOwnerGeneration: originalSpot.AuthorityOwnerGeneration,
            deactivate: null);
        Assert.Equal(ZLinkLocationWriteStatus.Stored, first);
        var firstRow = await original.Resolvers.ResolveSpotRowAsync(key);

        var restarted = await fixture.NodeAsync("node-a");
        var restartedSpot = await PublishReadySpotAsync(
            fixture, restarted, spotId, 7, replace: true);
        var takeover = await restarted.SpotLocations.ClaimAsync(
            "mesh",
            spotId,
            7,
            "game",
            RoutingId.From("node-a"),
            1,
            ZLinkSpotKind.User,
            authorityOwnerGeneration: restartedSpot.AuthorityOwnerGeneration,
            deactivate: null);

        Assert.Equal(ZLinkLocationWriteStatus.Stored, takeover);
        var current = await restarted.Resolvers.ResolveSpotRowAsync(key);
        Assert.Equal(restarted.Runtime.OwnerId, current!.OwnerId);
        Assert.Equal(RoutingId.From("node-a"), current.OwnerNodeRid);
        Assert.Equal(originalSpot.ObjectGeneration, firstRow!.SpotGeneration);
        Assert.Equal(restartedSpot.ObjectGeneration, current.SpotGeneration);

        var staleRelease = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await original.SpotLocations.ReleaseAsync("mesh", spotId));
        Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, staleRelease.Kind);
        var afterStaleRelease = await restarted.Resolvers.ResolveSpotRowAsync(key);
        Assert.Equal(restarted.Runtime.OwnerId, afterStaleRelease!.OwnerId);
    }

    [Fact]
    public async Task Spot_Claim_From_Different_Node_Does_Not_Take_Over_A_Live_Row()
    {
        await using var fixture = await LifecycleFixture.CreateAsync();
        var nodeA = await fixture.NodeAsync("node-a");
        var nodeB = await fixture.NodeAsync("node-b");
        const string spotId = "spot-7";
        var key = new ZLinkSpotLocationKey(spotId);

        var spot = await PublishReadySpotAsync(fixture, nodeA, spotId, 7);
        var first = await nodeA.SpotLocations.ClaimAsync(
            "mesh",
            spotId,
            7,
            "game",
            RoutingId.From("node-a"),
            1,
            ZLinkSpotKind.User,
            authorityOwnerGeneration: spot.AuthorityOwnerGeneration,
            deactivate: null);
        Assert.Equal(ZLinkLocationWriteStatus.Stored, first);

        var conflict = await nodeB.SpotLocations.ClaimAsync(
            "mesh",
            spotId,
            7,
            "game",
            RoutingId.From("node-b"),
            1,
            ZLinkSpotKind.User,
            authorityOwnerGeneration: 1,
            deactivate: null);

        Assert.Equal(ZLinkLocationWriteStatus.RejectedConflict, conflict);
        var row = await nodeA.Resolvers.ResolveSpotRowAsync(key);
        Assert.Equal(nodeA.Runtime.OwnerId, row!.OwnerId);
        Assert.Equal(RoutingId.From("node-a"), row.OwnerNodeRid);
    }

    [Fact]
    public async Task Entry_Spot_Claim_Tracks_Full_Width_Lifecycle_Generation()
    {
        await using var fixture = await LifecycleFixture.CreateAsync();
        var node = await fixture.NodeAsync("node-a");
        const string entrySpotId = "play-entry-full-width";

        var status = await node.SpotLocations.ClaimAsync(
            MeshName,
            entrySpotId,
            ulong.MaxValue,
            "entry",
            RoutingId.From("node-a"),
            ulong.MaxValue,
            ZLinkSpotKind.Entry,
            authorityOwnerGeneration: ulong.MaxValue,
            deactivate: null);

        Assert.Equal(ZLinkLocationWriteStatus.Stored, status);
        Assert.True(node.SpotLocations.TryGetTrackedGeneration(
            entrySpotId,
            out var trackedGeneration));
        Assert.Equal(ulong.MaxValue, trackedGeneration);
    }

    [Fact]
    public async Task Moved_Actor_Uses_The_Source_Fence_After_Local_Ownership_Is_Lost()
    {
        await using var fixture = await LifecycleFixture.CreateAsync();
        var controlled = new ControlledActorStore(fixture.Store);
        var nodeA = await fixture.NodeAsync("node-a", controlled);
        var nodeB = await fixture.NodeAsync("node-b");
        var deactivated = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);

        await CreateTrackedActorAsync(
            nodeA,
            _ =>
            {
                deactivated.TrySetResult();
                return ValueTask.CompletedTask;
            });
        var sourceSnapshot = Assert.IsType<ZLinkAuthorityReadResult.Found>(
            await fixture.Store.ReadAuthorityAsync(
                ZLinkActorAuthorityPayloadCodec.AuthorityKey(ActorId))).Snapshot;
        Assert.True(
            ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
                sourceSnapshot.Payload.Span,
                out var sourceAuthority));
        var sourceOwner = new ZLinkLocationOwnerToken(
            sourceSnapshot.OwnerId,
            sourceSnapshot.OwnerLeaseGeneration);
        var targetOwner = nodeB.Runtime.OwnerToken;
        var capacity = Assert.IsType<ZLinkRelocationCapacityReserveResult.Reserved>(
            await fixture.Store.ReserveRelocationCapacityAsync(
                new ZLinkRelocationCapacityReservationRequest(
                    Guid.NewGuid(),
                    ZLinkActorAuthorityPayloadCodec.AuthorityKey(ActorId),
                    sourceSnapshot.StoreVersion,
                    ZLinkPlacementObjectKind.Actor,
                    sourceAuthority.StableType,
                    new ZLinkMeshNodeDescriptorKey(MeshName, nodeA.NodeRid),
                    1,
                    sourceOwner,
                    new ZLinkMeshNodeDescriptorKey(MeshName, nodeB.NodeRid),
                    1,
                    targetOwner,
                    sourceSnapshot.Allocation.Capacity)));
        var targetPayload = ZLinkActorAuthorityPayloadCodec.Encode(
            sourceAuthority with
            {
                NodeRid = nodeB.NodeRid,
                NodeGeneration = 1,
                OwnerId = targetOwner.OwnerId,
                OwnerLeaseGeneration = checked((ulong)targetOwner.LeaseGeneration)
            });

        Assert.IsType<ZLinkAuthorityCompareExchangeResult.Stored>(
            await fixture.Store.CompareExchangeAuthorityAsync(
                ZLinkActorAuthorityPayloadCodec.AuthorityKey(ActorId),
                sourceSnapshot.StoreVersion,
                new ZLinkAuthorityMutation.Put(
                    targetPayload,
                    ZLinkAuthorityGenerationTransition.NewOwner,
                    targetOwner,
                    capacity.Fence)));

        await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await nodeA.ActorOwnership.NotifyActorJoinedSpotAsync(
                ActorId, "spot-1", spotGeneration: 1));
        await deactivated.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.False(nodeA.ActorOwnership.OwnsActor(ActorId));
        var currentSnapshot = Assert.IsType<ZLinkAuthorityReadResult.Found>(
            await fixture.Store.ReadAuthorityAsync(
                ZLinkActorAuthorityPayloadCodec.AuthorityKey(ActorId))).Snapshot;
        Assert.True(
            currentSnapshot.AuthorityOwnerGeneration
                > sourceSnapshot.AuthorityOwnerGeneration,
            $"source={sourceSnapshot.AuthorityOwnerGeneration}, current={currentSnapshot.AuthorityOwnerGeneration}");
        Assert.Equal(sourceSnapshot.ObjectGeneration, currentSnapshot.ObjectGeneration);
        Assert.True(
            ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
                currentSnapshot.Payload.Span,
                out var currentAuthority));
        Assert.NotEqual(sourceAuthority.NodeRid, currentAuthority.NodeRid);
        var removeCallsBeforeCleanup = controlled.RemoveCalls;

        // The ownership-loss callback removed local tracking before the
        // committed handoff cleanup ran. Cleanup must still submit the old
        // conditional delete and let the Store fence it against node B.
        await nodeA.ActorOwnership.ReleaseActorAfterMoveAsync(
            ActorId,
            sourceSnapshot);

        Assert.True(
            controlled.RemoveCalls - removeCallsBeforeCleanup == 1,
            $"before={removeCallsBeforeCleanup}, after={controlled.RemoveCalls}");
        var row = await nodeB.Resolvers.ResolveActorRowAsync(
            new ZLinkActorLocationKey(ActorId));
        Assert.Equal(nodeB.Runtime.OwnerId, row!.OwnerId);
        Assert.Equal(RoutingId.From("node-b"), row.OwnerNodeRid);
    }

    [Fact]
    public async Task OwnershipLost_Deactivates_The_Hosted_Actor_Exactly_Once()
    {
        await using var fixture = await LifecycleFixture.CreateAsync();
        var nodeA = await fixture.NodeAsync("node-a");
        var nodeB = await fixture.NodeAsync("node-b");
        var deactivated = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);

        await CreateTrackedActorAsync(
            nodeA,
            _ =>
            {
                deactivated.TrySetResult();
                return ValueTask.CompletedTask;
            });

        // Node B fences the row away (unplanned takeover). Node A only
        // learns about it when its next write comes back IgnoredStale.
        var takeover = await AuthorityLocationTestFixture.PublishActorAsync(
            fixture.Store,
            InMemoryLocationStoreTests.Actor(nodeB.Runtime.OwnerId),
            replace: true);
        Assert.NotNull(takeover);

        var stale = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await nodeA.ActorOwnership.NotifyActorJoinedSpotAsync(
                ActorId, "spot-1", spotGeneration: 1));
        Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, stale.Kind);

        await deactivated.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.False(nodeA.ActorOwnership.OwnsActor(ActorId));

        // The stale owner must not be able to damage the new row.
        await nodeA.ActorOwnership.ReleaseActorAsync(ActorId);
        var row = await nodeB.Resolvers.ResolveActorRowAsync(
            new ZLinkActorLocationKey(ActorId));
        Assert.Equal(nodeB.Runtime.OwnerId, row!.OwnerId);
    }

    [Fact]
    public async Task LocationLifecycle_DisposeAsync_WaitsForOwnershipLossDeactivation_And_IsIdempotent()
    {
        await using var fixture = await LifecycleFixture.CreateAsync();
        var nodeA = await fixture.NodeAsync("node-a");
        var nodeB = await fixture.NodeAsync("node-b");
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var completed = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);

        var releaseDeactivation = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        await CreateTrackedActorAsync(
            nodeA,
            async _ =>
            {
                started.TrySetResult();
                await releaseDeactivation.Task;
                completed.TrySetResult();
            });
        await AuthorityLocationTestFixture.PublishActorAsync(
            fixture.Store,
            InMemoryLocationStoreTests.Actor(nodeB.Runtime.OwnerId),
            replace: true);

        await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await nodeA.ActorOwnership.NotifyActorJoinedSpotAsync(
                ActorId, "spot-1", spotGeneration: 1));
        await started.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var firstDispose = nodeA.Lifecycle.DisposeAsync().AsTask();
        var secondDispose = nodeA.Lifecycle.DisposeAsync().AsTask();
        Assert.Same(firstDispose, secondDispose);
        await Task.Delay(30);
        Assert.False(firstDispose.IsCompleted);

        releaseDeactivation.TrySetResult();
        await firstDispose.WaitAsync(TimeSpan.FromSeconds(5));
        await completed.Task.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task Spot_Handle_Resolver_Uses_The_Row_And_Reports_Unavailable_When_The_Lease_Expires()
    {
        await using var fixture = await LifecycleFixture.CreateAsync();
        var node = await fixture.NodeAsync("node-a");
        const string spotId = "spot-9";

        var resolver = new ZLinkLocationAddressResolvers(
            node.Resolvers,
            new ZLinkSpotHandleRegistry());

        var spot = await PublishReadySpotAsync(fixture, node, spotId, 7);
        var status = await node.SpotLocations.ClaimAsync(
            "mesh",
            spotId,
            7,
            "game",
            RoutingId.From("node-a"),
            1,
            ZLinkSpotKind.User,
            authorityOwnerGeneration: spot.AuthorityOwnerGeneration,
            deactivate: null);
        Assert.Equal(ZLinkLocationWriteStatus.Stored, status);

        var handle = Assert.IsType<ZLinkResolvedSpotHandle>(
            await resolver.ResolveSpotHandleAsync(spotId, CancellationToken.None));
        Assert.Equal("mesh", handle.Snapshot.RouterChannelId);
        Assert.Equal(RoutingId.From("node-a"), handle.Snapshot.NodeRid);
        Assert.Equal(spotId, handle.SpotId);

        // No heartbeat: once the owner lease expires the durable row remains,
        // but its route cannot be used. Report Unavailable instead of exposing
        // a stale node or treating the known authority as NotFound.
        fixture.Time.Advance(fixture.Options.OwnerLeaseTtl + TimeSpan.FromSeconds(1));

        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await resolver.ResolveSpotHandleAsync(spotId, CancellationToken.None));
        Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, error.Kind);
    }

    [Fact]
    public async Task Actor_Reconnect_Refreshes_The_Location_And_Never_Activates_A_Second_Instance()
    {
        await using var fixture = await LifecycleFixture.CreateAsync();
        var nodeA = await fixture.NodeAsync("node-a");
        var nodeB = await fixture.NodeAsync("node-b");
        var nodeC = await fixture.NodeAsync("node-c");
        var key = new ZLinkActorLocationKey(ActorId);

        var activation = await nodeA.ActorOwnership.ExecuteActorClaimThenActivateAsync(
            MeshName, ActorType, ActorId, RoutingId.From("node-a"),
            deactivate: null,
            activate: async cancellationToken =>
            {
                var claimed = await nodeA.Resolvers.ResolveActorRowAsync(
                    key,
                    cancellationToken);
                Assert.NotNull(claimed);
                Assert.Equal(nodeA.Runtime.OwnerId, claimed.OwnerId);
                Assert.Equal(1UL, claimed.ActorRef.ObjectGeneration);
                return "instance-a";
            },
            CancellationToken.None);
        Assert.Equal("instance-a", activation.Activated);

        // Node B has resolved the actor before and would otherwise remember
        // node A.
        var cached = await nodeB.Resolvers.ResolveActorRowAsync(key);
        Assert.Equal(RoutingId.From("node-a"), cached!.OwnerNodeRid);

        // The actor moves to node C behind node B's earlier read.
        await AuthorityLocationTestFixture.PublishActorAsync(
            fixture.Store,
            InMemoryLocationStoreTests.Actor("ignored") with
            {
                OwnerId = nodeC.Runtime.OwnerId,
                OwnerNodeRid = RoutingId.From("node-c"),
                ActorRef = new ActorRef(ActorId, 1, MeshName, RoutingId.From("node-c")),
                MembershipEpoch = 1
            },
            replace: true);
        var refreshed = await nodeB.Resolvers.ResolveActorRowAsync(key);
        Assert.Equal(RoutingId.From("node-c"), refreshed!.OwnerNodeRid);

        // Reconnect at node B: the claim conflicts, the existing location
        // is re-read from the store (never from a stale cache), and no
        // local instance is activated.
        var activatedB = 0;
        var reconnect = await nodeB.ActorOwnership.ExecuteActorClaimThenActivateAsync<string>(
            MeshName, ActorType, ActorId, RoutingId.From("node-b"),
            deactivate: null,
            activate: _ =>
            {
                activatedB++;
                return ValueTask.FromResult("instance-b");
            },
            CancellationToken.None);

        Assert.Null(reconnect.Activated);
        Assert.Equal(0, activatedB);
        Assert.Equal(RoutingId.From("node-c"), reconnect.ExistingLocation!.OwnerNodeRid);

        var after = await nodeC.Resolvers.ResolveActorRowAsync(key);
        Assert.Equal(nodeC.Runtime.OwnerId, after!.OwnerId);
    }

    private static async ValueTask CreateTrackedActorAsync(
        LifecycleNode node,
        Func<CancellationToken, ValueTask>? deactivate = null)
    {
        var activation = await node.ActorOwnership.ExecuteActorClaimThenActivateAsync(
            MeshName,
            ActorType,
            ActorId,
            RoutingId.From("node-a"),
            deactivate,
            static _ => ValueTask.FromResult(new object()),
            CancellationToken.None);
        Assert.NotNull(activation.Activated);
        Assert.Null(activation.ExistingLocation);
    }

    private static async ValueTask<ZLinkAuthoritySnapshot> PublishReadySpotAsync(
        LifecycleFixture fixture,
        LifecycleNode node,
        string spotId,
        ulong spotGeneration,
        bool replace = false)
    {
        return Assert.IsType<ZLinkAuthoritySnapshot>(
            await AuthorityLocationTestFixture.PublishSpotAsync(
                fixture.Store,
                InMemoryLocationStoreTests.Spot(
                    node.Runtime.OwnerId,
                    spotId) with
                {
                    MeshName = "mesh",
                    SpotType = "game",
                    OwnerNodeRid = node.NodeRid,
                    OwnerNodeGeneration = 1,
                    SpotGeneration = spotGeneration
                },
                replace));
    }

    private static string CreateEntrySpotId(string meshName, RoutingId nodeRid)
    {
        var hex = Convert.ToHexString(
                System.Security.Cryptography.SHA256.HashData(nodeRid.ToBytes()))
            .ToLowerInvariant();
        return $"{meshName}-entry-{hex[..8]}-{hex[8..12]}"
               + $"-4{hex[13..16]}-a{hex[17..20]}-{hex[20..32]}";
    }

    private sealed class LifecycleFixture : IAsyncDisposable
    {
        private readonly List<LifecycleNode> _nodes = [];

        private LifecycleFixture(
            ZLinkInMemoryLocationStore store,
            ManualTimeProvider time,
            ZLinkLocationOptions options)
        {
            Store = store;
            Time = time;
            Options = options;
        }

        public ZLinkInMemoryLocationStore Store { get; }

        public ManualTimeProvider Time { get; }

        public ZLinkLocationOptions Options { get; }

        public static Task<LifecycleFixture> CreateAsync()
        {
            var time = new ManualTimeProvider();
            var store = new ZLinkInMemoryLocationStore(time);
            var options = new ZLinkLocationOptions
            {
                // Keep the lease snapshot maximally fresh so expiry is
                // observed on the next read.
                PollingInterval = TimeSpan.Zero,
                RouteCacheMaxAge = TimeSpan.Zero
            };
            return Task.FromResult(new LifecycleFixture(store, time, options));
        }

        public async Task<LifecycleNode> NodeAsync(
            string nodeRid,
            ControlledActorStore? actorStore = null)
        {
            IZLinkLocationRepository locationStore = actorStore is null
                ? Store
                : actorStore;
            var runtime = new ZLinkLocationRuntime(
                Options,
                locationStore,
                Time);
            // A single lease renewal instead of StartAsync keeps the
            // heartbeat loop out of the test so lease expiry is driven by
            // the manual clock alone.
            Assert.True(await runtime.RenewOwnerLeaseOnceAsync());
            var owner = runtime.OwnerToken;
            var descriptor = InMemoryLocationStoreTests.MeshNode(
                owner.OwnerId,
                "tcp://127.0.0.1:5001",
                nodeRid,
                leaseGeneration: owner.LeaseGeneration) with
            {
                ObjectRole = ZLinkMeshNodeObjectRole.Server,
                EntrySpotId = CreateEntrySpotId(MeshName, RoutingId.From(nodeRid)),
                ObjectCapabilities =
                [
                    new ZLinkObjectCapability(
                        ZLinkPlacementObjectKind.Actor,
                        ActorType,
                        ZLinkObjectMaintenancePolicyKind.Disabled,
                        false,
                        0),
                    new ZLinkObjectCapability(
                        ZLinkPlacementObjectKind.UserSpot,
                        "game",
                        ZLinkObjectMaintenancePolicyKind.Disabled,
                        false,
                        0)
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
            var existingDescriptor = (await Store.ListMeshNodesAsync(
                    MeshName,
                    default)).Items
                .SingleOrDefault(candidate => candidate.Rid == descriptor.Rid);
            if (existingDescriptor is not null
                && (existingDescriptor.OwnerId != owner.OwnerId
                    || existingDescriptor.LeaseGeneration != owner.LeaseGeneration))
            {
                Assert.Equal(
                    ZLinkLocationWriteStatus.Stored,
                    await Store.RemoveMeshNodeAsync(
                        new ZLinkMeshNodeDescriptorKey(
                            existingDescriptor.MeshName,
                            existingDescriptor.Rid),
                        new ZLinkLocationOwnerToken(
                            existingDescriptor.OwnerId,
                            existingDescriptor.LeaseGeneration)));
                existingDescriptor = null;
            }
            if (existingDescriptor is not null)
                descriptor = descriptor with
                {
                    DescriptorRevision = checked(
                        existingDescriptor.DescriptorRevision + 1)
                };
            Assert.Equal(
                ZLinkLocationWriteStatus.Stored,
                (await Store.UpdateMeshNodeAsync(
                    descriptor,
                    existingDescriptor is null
                        ? ZLinkLocationWriteIntent.NewClaim
                        : ZLinkLocationWriteIntent.Renew)).Status);
            if (await Store.ReadAuthorityAsync(
                    ZLinkActorAuthorityPayloadCodec.AuthorityKey(ActorId))
                is ZLinkAuthorityReadResult.Missing)
            {
                await AuthorityLocationTestFixture.PublishActorAsync(
                    Store,
                    InMemoryLocationStoreTests.Actor(owner.OwnerId, ActorId) with
                    {
                        ActorType = ActorType,
                        MeshName = MeshName,
                        OwnerNodeRid = RoutingId.From(nodeRid),
                        OwnerNodeGeneration = 1,
                        ActorRef = new ActorRef(
                            ActorId,
                            1,
                            MeshName,
                            RoutingId.From(nodeRid)),
                        SpotId = descriptor.EntrySpotId!,
                        SpotGeneration = descriptor.LifecycleGeneration,
                        MembershipEpoch = 1
                    });
            }
            var tracker = new ZLinkOwnerLeaseTracker(
                locationStore,
                Options,
                Time);
            var observed = new ZLinkObservedLocationGenerations();
            var resolvers = new ZLinkStoreLocationResolvers(
                locationStore,
                tracker,
                observed,
                options: Options,
                timeProvider: Time);
            var query = new ZLinkLocationRuntimeQueryService(
                Options,
                locationStore,
                RegisteredMeshes,
                tracker,
                runtime,
                observed);
            var node = new LifecycleNode(
                RoutingId.From(nodeRid),
                runtime,
                resolvers,
                query,
                new ZLinkLocationLifecycle(runtime, resolvers));
            _nodes.Add(node);
            return node;
        }

        private static string CreateEntrySpotId(string meshName, RoutingId nodeRid)
        {
            var hex = Convert.ToHexString(
                    System.Security.Cryptography.SHA256.HashData(nodeRid.ToBytes()))
                .ToLowerInvariant();
            return $"{meshName}-entry-{hex[..8]}-{hex[8..12]}"
                   + $"-4{hex[13..16]}-a{hex[17..20]}-{hex[20..32]}";
        }

        public async ValueTask DisposeAsync()
        {
            for (var index = _nodes.Count - 1; index >= 0; index--)
            {
                await _nodes[index].Lifecycle.DisposeAsync();
                await _nodes[index].Runtime.DisposeAsync();
            }
            _nodes.Clear();
        }
    }

    private sealed record LifecycleNode(
        RoutingId NodeRid,
        ZLinkLocationRuntime Runtime,
        ZLinkStoreLocationResolvers Resolvers,
        IZLinkLocationRuntimeQuery Query,
        ZLinkLocationLifecycle Lifecycle)
    {
        public ZLinkSpotLocationLifecycle SpotLocations => Lifecycle.SpotLocations;

        public ZLinkActorOwnershipCoordinator ActorOwnership => Lifecycle.ActorOwnership;
    }

    private sealed class ControlledActorStore(
        ZLinkInMemoryLocationStore inner) : ZLinkLocationStoreTestDouble
    {
        public int RejectNewClaimCount { get; set; }

        public bool RejectNextRenew { get; set; }

        public TaskCompletionSource? RenewGate { get; init; }

        public TaskCompletionSource? RemoveGate { get; init; }

        public TaskCompletionSource RenewStarted { get; } = new(
            TaskCreationOptions.RunContinuationsAsynchronously);

        public TaskCompletionSource RemoveStarted { get; } = new(
            TaskCreationOptions.RunContinuationsAsynchronously);

        public int RemoveCalls { get; private set; }

        public Exception? RemoveFailure { get; set; }

        public ZLinkLocationOwnerToken? LastRemoveOwner { get; private set; }

        public override async ValueTask<ZLinkObjectReserveResult> ReserveAsync(
            ZLinkObjectReservationRequest request,
            CancellationToken cancellationToken = default)
        {
            if (request.ObjectKind == ZLinkPlacementObjectKind.Actor
                && RejectNewClaimCount > 0)
            {
                RejectNewClaimCount--;
                return new ZLinkObjectReserveResult.Conflict(
                    await inner.ReadAuthorityAsync(
                        request.Key,
                        cancellationToken));
            }

            return await inner.ReserveAsync(request, cancellationToken);
        }

        public override async ValueTask<ZLinkAuthorityCompareExchangeResult>
            CompareExchangeAuthorityAsync(
                ZLinkAuthorityKey key,
                string expectedStoreVersion,
                ZLinkAuthorityMutation mutation,
                CancellationToken cancellationToken = default)
        {
            if (mutation is ZLinkAuthorityMutation.Put
                {
                    GenerationTransition:
                    ZLinkAuthorityGenerationTransition.Preserve
                })
            {
                RenewStarted.TrySetResult();
                if (RenewGate is not null)
                    await RenewGate.Task.WaitAsync(cancellationToken);
                // A rejection has to outlast the renew retry budget. Clearing
                // the flag after one attempt models a transient compare-exchange
                // race, which the coordinator is required to rebase and retry,
                // so the renew would succeed and the rejected membership would
                // reach the committed base.
                if (RejectNextRenew)
                {
                    return new ZLinkAuthorityCompareExchangeResult.Conflict(
                        await inner.ReadAuthorityAsync(
                            key,
                            cancellationToken));
                }
            }

            if (mutation is ZLinkAuthorityMutation.Delete)
            {
                RemoveCalls++;
                if (await inner.ReadAuthorityAsync(
                        key,
                        cancellationToken)
                    is ZLinkAuthorityReadResult.Found found)
                {
                    LastRemoveOwner = new ZLinkLocationOwnerToken(
                        found.Snapshot.OwnerId,
                        checked((long)found.Snapshot.AuthorityOwnerGeneration));
                }
                RemoveStarted.TrySetResult();
                if (RemoveGate is not null)
                    await RemoveGate.Task.WaitAsync(cancellationToken);
                if (RemoveFailure is { } failure)
                {
                    RemoveFailure = null;
                    throw failure;
                }
            }

            return await inner.CompareExchangeAuthorityAsync(
                key,
                expectedStoreVersion,
                mutation,
                cancellationToken);
        }

        public override ValueTask<ZLinkAuthorityReadResult> ReadAuthorityAsync(
            ZLinkAuthorityKey key,
            CancellationToken cancellationToken = default) =>
            inner.ReadAuthorityAsync(key, cancellationToken);

        public override ValueTask<ZLinkAuthorityScanResult> ListAuthoritiesAsync(
            string prefix,
            ZLinkAuthorityScanCursor? cursor,
            int limit,
            CancellationToken cancellationToken = default) =>
            inner.ListAuthoritiesAsync(prefix, cursor, limit, cancellationToken);

        public override ValueTask<ZLinkObjectCommitResult> CommitAsync(
            ZLinkObjectReservation reservation,
            ReadOnlyMemory<byte> readyPayload,
            CancellationToken cancellationToken = default) =>
            inner.CommitAsync(reservation, readyPayload, cancellationToken);

        public override ValueTask<ZLinkObjectCreationCompleteResult> CompleteCreationAsync(
            ZLinkObjectReservation reservation,
            ZLinkObjectCreationCompletion completion,
            CancellationToken cancellationToken = default) =>
            inner.CompleteCreationAsync(
                reservation,
                completion,
                cancellationToken);

        public override ValueTask<ZLinkCreationTerminalReadResult>
            ReadCreationTerminalAsync(
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

        public override ValueTask<ZLinkLocationWriteResult> UpdateMeshNodeAsync(
            ZLinkMeshNodeDescriptor descriptor,
            ZLinkLocationWriteIntent intent,
            CancellationToken cancellationToken = default) =>
            inner.UpdateMeshNodeAsync(descriptor, intent, cancellationToken);

        public override ValueTask<ZLinkLocationWriteStatus> RemoveMeshNodeAsync(
            ZLinkMeshNodeDescriptorKey key,
            ZLinkLocationOwnerToken owner,
            CancellationToken cancellationToken = default) =>
            inner.RemoveMeshNodeAsync(key, owner, cancellationToken);

        public override ValueTask<ZLinkLocationPage<ZLinkMeshNodeDescriptor>>
            ListMeshNodesAsync(
                string meshName,
                ZLinkPageRequest page,
                CancellationToken cancellationToken = default) =>
            inner.ListMeshNodesAsync(meshName, page, cancellationToken);

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
            CancellationToken cancellationToken = default) =>
            inner.RenewOwnerLeaseAsync(token, leaseTtl, cancellationToken);

        public override ValueTask<ZLinkOwnerLeaseReleaseResult> ReleaseOwnerLeaseAsync(
            ZLinkLocationOwnerToken token,
            CancellationToken cancellationToken = default) =>
            inner.ReleaseOwnerLeaseAsync(token, cancellationToken);

        public override ValueTask<long> RemoveAllByOwnerAsync(
            ZLinkLocationOwnerToken owner,
            CancellationToken cancellationToken = default) =>
            inner.RemoveAllByOwnerAsync(owner, cancellationToken);
    }
}
