using System.Security.Cryptography;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

public sealed class StandaloneActorRelocationPrecommitTests
{
    [Fact]
    public async Task Preparing_Captured_Prepared_and_NewOwner_are_durable()
    {
        var store = new ZLinkInMemoryLocationStore();
        var sourceOwner = Assert.IsType<ZLinkOwnerLeaseClaimResult.Claimed>(
            await store.ClaimOwnerLeaseAsync(
                "precommit-source",
                TimeSpan.FromMinutes(1))).Token;
        var targetOwner = Assert.IsType<ZLinkOwnerLeaseClaimResult.Claimed>(
            await store.ClaimOwnerLeaseAsync(
                "precommit-target",
                TimeSpan.FromMinutes(1))).Token;
        var source = Descriptor(
            RoutingId.From("precommit-source"),
            sourceOwner);
        var target = Descriptor(
            RoutingId.From("precommit-target"),
            targetOwner);
        await store.UpdateMeshNodeAsync(
            source,
            ZLinkLocationWriteIntent.NewClaim);
        await store.UpdateMeshNodeAsync(
            target,
            ZLinkLocationWriteIntent.NewClaim);
        var actorId = $"actor-{Guid.NewGuid():N}";
        var key = ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId);
        var reservation = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await store.ReserveAsync(
                new ZLinkObjectReservationRequest(
                    ZLinkPlacementObjectKind.Actor,
                    key,
                    "Game.Actor",
                    $"intent:{actorId}",
                    SHA256.HashData("intent"u8),
                    6,
                    new ZLinkMeshNodeDescriptorKey("mesh", source.Rid),
                    source.LifecycleGeneration,
                    sourceOwner,
                    new byte[] { 1 },
                    new ZLinkCapacityVector(1, 0, null))));
        var sourceAuthority = Authority(actorId, source, sourceOwner);
        var ready = Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await store.CommitAsync(
                reservation.Reservation,
                ZLinkActorAuthorityPayloadCodec.Encode(sourceAuthority)));
        var relocationId = Guid.NewGuid();
        var lossyStore = new LostStoredResponseAuthorityStore(store);
        var coordinator = new ZLinkStandaloneActorRelocationPrecommitCoordinator(
            lossyStore);

        lossyStore.LoseNextResponse();
        var preparing = await coordinator.BeginPreparingAsync(
            ready.Snapshot,
            sourceAuthority,
            relocationId,
            applicationVersion: 1,
            CancellationToken.None);
        var preparingState = Projection(preparing);
        Assert.Equal(1, preparingState.Phase);
        Assert.Empty(preparingState.RelocationReference);
        Assert.Equal(0UL, preparingState.TargetAttemptGeneration);
        Assert.Equal(sourceOwner.OwnerId, preparing.OwnerId);

        var abandoned = await coordinator.AbortPreparingAsync(
            key,
            relocationId,
            CancellationToken.None);
        Assert.False(ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
            abandoned.Payload.Span,
            out _));

        lossyStore.LoseNextResponse();
        preparing = await coordinator.BeginPreparingAsync(
            abandoned,
            sourceAuthority,
            relocationId,
            applicationVersion: 1,
            CancellationToken.None);

        var envelope = ZLinkCanonicalActorRelocationWriter.CreateInitial(
            ZLinkStandaloneActorRelocationRuntime.CreateImmutableRoot(
                preparing,
                sourceAuthority,
                target,
                relocationId,
                ReadOnlyMemory<byte>.Empty,
                [],
                default),
            applicationVersion: 1);
        var stored = new ZLinkRelocationStored(
            "precommit-root",
            0x12345678,
            DateTimeOffset.UtcNow + TimeSpan.FromHours(24),
            DateTimeOffset.UtcNow);
        lossyStore.LoseNextResponse();
        var captured = await coordinator.CaptureAsync(
            preparing,
            envelope,
            stored,
            CancellationToken.None);
        var capturedState = Projection(captured);
        Assert.Equal(2, capturedState.Phase);
        Assert.Equal(stored.Reference, capturedState.RelocationReference);
        Assert.Equal(0UL, capturedState.TargetAttemptGeneration);

        var prepare = ZLinkStandaloneActorRelocationRuntime.CreatePrepare(
            captured,
            sourceAuthority,
            target,
            envelope,
            stored,
            boundSession: null,
            applicationVersion: 1);
        var capacity = Assert.IsType<
            ZLinkRelocationCapacityReserveResult.Reserved>(
            await store.ReserveRelocationCapacityAsync(
                new ZLinkRelocationCapacityReservationRequest(
                    relocationId,
                    key,
                    captured.StoreVersion,
                    ZLinkPlacementObjectKind.Actor,
                    "Game.Actor",
                    new ZLinkMeshNodeDescriptorKey("mesh", source.Rid),
                    source.LifecycleGeneration,
                    sourceOwner,
                    new ZLinkMeshNodeDescriptorKey("mesh", target.Rid),
                    target.LifecycleGeneration,
                    targetOwner,
                    new ZLinkCapacityVector(1, 0, null))));
        var finalEnvelope = ZLinkCanonicalActorRelocationWriter.CreateInitial(
            envelope with
            {
                Participants =
                [
                    envelope.Participants.Single() with
                    {
                        ApplicationState = new byte[] { 7 }
                    }
                ]
            },
            applicationVersion: 1);
        var storedFinal = stored with
        {
            Reference = "precommit-final-root",
            ChecksumCrc32c = 0x87654321
        };
        lossyStore.LoseNextResponse();
        var refreshedCaptured = await coordinator.RefreshCapturedRootAsync(
            captured,
            finalEnvelope,
            storedFinal,
            capacity.Fence,
            CancellationToken.None);
        var refreshedState = Projection(refreshedCaptured);
        Assert.Equal(2, refreshedState.Phase);
        Assert.Equal(storedFinal.Reference, refreshedState.RelocationReference);
        Assert.Equal(0UL, refreshedState.TargetAttemptGeneration);

        lossyStore.LoseNextResponse();
        var prepared = await coordinator.PrepareTargetAsync(
            refreshedCaptured,
            finalEnvelope,
            capacity.Fence,
            prepare,
            reservationGeneration: 7,
            CancellationToken.None);
        var preparedState = Projection(prepared);
        Assert.Equal(3, preparedState.Phase);
        Assert.Equal(1UL, preparedState.TargetAttemptGeneration);
        Assert.Equal(target.Rid.ToHex(), preparedState.State.TargetNodeRid);
        Assert.Equal(sourceOwner.OwnerId, prepared.OwnerId);

        var targetAuthority = Authority(actorId, target, targetOwner);
        lossyStore.ReturnAuxiliaryConflictNext();
        lossyStore.LoseNextResponse();
        var committed = await coordinator.CommitTargetAsync(
            prepared,
            finalEnvelope,
            capacity.Fence,
            targetAuthority,
            targetOwner,
            CancellationToken.None);
        var committedState = Projection(committed);
        Assert.Equal(4, committedState.Phase);
        Assert.Equal(targetOwner.OwnerId, committed.OwnerId);
        Assert.Equal(ready.Snapshot.ObjectGeneration, committed.ObjectGeneration);
        Assert.Equal(
            checked(ready.Snapshot.AuthorityOwnerGeneration + 1),
            committed.AuthorityOwnerGeneration);
        Assert.Equal(
            ZLinkRelocationCapacityAbortResult.AlreadyCommitted,
            await store.AbortRelocationCapacityAsync(capacity.Fence));
    }

    [Fact]
    public async Task Startup_recovery_aborts_exact_preparing_after_source_lease_expires()
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryLocationStore(time);
        var sourceOwner = Assert.IsType<ZLinkOwnerLeaseClaimResult.Claimed>(
            await store.ClaimOwnerLeaseAsync(
                "expired-preparing-source",
                TimeSpan.FromMinutes(1))).Token;
        var source = Descriptor(
            RoutingId.From("expired-preparing-source"),
            sourceOwner);
        await store.UpdateMeshNodeAsync(
            source,
            ZLinkLocationWriteIntent.NewClaim);
        var actorId = $"actor-{Guid.NewGuid():N}";
        var key = ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId);
        var reservation = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await store.ReserveAsync(
                new ZLinkObjectReservationRequest(
                    ZLinkPlacementObjectKind.Actor,
                    key,
                    "Game.Actor",
                    $"intent:{actorId}",
                    SHA256.HashData("intent"u8),
                    6,
                    new ZLinkMeshNodeDescriptorKey("mesh", source.Rid),
                    source.LifecycleGeneration,
                    sourceOwner,
                    new byte[] { 1 },
                    new ZLinkCapacityVector(1, 0, null))));
        var sourceAuthority = Authority(actorId, source, sourceOwner);
        var ready = Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await store.CommitAsync(
                reservation.Reservation,
                ZLinkActorAuthorityPayloadCodec.Encode(sourceAuthority)));
        var relocationId = Guid.NewGuid();
        var coordinator = new ZLinkStandaloneActorRelocationPrecommitCoordinator(
            store);
        _ = await coordinator.BeginPreparingAsync(
            ready.Snapshot,
            sourceAuthority,
            relocationId,
            applicationVersion: 1,
            CancellationToken.None);

        time.Advance(TimeSpan.FromMinutes(2));
        var recovered = await coordinator.AbortPreparingAsync(
            key,
            relocationId,
            CancellationToken.None);

        Assert.False(ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
            recovered.Payload.Span,
            out _));
        Assert.Equal(sourceOwner.OwnerId, recovered.OwnerId);
        Assert.Equal(sourceOwner.LeaseGeneration,
            recovered.OwnerLeaseGeneration);
        Assert.Equal(ready.Snapshot.ObjectGeneration,
            recovered.ObjectGeneration);
        Assert.Equal(ready.Snapshot.AuthorityOwnerGeneration,
            recovered.AuthorityOwnerGeneration);
    }

    [Fact]
    public void Precommit_closed_fields_reject_target_or_root_in_wrong_phase()
    {
        var actor = Authority(
            "actor",
            Descriptor(
                RoutingId.From("closed-source"),
                new ZLinkLocationOwnerToken("source", 1)),
            new ZLinkLocationOwnerToken("source", 1));
        var payload = ZLinkActorAuthorityPayloadCodec.Encode(actor);
        var state = new ZLinkCanonicalRelocationAuthorityState(
            1, 2, 1,
            actor.NodeRid.ToHex(), actor.NodeGeneration,
            actor.OwnerId, actor.OwnerLeaseGeneration,
            "target", 1, "target-owner", 1,
            1, actor.OwnerId, actor.OwnerLeaseGeneration,
            actor.NodeRid.ToHex(), actor.NodeGeneration,
            1, string.Empty, 0, 1, 0);

        Assert.Throws<ArgumentException>(() =>
            ZLinkCanonicalRelocationAuthorityStateCodec.ReplaceRelocationState(
                payload,
                state,
                root: null));
    }

    private static ZLinkCanonicalRelocationAuthorityProjection Projection(
        ZLinkAuthoritySnapshot snapshot)
    {
        Assert.True(ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
            snapshot.Payload.Span,
            out var projection));
        return projection;
    }

    private static ZLinkActorAuthorityPayload Authority(
        string actorId,
        ZLinkMeshNodeDescriptor descriptor,
        ZLinkLocationOwnerToken owner) => new(
        ZLinkActorAuthorityState.Ready,
        "Game.Actor",
        actorId,
        descriptor.EntrySpotId!,
        descriptor.LifecycleGeneration,
        ZLinkSpotKind.Entry,
        owner.OwnerId,
        checked((ulong)owner.LeaseGeneration),
        descriptor.MeshName,
        descriptor.Rid,
        descriptor.LifecycleGeneration);

    private static ZLinkMeshNodeDescriptor Descriptor(
        RoutingId rid,
        ZLinkLocationOwnerToken owner) => new(
        "mesh",
        rid,
        1,
        1,
        $"inproc://{rid.ToHex()}",
        new Dictionary<string, int> { ["mesh"] = 100 },
        string.Empty,
        owner.OwnerId,
        owner.LeaseGeneration,
        DateTimeOffset.UtcNow)
    {
        ObjectRole = ZLinkMeshNodeObjectRole.Server,
        State = ZLinkFrameworkRuntimeState.Serving,
        EntrySpotId = $"{rid.ToHex()}-entry-00000000-0000-4000-8000-000000000001",
        ObjectCapabilities =
        [
            new ZLinkObjectCapability(
                ZLinkPlacementObjectKind.Actor,
                "Game.Actor",
                ZLinkObjectMaintenancePolicyKind.Recreate,
                false,
                0)
        ],
        Capacity = new ZLinkPlacementCapacity(
            new ZLinkPopulationCapacity(0, 0, 100),
            new ZLinkPopulationCapacity(0, 0, 100),
            [])
    };

    private sealed class LostStoredResponseAuthorityStore(
        IZLinkLocationRepository inner) : ZLinkLocationStoreTestDouble
    {
        private int _loseNext;
        private int _conflictNext;

        internal void LoseNextResponse() =>
            Interlocked.Exchange(ref _loseNext, 1);

        internal void ReturnAuxiliaryConflictNext() =>
            Interlocked.Exchange(ref _conflictNext, 1);

        public override ValueTask<ZLinkAuthorityReadResult> ReadAuthorityAsync(
            ZLinkAuthorityKey key,
            CancellationToken cancellationToken = default) =>
            inner.ReadAuthorityAsync(key, cancellationToken);

        public override async ValueTask<ZLinkAuthorityCompareExchangeResult>
            CompareExchangeAuthorityAsync(
                ZLinkAuthorityKey key,
                string expectedStoreVersion,
                ZLinkAuthorityMutation mutation,
                CancellationToken cancellationToken = default)
        {
            if (Interlocked.Exchange(ref _conflictNext, 0) == 1)
                return new ZLinkAuthorityCompareExchangeResult.Conflict(
                    await inner.ReadAuthorityAsync(key, cancellationToken)
                        .ConfigureAwait(false));
            var result = await inner.CompareExchangeAuthorityAsync(
                    key,
                    expectedStoreVersion,
                    mutation,
                    cancellationToken)
                .ConfigureAwait(false);
            if (result is ZLinkAuthorityCompareExchangeResult.Stored
                && Interlocked.Exchange(ref _loseNext, 0) == 1)
                throw new IOException("The stored CAS response was lost.");
            return result;
        }

    }
}
