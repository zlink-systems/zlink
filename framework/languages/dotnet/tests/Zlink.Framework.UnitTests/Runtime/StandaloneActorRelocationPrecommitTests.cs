using System.Security.Cryptography;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

public sealed class StandaloneActorRelocationPrecommitTests
{
    [Fact]
    public async Task Precommit_retries_owner_lease_heartbeat_conflict_and_completes()
    {
        var provider = new OwnerLeaseHeartbeatLocationStore(
            new ZLinkInMemoryProviderLocationStore(),
            "heartbeat-source");
        var store = new ZLinkProviderLocationRepository(provider);
        var sourceOwner = Assert.IsType<ZLinkOwnerLeaseClaimResult.Claimed>(
            await store.ClaimOwnerLeaseAsync(
                "heartbeat-source",
                TimeSpan.FromMinutes(1))).Token;
        var source = Descriptor(
            RoutingId.From("heartbeat-source"),
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
        provider.ArmHeartbeatBeforeAuthorityWrite();
        var coordinator = new ZLinkStandaloneActorRelocationPrecommitCoordinator(
            store);

        var preparing = await coordinator.BeginPreparingAsync(
            ready.Snapshot,
            sourceAuthority,
            Guid.NewGuid(),
            applicationVersion: 1,
            CancellationToken.None);

        Assert.Equal(1, Projection(preparing).Phase);
        Assert.Equal(sourceOwner.OwnerId, preparing.OwnerId);
        Assert.Equal(1, provider.HeartbeatWriteCount);
        Assert.Equal(2, provider.AuthorityWriteCount);
    }

    [Fact]
    public async Task Preparing_Captured_and_target_cutover_are_durable()
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
        Assert.Equal("pending", preparingState.RelocationReference);
        Assert.Equal(ready.Snapshot.StoreVersion,
            preparingState.State.CoordinatorExpectedAuthorityStoreVersion);
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
        lossyStore.LoseNextResponse();
        var captured = await coordinator.CaptureAsync(
            preparing,
            envelope,
            CancellationToken.None);
        var capturedState = Projection(captured);
        Assert.Equal(2, capturedState.Phase);
        Assert.Equal("pending", capturedState.RelocationReference);
        Assert.Equal(envelope.AggregateGeneration,
            capturedState.AggregateGeneration);
        Assert.Equal(0UL, capturedState.TargetAttemptGeneration);

        var prepare = ZLinkStandaloneActorRelocationRuntime.CreatePrepare(
            captured,
            sourceAuthority,
            target,
            envelope,
            ZLinkRelocationTransferPayload.Create(envelope, 1024),
            applicationVersion: 1);

        var targetAuthority = Authority(actorId, target, targetOwner);
        lossyStore.ReturnAuxiliaryConflictNext();
        lossyStore.LoseNextResponse();
        var committed = await coordinator.CommitTargetAsync(
            captured,
            envelope,
            prepare,
            targetAuthority,
            CancellationToken.None);
        var committedState = Projection(committed);
        Assert.Equal(4, committedState.Phase);
        Assert.Equal(
            prepare.TargetAttemptGeneration,
            committedState.TargetAttemptGeneration);
        Assert.Equal(target.Rid.ToHex(), committedState.State.TargetNodeRid);
        Assert.Equal(targetOwner.OwnerId, committed.OwnerId);
        Assert.Equal(ready.Snapshot.ObjectGeneration, committed.ObjectGeneration);
        Assert.Equal(
            checked(ready.Snapshot.AuthorityOwnerGeneration + 1),
            committed.AuthorityOwnerGeneration);
    }

    [Fact]
    public async Task Prepare_coordinator_reuses_the_pre_precommit_baseline_shared_with_zljr()
    {
        //  Regression for the StoreVersion split: the durable ZLJR/saved-work
        //  recovery record and the command-40 Prepare must carry the exact
        //  same Coordinator fence — a single value sourced from the
        //  authority snapshot as it stood *before* BeginPreparingAsync's own
        //  CAS write (the cpp reference builds exactly one `coordinator`
        //  from that pre-precommit snapshot and reuses it for both wire
        //  messages: mesh_node_runtime.cpp's relocate_application_actor).
        //  BeginPreparingAsync/CaptureAsync each mint a fresh StoreVersion
        //  via their own CAS write, so asserting against the *post-Capture*
        //  snapshot here would pass accidentally unless the test also pins
        //  that it differs from the pre-precommit baseline.
        var store = new ZLinkInMemoryLocationStore();
        var sourceOwner = Assert.IsType<ZLinkOwnerLeaseClaimResult.Claimed>(
            await store.ClaimOwnerLeaseAsync(
                "provenance-source",
                TimeSpan.FromMinutes(1))).Token;
        var target = Descriptor(
            RoutingId.From("provenance-target"),
            Assert.IsType<ZLinkOwnerLeaseClaimResult.Claimed>(
                await store.ClaimOwnerLeaseAsync(
                    "provenance-target",
                    TimeSpan.FromMinutes(1))).Token);
        var source = Descriptor(
            RoutingId.From("provenance-source"),
            sourceOwner);
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
        //  The pre-precommit baseline (V0) — both ZLJR and Prepare's
        //  Coordinator must trace back to this exact StoreVersion.
        var v0 = Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await store.CommitAsync(
                reservation.Reservation,
                ZLinkActorAuthorityPayloadCodec.Encode(sourceAuthority))).Snapshot;
        var relocationId = Guid.NewGuid();

        //  Mirrors ZLinkActorRemoteJoiner's sessionRelocationContext, built
        //  from the same pre-precommit snapshot the production fix now
        //  passes to CreatePrepare.
        var sessionRelocationContext = ZLinkSessionRelocationContext.Create(
            relocationId,
            v0.OwnerId,
            checked((ulong)v0.OwnerLeaseGeneration),
            sourceAuthority.NodeRid,
            sourceAuthority.NodeGeneration,
            v0.StoreVersion);

        var coordinator = new ZLinkStandaloneActorRelocationPrecommitCoordinator(
            store);
        var preparing = await coordinator.BeginPreparingAsync(
            v0,
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
        var captured = await coordinator.CaptureAsync(
            preparing,
            envelope,
            CancellationToken.None);

        //  Each CAS write (Preparing, then Captured) mints a new
        //  StoreVersion — proves this test would catch a regression back to
        //  the post-Capture snapshot rather than passing by coincidence.
        Assert.NotEqual(v0.StoreVersion, preparing.StoreVersion);
        Assert.NotEqual(v0.StoreVersion, captured.StoreVersion);

        var prepare = ZLinkStandaloneActorRelocationRuntime.CreatePrepare(
            v0,
            sourceAuthority,
            target,
            envelope,
            ZLinkRelocationTransferPayload.Create(envelope, 1024),
            applicationVersion: 1);

        Assert.Equal(
            sessionRelocationContext.Coordinator.ExpectedAuthorityStoreVersion,
            prepare.Coordinator.ExpectedAuthorityStoreVersion);
        Assert.Equal(v0.StoreVersion, prepare.Coordinator.ExpectedAuthorityStoreVersion);
        Assert.Equal(sessionRelocationContext.Coordinator.OwnerId, prepare.Coordinator.OwnerId);
        Assert.Equal(
            sessionRelocationContext.Coordinator.LeaseGeneration,
            prepare.Coordinator.LeaseGeneration);
        Assert.Equal(sessionRelocationContext.Coordinator.NodeRid, prepare.Coordinator.NodeRid);
        Assert.Equal(
            sessionRelocationContext.Coordinator.NodeGeneration,
            prepare.Coordinator.NodeGeneration);
    }

    [Fact]
    public async Task Target_cutover_accepts_a_fenced_foreign_source_authority()
    {
        var store = new ZLinkInMemoryLocationStore();
        var sourceOwner = Assert.IsType<ZLinkOwnerLeaseClaimResult.Claimed>(
            await store.ClaimOwnerLeaseAsync("foreign-source", TimeSpan.FromMinutes(1))).Token;
        var targetOwner = Assert.IsType<ZLinkOwnerLeaseClaimResult.Claimed>(
            await store.ClaimOwnerLeaseAsync("foreign-target", TimeSpan.FromMinutes(1))).Token;
        var source = Descriptor(RoutingId.From("foreign-source"), sourceOwner);
        var target = Descriptor(RoutingId.From("foreign-target"), targetOwner);
        await store.UpdateMeshNodeAsync(source, ZLinkLocationWriteIntent.NewClaim);
        await store.UpdateMeshNodeAsync(target, ZLinkLocationWriteIntent.NewClaim);
        var actorId = $"actor-{Guid.NewGuid():N}";
        var key = ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId);
        var reservation = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await store.ReserveAsync(new ZLinkObjectReservationRequest(
                ZLinkPlacementObjectKind.Actor, key, "Game.Actor", $"intent:{actorId}",
                SHA256.HashData("intent"u8), 6,
                new ZLinkMeshNodeDescriptorKey("mesh", source.Rid), source.LifecycleGeneration,
                sourceOwner, new byte[] { 1 }, new ZLinkCapacityVector(1, 0, null))));
        var sourceAuthority = Authority(actorId, source, sourceOwner);
        var steady = Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await store.CommitAsync(reservation.Reservation,
                ZLinkActorAuthorityPayloadCodec.Encode(sourceAuthority))).Snapshot;
        var relocationId = Guid.NewGuid();
        var envelope = ZLinkCanonicalActorRelocationWriter.CreateInitial(
            ZLinkStandaloneActorRelocationRuntime.CreateImmutableRoot(
                steady, sourceAuthority, target, relocationId,
                ReadOnlyMemory<byte>.Empty, [], default), applicationVersion: 1);
        var prepare = ZLinkStandaloneActorRelocationRuntime.CreatePrepare(
            steady, sourceAuthority, target, envelope,
            ZLinkRelocationTransferPayload.Create(envelope, 1024), applicationVersion: 1);

        var committed = await new ZLinkStandaloneActorRelocationPrecommitCoordinator(store)
            .CommitTargetAsync(steady, envelope, prepare,
                Authority(actorId, target, targetOwner), CancellationToken.None);

        Assert.Equal(targetOwner.OwnerId, committed.OwnerId);
        Assert.Equal(steady.AuthorityOwnerGeneration + 1,
            committed.AuthorityOwnerGeneration);
        Assert.Equal((byte)ZLinkStandaloneActorCanonicalPhase.Committed,
            Projection(committed).Phase);
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
            actor.OwnerId, actor.OwnerLeaseGeneration,
            actor.NodeRid.ToHex(), actor.NodeGeneration,
            1, string.Empty, 0, 1);

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

    private sealed class OwnerLeaseHeartbeatLocationStore(
        IZLinkLocationStore inner,
        string ownerId) : IZLinkLocationStore
    {
        private readonly ZLinkStoreKey _ownerKey =
            ZLinkProviderLocationRepository.OwnerKey(ownerId);
        private int _trackAuthorityWrites;
        private int _heartbeatNext;

        internal int HeartbeatWriteCount { get; private set; }

        internal int AuthorityWriteCount { get; private set; }

        internal void ArmHeartbeatBeforeAuthorityWrite()
        {
            Interlocked.Exchange(ref _trackAuthorityWrites, 1);
            Interlocked.Exchange(ref _heartbeatNext, 1);
        }

        public ValueTask<ZLinkStoreReadResult> ReadAsync(
            ZLinkStoreKey key,
            CancellationToken cancellationToken = default) =>
            inner.ReadAsync(key, cancellationToken);

        public async ValueTask<ZLinkStoreWriteResult> WriteAsync(
                ZLinkStoreWriteRequest request,
                CancellationToken cancellationToken = default)
        {
            var writesAuthority = request.Mutations
                .OfType<ZLinkStoreMutation.Put>()
                .Any(static mutation => mutation.Key.Value.StartsWith(
                    "authority\0",
                    StringComparison.Ordinal));
            if (writesAuthority
                && Volatile.Read(ref _trackAuthorityWrites) != 0)
                AuthorityWriteCount++;
            if (writesAuthority
                && Interlocked.Exchange(ref _heartbeatNext, 0) == 1)
            {
                var owner = Assert.IsType<ZLinkStoreReadResult.Found>(
                    await inner.ReadAsync(_ownerKey, cancellationToken)
                        .ConfigureAwait(false));
                Assert.IsType<ZLinkStoreWriteResult.Applied>(
                    await inner.WriteAsync(
                            new ZLinkStoreWriteRequest(
                                [new ZLinkStoreCondition.Version(
                                    _ownerKey,
                                    owner.Value.Version)],
                                [new ZLinkStoreMutation.Put(
                                    _ownerKey,
                                    owner.Value.Bytes,
                                    TimeSpan.FromMinutes(1))]),
                            cancellationToken)
                        .ConfigureAwait(false));
                HeartbeatWriteCount++;
            }
            return await inner.WriteAsync(
                    request,
                    cancellationToken)
                .ConfigureAwait(false);
        }

        public ValueTask<ZLinkStoreScanResult> ScanAsync(
            ZLinkStoreScanRequest request,
            CancellationToken cancellationToken = default) =>
            inner.ScanAsync(request, cancellationToken);
    }
}
