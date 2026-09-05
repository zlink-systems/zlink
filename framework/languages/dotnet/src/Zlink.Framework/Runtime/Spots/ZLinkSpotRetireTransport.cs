using System.Diagnostics;
using Microsoft.Extensions.DependencyInjection;
using System.Collections.Concurrent;
using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Identifiers;

namespace Zlink.Framework.Runtime.Spots;

internal sealed record ZLinkCanonicalSpotActorDescriptor(
    string ActorId,
    string StableType,
    byte[] AuthorityPayload);

internal sealed record ZLinkCanonicalSpotStageContext(
    Guid AggregateId,
    ulong AggregateGeneration,
    ulong TargetAttemptGeneration,
    string MeshName,
    string SourceNodeRid,
    ulong SourceNodeLifecycleGeneration,
    string SourceOwnerId,
    ulong SourceOwnerLeaseGeneration,
    string TargetNodeRid,
    ulong TargetNodeLifecycleGeneration,
    string TargetOwnerId,
    ulong TargetOwnerLeaseGeneration,
    string SpotId,
    string StableType,
    bool InstanceSpot,
    string RelocationReference,
    uint RelocationChecksum,
    ZLinkCanonicalSpotActorDescriptor[] Actors,
    string CoordinatorExpectedAuthorityStoreVersion = "");

internal sealed record ZLinkSpotRetireHeldRecord(
    ulong AcceptedSequence,
    byte[] Payload);

internal sealed class ZLinkCanonicalRelocationDurablyAbortedException(
    string message) : Exception(message);

/// <summary>
/// Implements both the source service-wire client and target staging journal.
/// Target state is keyed by aggregate fence, so duplicate Stage/Publish/Abort
/// requests are idempotent.
/// </summary>
internal sealed class ZLinkSpotRetireTargetRuntime(
    IServiceProvider services,
    ZLinkFrameworkRuntime runtime,
    ZLinkFrameworkRegistration registration) : IZLinkSpotRetireTarget
{
    internal static readonly TimeSpan StageRetention = TimeSpan.FromHours(24);
    internal static readonly TimeSpan TombstoneRetention = TimeSpan.FromMinutes(5);
    private readonly ConcurrentDictionary<
        ZLinkAggregateFence,
        ITargetStageEntry> _staged = new();
    private readonly ConcurrentQueue<ZLinkAggregateFence> _terminalOrder = new();
    private readonly ConcurrentDictionary<ZLinkAggregateFence,
        ZLinkServiceWireCodec.RelocationPrepareRecord> _sourceAttempts = new();
    private readonly SemaphoreSlim _reconciliationWake = new(0, 1);
    private int _reconciliationRunning;

    internal int ActiveStageCount => _staged.Values.OfType<TargetStage>().Count();

    internal int TerminalTombstoneCount =>
        _staged.Values.OfType<TargetStageTombstone>().Count();

    internal bool TryTrackStage(
        ZLinkAggregateFence fence,
        TargetStage stage) => _staged.TryAdd(fence, stage);

    public async ValueTask<ZLinkSpotRetireReservation?> TryReserveAsync(
        ZLinkSpotRetireInventory inventory,
        ZLinkRelocationTargetSelection selection,
        CancellationToken cancellationToken) =>
        await ResolveReservationAsync(
                inventory,
                null,
                selection,
                cancellationToken)
            .ConfigureAwait(false);

    public async ValueTask<ZLinkSpotRetireReservation?>
        TryReserveForPreflightAsync(
            ZLinkSpotRetireInventory inventory,
            ZLinkRetirePreflightPlan plan,
            ZLinkRelocationTargetSelection selection,
            CancellationToken cancellationToken) =>
        await ResolveReservationAsync(
                inventory,
                plan,
                selection,
                cancellationToken)
            .ConfigureAwait(false);

    private async ValueTask<ZLinkSpotRetireReservation?> ResolveReservationAsync(
        ZLinkSpotRetireInventory inventory,
        ZLinkRetirePreflightPlan? plan,
        ZLinkRelocationTargetSelection selection,
        CancellationToken cancellationToken)
    {
        if (services.GetService<IZLinkMeshNodeLocationResolver>() is not { } resolver)
            return null;
        var descriptors = await resolver.ListLiveMeshNodesAsync(
                inventory.MeshName,
                cancellationToken)
            .ConfigureAwait(false);
        var kind = inventory.InstanceSpot
            ? ZLinkPlacementObjectKind.InstanceSpot
            : ZLinkPlacementObjectKind.UserSpot;
        var localNodeRids = registration.SpotNodes.Values
            .Select(static node => node.EffectiveRoutingId)
            .ToHashSet();
        var capacity = new ZLinkCapacityVector(
            plan is not null || !inventory.PerActorShell
                ? inventory.ActorIds.Count
                : 0,
            1,
            new ZLinkSpotTypeCapacityDelta(
                kind,
                inventory.StableType,
                1));
        var targets = descriptors
            .Where(candidate =>
                !localNodeRids.Contains(candidate.Rid)
                && candidate.State == ZLinkFrameworkRuntimeState.Serving
                && candidate.ObjectRole == ZLinkMeshNodeObjectRole.Server
                && candidate.PlacementWeight > 0
                && selection.Matches(candidate)
                && IsCompatibleTarget(candidate, registration, inventory, kind))
            .OrderBy(static candidate => candidate.Rid, ZLinkRoutingIdOrder.Instance)
            .ToArray();
        var target = targets.FirstOrDefault(candidate =>
            candidate.LeaseGeneration > 0
            && (plan is null || plan.TryReserve(candidate, capacity)));
        if (target is null)
            return null;
        return new ZLinkSpotRetireReservation(
            inventory,
            new ZLinkMeshNodeDescriptorKey(target.MeshName, target.Rid),
            target.LifecycleGeneration,
            capacity,
            new ZLinkLocationOwnerToken(
                target.OwnerId,
                checked((ulong)target.LeaseGeneration)));
    }

    internal static bool IsCompatibleTarget(
        ZLinkMeshNodeDescriptor candidate,
        ZLinkFrameworkRegistration source,
        ZLinkSpotRetireInventory inventory,
        ZLinkPlacementObjectKind spotKind)
    {
        if (source.MaintenanceWave is { } sourceWave
            && StringComparer.Ordinal.Equals(
                sourceWave,
                candidate.MaintenanceWave)
            || !HasHeadroom(candidate.Capacity.Actors, inventory.ActorIds.Count)
            || !HasHeadroom(candidate.Capacity.Spots, 1)
            || candidate.ActivationConcurrency.Limit
               - candidate.ActivationConcurrency.Active < 1)
            return false;

        foreach (var required in inventory.RequiredCapabilities)
        {
            var compatible = candidate.ObjectCapabilities.Any(capability =>
                capability.ObjectKind == required.ObjectKind
                && StringComparer.Ordinal.Equals(
                    capability.StableType,
                    required.StableType)
                && capability.Policy == required.Policy
                && (required.Policy != ZLinkObjectMaintenancePolicyKind.Snapshot
                    || capability.HasSnapshotAdapter));
            if (!compatible)
                return false;
        }

        var typeCapacity = candidate.Capacity.SpotTypes.SingleOrDefault(capacity =>
            capacity.ObjectKind == spotKind
            && StringComparer.Ordinal.Equals(
                capacity.StableType,
                inventory.StableType));
        return typeCapacity is not null
               && HasHeadroom(
                   new ZLinkPopulationCapacity(
                       typeCapacity.Active,
                       typeCapacity.Reserved,
                       typeCapacity.Limit),
                   1);
    }

    internal static ZLinkServiceWireCodec.RelocationObjectRecord
        CreateCanonicalRelocationObject(
            ZLinkPlacementObjectKind kind,
            string stableType,
            string spotId,
            ulong objectGeneration,
            ulong authorityOwnerGeneration)
    {
        if (kind is not (ZLinkPlacementObjectKind.UserSpot
            or ZLinkPlacementObjectKind.InstanceSpot))
            throw new ArgumentOutOfRangeException(nameof(kind));
        return new ZLinkServiceWireCodec.RelocationObjectRecord(
            (byte)kind,
            kind == ZLinkPlacementObjectKind.InstanceSpot
                ? stableType
                : string.Empty,
            spotId,
            objectGeneration,
            kind == ZLinkPlacementObjectKind.InstanceSpot
                ? 0
                : authorityOwnerGeneration);
    }

    internal static bool HasHeadroom(
        ZLinkPopulationCapacity capacity,
        int required) =>
        required == 0
        || capacity.Limit == 0
        || capacity.Limit - capacity.Active - capacity.Reserved >= required;

    public async ValueTask StageAsync(
        ZLinkSpotRetireReservation reservation,
        ZLinkPreparedSpotRetireStaging relocation,
        CancellationToken cancellationToken)
    {
        if (runtime.GetSpotNodeRuntime(reservation.Inventory.SourceNodeRid).Node
                is IZLinkBackendCanonicalRelocation canonical)
        {
            var spot = relocation.Envelope.Participants.Single(
                static participant => participant.ObjectKind
                    is ZLinkPlacementObjectKind.UserSpot
                    or ZLinkPlacementObjectKind.InstanceSpot);
            var authority = relocation.Participants.Single(participant =>
                participant.Envelope.AuthorityKey == spot.AuthorityKey);
            var idBytes = relocation.Envelope.AggregateId.ToByteArray(
                bigEndian: true);
            var relocationId = new ZLinkServiceWireCodec.RelocationWireId(
                BinaryPrimitives.ReadUInt64BigEndian(idBytes.AsSpan(0, 8)),
                BinaryPrimitives.ReadUInt64BigEndian(idBytes.AsSpan(8, 8)));
            if (relocationId.IsEmpty)
                throw new InvalidDataException("Canonical relocation ID is empty.");
            var targetAttemptGeneration =
                relocation.Envelope.AggregateGeneration;
            if (targetAttemptGeneration == 0)
                throw new InvalidDataException(
                    "Canonical relocation target attempt generation is empty.");
            //  Direct transfer (spec 28 §4.2): encode the captured aggregate
            //  envelope once into source memory and stream it as
            //  relocationState chunks behind command 40.
            var transferPayload = ZLinkRelocationTransferPayload.Create(
                relocation.Envelope,
                registration.Locations.Options.RelocationPayloadChunkLimit);
            var prepare = new ZLinkServiceWireCodec.RelocationPrepareRecord(
                relocationId,
                targetAttemptGeneration,
                new ZLinkServiceWireCodec.RelocationCoordinatorFence(
                    reservation.Inventory.SourceOwner.OwnerId,
                    checked((ulong)reservation.Inventory.SourceOwner
                        .LeaseGeneration),
                    reservation.Inventory.SourceNodeRid,
                    reservation.Inventory.SourceNodeLifecycleGeneration,
                    authority.ExpectedStoreVersion),
                new ZLinkServiceWireCodec.RelocationTargetRecord(
                    reservation.TargetDescriptor.Rid,
                    reservation.TargetDescriptorLifecycleGeneration,
                    reservation.TargetOwner.OwnerId,
                    checked((ulong)reservation.TargetOwner.LeaseGeneration)),
                1,
                CreateCanonicalRelocationObject(
                    spot.ObjectKind,
                    reservation.Inventory.StableType,
                    reservation.Inventory.SpotId,
                    spot.ObjectGeneration,
                    spot.AuthorityOwnerGeneration),
                reservation.Inventory.SourceNodeRid,
                reservation.Inventory.SourceNodeLifecycleGeneration,
                checked((ulong)transferPayload.TotalLength),
                checked((uint)transferPayload.ChunkCount),
                transferPayload.ChecksumCrc32c,
                checked((ulong)registration.ApplicationVersion));
            _ = await canonical.PrepareCanonicalRelocationAsync(
                    reservation.TargetDescriptor.Rid,
                    prepare,
                    transferPayload,
                    registration.DefaultRequestTimeout,
                    cancellationToken)
                .ConfigureAwait(false);
            var sourceFence = new ZLinkAggregateFence(
                relocation.Envelope.AggregateId,
                relocation.Envelope.AggregateGeneration);
            _sourceAttempts[sourceFence] = prepare;
            try
            {
                //  The spot aggregate relays no post-capture ingress via
                //  command 31 — held ingress travels inside the envelope —
                //  so the boundary batch is empty (spec 28 §4.4).
                await canonical.SendCanonicalRelocationCutoverAsync(
                        reservation.TargetDescriptor.Rid,
                        new ZLinkServiceWireCodec.RelocationCutoverRecord(
                            prepare.RelocationId,
                            prepare.TargetAttemptGeneration,
                            prepare.Coordinator,
                            prepare.InitiatorRole,
                            prepare.Object,
                            0,
                            ZLinkRelocationBoundaryBatch.ComputeChecksum([])),
                        cancellationToken)
                    .ConfigureAwait(false);
            }
            catch (Exception error)
            {
                ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"cutover_send_uncertain object=spot error={error.GetType().Name}");
            }
            return;
        }
        throw new ZLinkConfigurationException(
            "The MeshNode backend does not support canonical relocation wire commands.");
    }

    public async ValueTask<ulong> PublishAsync(
        ZLinkSpotRetireReservation reservation,
        ZLinkAggregateRelocationPublished relocation,
        CancellationToken cancellationToken)
    {
        return await ReconcilePublishedAuthorityAsync(
                registration.Locations.ResolveStore()
                ?? throw new ZLinkConfigurationException(
                    "Location Store is not registered."),
                reservation, relocation, cancellationToken)
            .ConfigureAwait(false);
    }

    public ValueTask AbortAsync(
        ZLinkSpotRetireReservation reservation,
        ZLinkAggregateFence? fence)
    {
        _ = reservation;
        if (fence is { } exactFence
            && _sourceAttempts.TryGetValue(exactFence, out var prepare))
            _sourceAttempts.TryRemove(new KeyValuePair<
                ZLinkAggregateFence,
                ZLinkServiceWireCodec.RelocationPrepareRecord>(
                exactFence,
                prepare));
        return ValueTask.CompletedTask;
    }

    internal async ValueTask<ulong> ReconcilePublishedAuthorityAsync(
        IZLinkLocationRepository store,
        ZLinkSpotRetireReservation reservation,
        ZLinkAggregateRelocationPublished relocation,
        CancellationToken cancellationToken)
    {
        var deadline = Stopwatch.GetElapsedTime(0)
            + registration.DefaultRequestTimeout;
        var spot = relocation.Envelope.Participants.Single(
            static participant => participant.ObjectKind is
                ZLinkPlacementObjectKind.UserSpot
                or ZLinkPlacementObjectKind.InstanceSpot);
        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var read = await store.ReadAuthorityAsync(
                    spot.AuthorityKey, cancellationToken)
                .ConfigureAwait(false);
            if (read is ZLinkAuthorityReadResult.Found found
                && found.Snapshot.Allocation.Descriptor
                   == reservation.TargetDescriptor
                && found.Snapshot.Allocation.DescriptorLifecycleGeneration
                   == reservation.TargetDescriptorLifecycleGeneration
                && found.Snapshot.ObjectGeneration == spot.ObjectGeneration
                && found.Snapshot.AuthorityOwnerGeneration
                   > spot.AuthorityOwnerGeneration
                && found.Snapshot.OwnerId == reservation.TargetOwner.OwnerId
                && found.Snapshot.OwnerLeaseGeneration
                   == reservation.TargetOwner.LeaseGeneration
                && (ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                    found.Snapshot.Payload.Span, out var publication)
                && publication.AggregateId == relocation.Fence.AggregateId
                && publication.AggregateGeneration
                   == relocation.Fence.AggregateGeneration
                || ZLinkAggregateRelocationCoordinator.IsExactNormalizedTargetAuthority(
                    found.Snapshot,
                    spot,
                    relocation.Fence.AggregateId,
                    reservation.TargetDescriptor,
                    reservation.TargetDescriptorLifecycleGeneration,
                    reservation.TargetOwner)))
                return found.Snapshot.AuthorityOwnerGeneration;
            if (Stopwatch.GetElapsedTime(0) >= deadline)
            {
                var relocationStore = registration.Locations
                    .ResolveRelocationStore()
                    ?? throw new ZLinkConfigurationException(
                        "Relocation Store is not registered.");
                var exact = await new ZLinkRelocationStartupRecovery(
                        store, relocationStore)
                    .TryReadExactPublishedAsync(
                        relocation.Envelope, CancellationToken.None)
                    .ConfigureAwait(false);
                if (exact is not null)
                    return exact.Authorities.Single(authority =>
                            authority.Key == spot.AuthorityKey)
                        .Snapshot.AuthorityOwnerGeneration;
                var kind = spot.ObjectKind;
                if (kind == ZLinkPlacementObjectKind.UserSpot)
                {
                    var abort = await store.AbortAggregateAsync(
                            relocation.Fence, CancellationToken.None)
                        .ConfigureAwait(false);
                    if (abort is ZLinkAggregateAbortResult.Aborted
                        or ZLinkAggregateAbortResult.AlreadyAborted)
                        throw new ZLinkCanonicalRelocationDurablyAbortedException(
                            "The target did not publish before the durable aggregate abort won.");
                    exact = await new ZLinkRelocationStartupRecovery(
                            store, relocationStore)
                        .TryReadExactPublishedAsync(
                            relocation.Envelope, CancellationToken.None)
                        .ConfigureAwait(false);
                    if (exact is not null)
                        return exact.Authorities.Single(authority =>
                                authority.Key == spot.AuthorityKey)
                            .Snapshot.AuthorityOwnerGeneration;
                }
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.DeadlineExceeded,
                    "Canonical relocation publication could not be reconciled.",
                    ZLinkRetryAdvice.RetryAfterBackoff);
            }
            await Task.Delay(1, cancellationToken).ConfigureAwait(false);
        }
    }

    public ValueTask RelayCommittedAsync(
        ZLinkSpotRetireReservation reservation,
        ZLinkAggregateRelocationPublished relocation,
        IReadOnlyList<ZLinkAcceptedWorkRecord> held,
        CancellationToken cancellationToken)
    {
        return CompleteCanonicalSourceAsync(
            reservation, relocation, held, cancellationToken);
    }

    private ValueTask CompleteCanonicalSourceAsync(
        ZLinkSpotRetireReservation reservation,
        ZLinkAggregateRelocationPublished relocation,
        IReadOnlyList<ZLinkAcceptedWorkRecord> held,
        CancellationToken cancellationToken)
    {
        _ = reservation;
        _ = held;
        cancellationToken.ThrowIfCancellationRequested();
        if (!_sourceAttempts.TryGetValue(relocation.Fence, out var prepare))
            return ValueTask.CompletedTask;
        _sourceAttempts.TryRemove(
            new KeyValuePair<
                ZLinkAggregateFence,
                ZLinkServiceWireCodec.RelocationPrepareRecord>(
                relocation.Fence,
                prepare));
        return ValueTask.CompletedTask;
    }

    internal async ValueTask RecoverPublishedAsync(
        ZLinkRelocationRecoveryCandidate candidate,
        CancellationToken cancellationToken)
    {
        // No different-target transition exists in this contract version:
        // recovery restages only when this process hosts the exact published
        // target (the NotFound return below); a dead target parks the
        // aggregate until that exact lifecycle returns.
        candidate = BindRecoveredCanonicalInventory(candidate);
        var spotAuthority = candidate.Authorities.Single(
            static entry => entry.Snapshot.Allocation.ObjectKind
                is ZLinkPlacementObjectKind.UserSpot
                or ZLinkPlacementObjectKind.InstanceSpot);
        if (!ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                spotAuthority.Snapshot.Payload.Span,
                out var spotPublication))
            throw new ZLinkRelocationDataLostException(
                $"SPOT authority '{spotAuthority.Key.Value}' has no relocation publication.");
        if (!ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                spotAuthority.Snapshot.Payload.Span,
                out var canonical)
            || canonical.TargetAttemptGeneration == 0)
            throw new ZLinkRelocationDataLostException(
                $"SPOT authority '{spotAuthority.Key.Value}' has no current target attempt.");

        var instanceSpot =
            spotAuthority.Snapshot.Allocation.ObjectKind
            == ZLinkPlacementObjectKind.InstanceSpot;
        string spotId;
        string meshName;
        RoutingId sourceNodeRid;
        ulong sourceNodeGeneration;
        string sourceOwnerId;
        ulong sourceOwnerGeneration;
        if (instanceSpot)
        {
            if (!ZLinkInstanceSpotAuthorityPayloadCodec.TryDecode(
                    spotPublication.ApplicationPayload.Span,
                    out var source))
                throw new ZLinkRelocationDataLostException(
                    "Instance SPOT relocation source authority is invalid.");
            spotId = source.SpotId;
            meshName = source.MeshName;
            sourceNodeRid = source.NodeRid;
            sourceNodeGeneration = source.NodeGeneration;
            sourceOwnerId = source.OwnerId;
            sourceOwnerGeneration = source.OwnerLeaseGeneration;
        }
        else
        {
            if (!ZLinkUserSpotAuthorityPayloadCodec.TryDecode(
                    spotPublication.ApplicationPayload.Span,
                    out var source))
                throw new ZLinkRelocationDataLostException(
                    "User SPOT relocation source authority is invalid.");
            spotId = source.SpotId;
            meshName = source.MeshName;
            sourceNodeRid = source.NodeRid;
            sourceNodeGeneration = source.NodeGeneration;
            sourceOwnerId = source.OwnerId;
            sourceOwnerGeneration = source.OwnerLeaseGeneration;
        }

        var actorAuthorities = candidate.Authorities
            .Where(static entry =>
                entry.Snapshot.Allocation.ObjectKind
                == ZLinkPlacementObjectKind.Actor)
            .ToDictionary(
                static entry => entry.Key.Value,
                StringComparer.Ordinal);
        var actors = candidate.Envelope.Participants
            .Where(static participant =>
                participant.ObjectKind == ZLinkPlacementObjectKind.Actor)
            .Select(participant =>
            {
                var authority = actorAuthorities[
                    participant.AuthorityKey.Value];
                if (!ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                        authority.Snapshot.Payload.Span,
                        out var publication)
                    || !ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
                        publication.ApplicationPayload.Span,
                        out var actor))
                    throw new ZLinkRelocationDataLostException(
                        $"Actor authority '{authority.Key.Value}' is invalid.");
                return new ZLinkCanonicalSpotActorDescriptor(
                    actor.ActorId,
                    actor.StableType,
                    publication.ApplicationPayload.ToArray());
            })
            .ToArray();
        ZLinkSpotNodeRuntime recoveryNode;
        try
        {
            recoveryNode = runtime.GetSpotNodeRuntime(
                spotAuthority.Snapshot.Allocation.Descriptor.Rid);
        }
        catch (ZLinkFrameworkException exception)
            when (exception.Kind
                  == ZLinkFrameworkErrorKind.NotFound)
        {
            return;
        }
        var request = new ZLinkCanonicalSpotStageContext(
            candidate.Envelope.AggregateId,
            candidate.Envelope.AggregateGeneration,
            canonical.TargetAttemptGeneration,
            meshName,
            sourceNodeRid.ToHex(),
            sourceNodeGeneration,
            sourceOwnerId,
            sourceOwnerGeneration,
            spotAuthority.Snapshot.Allocation.Descriptor.Rid.ToHex(),
            spotAuthority.Snapshot.Allocation.DescriptorLifecycleGeneration,
            spotAuthority.Snapshot.OwnerId,
            checked((ulong)spotAuthority.Snapshot.OwnerLeaseGeneration),
            spotId,
            spotAuthority.Snapshot.Allocation.StableType,
            instanceSpot,
            candidate.Reference.Reference,
            candidate.Reference.ChecksumCrc32c,
            actors);
        var fence = new ZLinkAggregateFence(
            candidate.Envelope.AggregateId,
            candidate.Envelope.AggregateGeneration);
        var existing = _staged.Values.OfType<TargetStage>().SingleOrDefault(stage =>
            stage.Envelope.AggregateId == candidate.Envelope.AggregateId
            && candidate.Envelope.AggregateGeneration
               == stage.Envelope.AggregateGeneration);
        if (existing is not null)
        {
            var stagingRequest = request with
            {
                AggregateGeneration = existing.Envelope.AggregateGeneration,
                RelocationReference = existing.RelocationReference,
                RelocationChecksum = existing.RelocationChecksum
            };
            if (!existing.Matches(stagingRequest, sourceNodeRid)
                || !IsStagingPrefix(
                    existing.Envelope,
                    candidate.Envelope,
                    existing.TargetAuthorityOwnerGenerationFor))
                throw new ZLinkRelocationDataLostException(
                    "Recovered SPOT staging conflicts with its published aggregate.");
            await ReconcilePublishedStageAsync(
                    existing,
                    candidate,
                    cancellationToken)
                .ConfigureAwait(false);
            return;
        }

        TargetStage stage;
        try
        {
            stage = await runtime.StageInboundSpotAggregateAsync(
                    request,
                    candidate.Envelope,
                    reservedTargetAuthorityOwnerGeneration: 0,
                    cancellationToken)
                .ConfigureAwait(false);
        }
        catch
        {
            throw;
        }
        if (!TryTrackStage(fence, stage))
        {
            await AbortTargetStageAsync(stage)
                .ConfigureAwait(false);
            if (!_staged.TryGetValue(fence, out var winningEntry)
                || winningEntry is not TargetStage winner
                || !winner.Matches(request, sourceNodeRid))
                throw new ZLinkRelocationDataLostException(
                    "Recovered SPOT staging conflicts with the aggregate winner.");
            stage = winner;
        }
        await ReconcilePublishedStageAsync(
                stage,
                candidate,
                cancellationToken)
            .ConfigureAwait(false);
        if (Volatile.Read(ref stage.Published) == 0)
            ScheduleReconciliation();
    }

    // Read-side tolerance: durable roots written by earlier builds may carry
    // takeover-bumped aggregate generations; the authority publication is the
    // generation of record and the envelope is rebound to it here.
    internal static ZLinkRelocationRecoveryCandidate BindRecoveredCanonicalInventory(
        ZLinkRelocationRecoveryCandidate candidate)
    {
        if (candidate.Envelope.CanonicalLogicalStream.IsEmpty)
            return candidate;
        var states = candidate.Envelope.Participants
            .OrderBy(static participant => participant.CanonicalParticipantId)
            .ToArray();
        var authorities = candidate.Authorities
            .OrderBy(static authority => authority.Snapshot.Allocation.ObjectKind
                is ZLinkPlacementObjectKind.UserSpot
                or ZLinkPlacementObjectKind.InstanceSpot ? 0 : 1)
            .ThenBy(static authority => authority.Key.Value, StringComparer.Ordinal)
            .ToArray();
        if (states.Length != authorities.Length)
            throw new ZLinkRelocationDataLostException(
                "Canonical relocation authority inventory count changed during recovery.");
        ulong aggregateGeneration = 0;
        var bound = states.Select((state, index) =>
        {
            var authority = authorities[index];
            if (!ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                    authority.Snapshot.Payload.Span,
                    out var publication)
                || !publication.IsCanonical)
                throw new ZLinkRelocationDataLostException(
                    $"Canonical relocation authority '{authority.Key.Value}' is invalid.");
            aggregateGeneration = aggregateGeneration == 0
                ? publication.AggregateGeneration
                : aggregateGeneration;
            if (publication.AggregateGeneration != aggregateGeneration)
                throw new ZLinkRelocationDataLostException(
                    "Canonical relocation authorities disagree on progress.");
            return state with
            {
                AuthorityKey = authority.Key,
                ObjectKind = authority.Snapshot.Allocation.ObjectKind,
                ObjectGeneration = authority.Snapshot.ObjectGeneration,
                AuthorityOwnerGeneration = authority.Snapshot.AuthorityOwnerGeneration
            };
        }).ToArray();
        return candidate with
        {
            Envelope = candidate.Envelope with
            {
                AggregateGeneration = aggregateGeneration,
                Participants = bound
            }
        };
    }

    internal async ValueTask ReconcilePublishedStageAsync(
        TargetStage stage,
        ZLinkRelocationRecoveryCandidate candidate,
        CancellationToken cancellationToken)
    {
        candidate = BindRecoveredCanonicalInventory(candidate);
        if (!IsStagingPrefix(
                stage.Envelope,
                candidate.Envelope,
                stage.TargetAuthorityOwnerGenerationFor))
            throw new ZLinkRelocationDataLostException(
                "Published SPOT relocation root does not extend its staged root.");
        RestoreHeldRecords(stage, candidate.Envelope);
        Volatile.Write(ref stage.AuthorityPublished, 1);

        stage.RememberFinalRoot(
            candidate.Reference.Reference,
            candidate.Reference.ChecksumCrc32c);
        cancellationToken.ThrowIfCancellationRequested();

        await FinalizeStageAsync(
                stage,
                normalizeAuthority: false,
                cancellationToken)
            .ConfigureAwait(false);
        await CompleteInboundThenNormalizeAsync(stage, cancellationToken)
            .ConfigureAwait(false);
    }

    private static void RestoreHeldRecords(
        TargetStage stage,
        ZLinkRelocationEnvelope authoritative)
    {
        var stagedSpot = stage.SpotParticipant;
        var authoritativeSpot = authoritative.Participants.Single(
            static participant => participant.ObjectKind
                is ZLinkPlacementObjectKind.UserSpot
                or ZLinkPlacementObjectKind.InstanceSpot);
        var held = authoritativeSpot.AcceptedJobs
            .Skip(stagedSpot.AcceptedJobs.Count)
            .Select(static job => new ZLinkSpotRetireHeldRecord(
                job.AcceptedSequence,
                job.Payload.ToArray()))
            .ToArray();
        ValidateHeldRecords(held);
        if (!TrySetHeldRecords(stage, held))
            throw new ZLinkRelocationDataLostException(
                "Published held ingress conflicts with the target journal.");
    }

    internal static bool TrySetHeldRecords(
        TargetStage stage,
        IReadOnlyList<ZLinkSpotRetireHeldRecord> records)
    {
        var digest = ComputeHeldDigest(records);
        return stage.TrySetHeldRecords(records, digest);
    }

    internal static void ValidateHeldRecords(
        IReadOnlyList<ZLinkSpotRetireHeldRecord> records)
    {
        long bytes = 0;
        ulong previous = 0;
        foreach (var record in records)
        {
            if (record.AcceptedSequence == 0
                || record.AcceptedSequence <= previous)
                throw new InvalidDataException(
                    "Held Spot ingress sequences must be strictly increasing.");
            previous = record.AcceptedSequence;
            bytes = checked(bytes + record.Payload.LongLength);
        }
    }

    internal static byte[] ComputeStageRequestDigest(
        ZLinkCanonicalSpotStageContext request)
    {
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        Append(request.AggregateId.ToString("N"));
        Append(request.AggregateGeneration.ToString());
        Append(request.TargetAttemptGeneration.ToString());
        Append(request.MeshName);
        Append(request.SourceNodeRid);
        Append(request.SourceNodeLifecycleGeneration.ToString());
        Append(request.SourceOwnerId);
        Append(request.SourceOwnerLeaseGeneration.ToString());
        Append(request.TargetNodeRid);
        Append(request.TargetNodeLifecycleGeneration.ToString());
        Append(request.TargetOwnerId);
        Append(request.TargetOwnerLeaseGeneration.ToString());
        Append(request.SpotId);
        Append(request.StableType);
        Append(request.InstanceSpot ? "1" : "0");
        Append(request.RelocationReference);
        Append(request.RelocationChecksum.ToString());
        Append(request.CoordinatorExpectedAuthorityStoreVersion);
        Span<byte> payloadLength = stackalloc byte[4];
        foreach (var actor in request.Actors)
        {
            Append(actor.ActorId);
            Append(actor.StableType);
            BinaryPrimitives.WriteInt32BigEndian(
                payloadLength,
                actor.AuthorityPayload.Length);
            hash.AppendData(payloadLength);
            hash.AppendData(actor.AuthorityPayload);
        }
        return hash.GetHashAndReset();

        void Append(string value)
        {
            var bytes = Encoding.UTF8.GetBytes(value);
            Span<byte> length = stackalloc byte[4];
            BinaryPrimitives.WriteInt32BigEndian(length, bytes.Length);
            hash.AppendData(length);
            hash.AppendData(bytes);
        }
    }

    internal async ValueTask StageCanonicalInboundAsync(
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
        ZLinkRelocationEnvelope envelope,
        RoutingId sourceNodeRid,
        CancellationToken cancellationToken)
    {
        if (prepare.Object.Kind == 1)
            throw new InvalidOperationException(
                "Standalone Actor relocation must be handled by the Actor maintenance owner.");
        //  Spec 28 §4.3: the aggregate envelope arrived over relocationState
        //  chunks and was verified before this call — no store handoff read.
        // The canonical relocation envelope deliberately excludes the private
        // participant-recovery projection.  A decoded canonical envelope
        // therefore has empty recovery payloads; only decode the projection
        // when this caller supplied one outside that canonical wire path.
        var recoveries = envelope.Participants
            .Where(static participant => !participant.RecoveryPayload.IsEmpty)
            .Select(participant =>
                ZLinkCanonicalParticipantRecoveryCodec.Decode(
                    participant.RecoveryPayload.Span))
            .ToArray();
        var actors = recoveries
            .Where(static recovery =>
                recovery.ObjectKind == ZLinkPlacementObjectKind.Actor)
            .Select(recovery =>
            {
                if (!ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
                        recovery.AuthorityPayload.Span, out var actor))
                    throw new InvalidDataException(
                        "Canonical Actor recovery payload is invalid.");
                return new ZLinkCanonicalSpotActorDescriptor(
                    actor.ActorId, actor.StableType,
                    recovery.AuthorityPayload.ToArray());
            })
            .ToArray();
        var spotRecovery = recoveries.SingleOrDefault(static recovery =>
            recovery.ObjectKind is ZLinkPlacementObjectKind.UserSpot
                or ZLinkPlacementObjectKind.InstanceSpot);
        var stableType = spotRecovery?.StableType
                         ?? await ResolveCanonicalSpotStableTypeAsync(
                                 prepare,
                                 cancellationToken)
                             .ConfigureAwait(false);
        var context = new ZLinkCanonicalSpotStageContext(
            envelope.AggregateId,
            envelope.AggregateGeneration,
            prepare.TargetAttemptGeneration,
            runtime.GetSpotNodeRuntime(
                    prepare.Target.NodeRid)
                .Node.MeshStatus().MeshName,
            prepare.SourceNodeRid.ToHex(),
            prepare.SourceNodeGeneration,
            prepare.Coordinator.OwnerId,
            prepare.Coordinator.LeaseGeneration,
            prepare.Target.NodeRid.ToHex(),
            prepare.Target.NodeGeneration,
            prepare.Target.OwnerId,
            prepare.Target.OwnerLeaseGeneration,
            prepare.Object.ObjectId,
            stableType,
            prepare.Object.Kind == 3,
            string.Empty,
            prepare.PayloadChecksumCrc32c,
            actors,
            prepare.Coordinator.ExpectedAuthorityStoreVersion);
        var staged = await StageInboundAsync(
                context,
                envelope,
                sourceNodeRid,
                cancellationToken)
            .ConfigureAwait(false);
        if (!staged)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                "Canonical target staging was rejected.", ZLinkRetryAdvice.RetryAfterBackoff);
    }

    private async ValueTask<string> ResolveCanonicalSpotStableTypeAsync(
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
        CancellationToken cancellationToken)
    {
        if (prepare.Object.Kind == (byte)ZLinkPlacementObjectKind.InstanceSpot)
            return prepare.Object.StableType;

        var authorityStore = registration.Locations.ResolveStore()
                             ?? throw new ZLinkConfigurationException(
                                 "Location Store is not registered.");
        var key = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(
            prepare.Object.ObjectId);
        var read = await authorityStore.ReadAuthorityAsync(key, cancellationToken)
            .ConfigureAwait(false);
        if (read is not ZLinkAuthorityReadResult.Found found
            || !ZLinkUserSpotAuthorityPayloadCodec.TryDecode(
                found.Snapshot.Payload.Span,
                out var source)
            || source.SpotId != prepare.Object.ObjectId
            || string.IsNullOrEmpty(source.StableType))
            throw new ZLinkRelocationDataLostException(
                "Canonical User SPOT relocation source authority is invalid.");
        return source.StableType;
    }

    internal void ScheduleCanonicalCutoverFallback(
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
        RoutingId sourceNodeRid) =>
        _ = RunCanonicalCutoverFallbackAsync(prepare, sourceNodeRid);

    internal async ValueTask AbortCanonicalPreparedTargetAsync(
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
        RoutingId sourceNodeRid)
    {
        var fence = new ZLinkAggregateFence(
            DecodeRelocationId(prepare.RelocationId),
            prepare.TargetAttemptGeneration);
        if (!_staged.TryGetValue(fence, out var candidate)
            || candidate is not TargetStage stage
            || stage.SourceNodeRid != sourceNodeRid
            || Volatile.Read(ref stage.AuthorityPublished) != 0
            || Volatile.Read(ref stage.Published) != 0
            || !_staged.TryRemove(
                new KeyValuePair<ZLinkAggregateFence, ITargetStageEntry>(
                    fence,
                    stage)))
            return;
        await AbortTargetStageAsync(stage).ConfigureAwait(false);
    }

    private async Task RunCanonicalCutoverFallbackAsync(
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
        RoutingId sourceNodeRid)
    {
        try
        {
            await Task.Delay(
                    registration.Locations.Options.RelocationCutoverWaitTimeout,
                    runtime.ShutdownToken)
                .ConfigureAwait(false);
            var fence = new ZLinkAggregateFence(
                DecodeRelocationId(prepare.RelocationId),
                prepare.TargetAttemptGeneration);
            if (!_staged.TryGetValue(fence, out var entry)
                || entry is not TargetStage stage
                || Volatile.Read(ref stage.AuthorityPublished) != 0)
                return;
            ZLinkFrameworkDebugLog.SpotDiscovery(
                "cutover_timeout object=spot");
            //  Spec 28 §4.4/25 §5: fallback proceeds to the CAS without
            //  boundary completeness verification and is counted.
            ZLinkRuntimeMetrics.RecordRelocationCutoverTimeout(
                prepare.Object.Kind == 3 ? "instance_spot" : "user_spot");
            await CutoverCanonicalInboundAsync(
                    new ZLinkServiceWireCodec.RelocationCutoverRecord(
                        prepare.RelocationId,
                        prepare.TargetAttemptGeneration,
                        prepare.Coordinator,
                        prepare.InitiatorRole,
                        prepare.Object,
                        0,
                        0),
                    sourceNodeRid,
                    verifyBoundary: false,
                    runtime.ShutdownToken)
                .ConfigureAwait(false);
        }
        catch (OperationCanceledException)
            when (runtime.ShutdownToken.IsCancellationRequested)
        {
        }
        catch (Exception error)
        {
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"location_update_failed object=spot error={error.GetType().Name}");
        }
    }

    internal ValueTask AppendCanonicalInboundDataAsync(
        ZLinkServiceWireCodec.RelocationDataRecord data,
        RoutingId sourceNodeRid,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var stage = RequireWireStage(
            data.RelocationId,
            data.TargetAttemptGeneration,
            data.Coordinator,
            data.SenderRole,
            data.Object,
            sourceNodeRid,
            "31");
        return stage.AppendCanonicalInboundDataAsync(data);
    }

    internal ValueTask CutoverCanonicalInboundAsync(
        ZLinkServiceWireCodec.RelocationCutoverRecord cutover,
        RoutingId sourceNodeRid,
        CancellationToken cancellationToken) =>
        CutoverCanonicalInboundAsync(
            cutover,
            sourceNodeRid,
            verifyBoundary: true,
            cancellationToken);

    private async ValueTask CutoverCanonicalInboundAsync(
        ZLinkServiceWireCodec.RelocationCutoverRecord cutover,
        RoutingId sourceNodeRid,
        bool verifyBoundary,
        CancellationToken cancellationToken)
    {
        TargetStage stage;
        try
        {
            stage = RequireWireStage(
                cutover.RelocationId,
                cutover.TargetAttemptGeneration,
                cutover.Coordinator,
                cutover.SenderRole,
                cutover.Object,
                sourceNodeRid,
                "34");
        }
        catch (ZLinkFrameworkException exception)
            when (exception.Kind == ZLinkFrameworkErrorKind.NotFound)
        {
            ZLinkFrameworkDebugLog.SpotDiscovery(
                "late_cutover object=spot reason=no_prepared_target");
            return;
        }

        await stage.PublishGate.WaitAsync(cancellationToken)
            .ConfigureAwait(false);
        try
        {
            if (Volatile.Read(ref stage.AuthorityPublished) != 0)
            {
                ZLinkFrameworkDebugLog.SpotDiscovery(
                    "late_cutover object=spot reason=already_committed");
                return;
            }
            if (verifyBoundary)
                stage.ValidateBoundary(cutover);
            await CommitWireTargetAggregateAsync(stage, cancellationToken)
                .ConfigureAwait(false);
            Volatile.Write(ref stage.AuthorityPublished, 1);
        }
        finally
        {
            stage.PublishGate.Release();
        }
        //  Spec 25 §5: target-local S2 (CAS confirmed) → S3 (dispatch open).
        var resumeStartedTimestamp =
            System.Diagnostics.Stopwatch.GetTimestamp();
        await FinalizeStageAsync(
                stage,
                normalizeAuthority: false,
                cancellationToken)
            .ConfigureAwait(false);
        ZLinkRuntimeMetrics.RecordRelocationTargetResume(
            System.Diagnostics.Stopwatch.GetElapsedTime(
                resumeStartedTimestamp),
            cutover.Object.Kind == 3 ? "instance_spot" : "user_spot");
        runtime.ScheduleRelocationSessionRouteConvergence(stage);
    }

    private TargetStage RequireWireStage(
        ZLinkServiceWireCodec.RelocationWireId relocationId,
        ulong targetAttemptGeneration,
        ZLinkServiceWireCodec.RelocationCoordinatorFence coordinator,
        byte senderRole,
        ZLinkServiceWireCodec.RelocationObjectRecord relocationObject,
        RoutingId sourceNodeRid,
        string command)
    {
        var fence = new ZLinkAggregateFence(
            DecodeRelocationId(relocationId),
            targetAttemptGeneration);
        if (!_staged.TryGetValue(fence, out var entry)
            || entry is not TargetStage stage)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.NotFound,
                $"Command {command} has no prepared target.",
                ZLinkRetryAdvice.DoNotRetry);
        var spot = stage.SpotParticipant;
        var expectedKind = checked((byte)spot.ObjectKind);
        var expectedStableType = spot.ObjectKind
                                 == ZLinkPlacementObjectKind.InstanceSpot
            ? stage.StableType
            : string.Empty;
        var expectedAuthorityOwnerGeneration = spot.ObjectKind
                                               == ZLinkPlacementObjectKind.InstanceSpot
            ? 0UL
            : spot.AuthorityOwnerGeneration;
        if (senderRole != 1
            || stage.SourceNodeRid != sourceNodeRid
            || coordinator.NodeRid != sourceNodeRid
            || coordinator.NodeGeneration
               != stage.SourceNodeLifecycleGeneration
            || !StringComparer.Ordinal.Equals(
                coordinator.OwnerId,
                stage.SourceOwner.OwnerId)
            || coordinator.LeaseGeneration
               != checked((ulong)stage.SourceOwner.LeaseGeneration)
            || stage.CoordinatorExpectedAuthorityStoreVersion.Length != 0
            && !StringComparer.Ordinal.Equals(
                coordinator.ExpectedAuthorityStoreVersion,
                stage.CoordinatorExpectedAuthorityStoreVersion)
            || relocationObject.Kind != expectedKind
            || !StringComparer.Ordinal.Equals(
                relocationObject.StableType,
                expectedStableType)
            || !StringComparer.Ordinal.Equals(
                relocationObject.ObjectId,
                stage.Spot.Activation.SpotId)
            || relocationObject.ObjectGeneration != spot.ObjectGeneration
            || relocationObject.ExpectedAuthorityOwnerGeneration
               != expectedAuthorityOwnerGeneration)
            throw new InvalidDataException(
                $"Command {command} changed its prepared SPOT attempt.");
        return stage;
    }

    private async ValueTask CommitWireTargetAggregateAsync(
        TargetStage stage,
        CancellationToken cancellationToken)
    {
        var authorityStore = registration.Locations.ResolveStore()
                             ?? throw new ZLinkConfigurationException(
                                 "Location Store is not registered.");
        var relocationStore = registration.Locations.ResolveRelocationStore()
                              ?? throw new ZLinkConfigurationException(
                                  "Relocation Store is not registered.");
        var targetOwner = runtime.LocationLifecycle?.OwnerToken
                          ?? throw new ZLinkConfigurationException(
                              "The target owner lease is not established.");
        var participants = stage.Envelope.Participants.Select(state =>
        {
            var recovery = ZLinkCanonicalParticipantRecoveryCodec.Decode(
                stage.SourceRecoveries[state.AuthorityKey].Span);
            return new ZLinkAggregateRelocationParticipant(
                state with { RecoveryPayload = stage.SourceRecoveries[state.AuthorityKey] },
                recovery.ExpectedStoreVersion,
                ZLinkAuthorityGenerationTransition.NewOwner,
                recovery.AuthorityPayload,
                recovery.MembershipMutation);
        }).ToArray();
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            authorityStore,
            relocationStore);
        _ = await coordinator.PublishAsync(
                new ZLinkAggregateRelocationRequest(
                    stage.Envelope.AggregateId,
                    stage.Envelope.AggregateGeneration,
                    stage.TargetAttemptGeneration,
                    participants,
                    new ZLinkMeshNodeDescriptorKey(
                        stage.TargetMeshName,
                        stage.Node.Node.RoutingId),
                    stage.TargetNodeLifecycleGeneration,
                    new ZLinkCapacityVector(0, 0, null),
                    targetOwner,
                    stage.Envelope),
                cancellationToken)
            .ConfigureAwait(false);
    }

    private static Guid DecodeRelocationId(
        ZLinkServiceWireCodec.RelocationWireId relocationId)
    {
        Span<byte> bytes = stackalloc byte[16];
        BinaryPrimitives.WriteUInt64BigEndian(
            bytes[..8],
            relocationId.High);
        BinaryPrimitives.WriteUInt64BigEndian(
            bytes[8..],
            relocationId.Low);
        return new Guid(bytes, bigEndian: true);
    }

    private async ValueTask CompleteInboundThenNormalizeAsync(
        TargetStage stage,
        CancellationToken cancellationToken)
    {
        await stage.PublishGate.WaitAsync(cancellationToken)
            .ConfigureAwait(false);
        try
        {
            await CompleteInboundThenNormalizeCoreAsync(
                    stage,
                    cancellationToken)
                .ConfigureAwait(false);
        }
        finally
        {
            stage.PublishGate.Release();
        }
    }

    private async ValueTask CompleteInboundThenNormalizeCoreAsync(
        TargetStage stage,
        CancellationToken cancellationToken)
    {
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"aggregate_target_publication_completed aggregate={stage.Envelope.AggregateId:N}");
        if (stage.AdmissionDrainTask is { } existingDrain)
            await existingDrain.ConfigureAwait(false);

        // Post-Ready convergence: admission already opened from the publish
        // path, so the replay and admission calls below are idempotent
        // no-ops that only re-drive an interrupted drain. This path owns
        // steady normalization, the final-root delete, and the tombstone.
        await runtime.CompleteInboundSpotAggregateReplayAsync(
                stage,
                cancellationToken)
            .ConfigureAwait(false);
        runtime.ScheduleRelocationSessionRouteConvergence(stage);
        await NormalizeAuthorityAsync(
                stage,
                cancellationToken,
                releaseFinalRoot: false)
            .ConfigureAwait(false);
        await runtime.OpenInboundSpotAggregateAdmissionAsync(stage)
            .ConfigureAwait(false);
        var root = stage.GetFinalRoot()
                   ?? throw new ZLinkRelocationDataLostException(
                       "Completed SPOT relocation lost its final root reference.");
        await DeleteFinalRootBestEffortAsync(root.Reference, cancellationToken)
            .ConfigureAwait(false);
        CompleteStage(stage, TargetStageTerminalOutcome.Completed);
    }

    internal async ValueTask<bool> StageInboundAsync(
        ZLinkCanonicalSpotStageContext request,
        ZLinkRelocationEnvelope transferredEnvelope,
        RoutingId sourceNodeRid,
        CancellationToken cancellationToken)
    {
        await CleanupExpiredAsync().ConfigureAwait(false);
        if (sourceNodeRid.ToHex() != request.SourceNodeRid)
            return false;
        var targetNode = runtime.GetSpotNodeRuntime(
            RoutingId.FromHex(request.TargetNodeRid));
        var targetStatus = targetNode.Node.MeshStatus();
        var localOwner = runtime.LocationLifecycle?.OwnerToken;
        if (request.SourceNodeLifecycleGeneration == 0
            || request.SourceOwnerLeaseGeneration == 0
            || string.IsNullOrWhiteSpace(request.SourceOwnerId)
            || targetStatus.MeshName != request.MeshName
            || targetStatus.RoutingId.ToHex() != request.TargetNodeRid
            || targetStatus.LifecycleGeneration
               != request.TargetNodeLifecycleGeneration
            || localOwner is not { } exactLocalOwner
            || exactLocalOwner.OwnerId != request.TargetOwnerId
            || exactLocalOwner.LeaseGeneration
               != checked((long)request.TargetOwnerLeaseGeneration))
            return false;
        var fence = new ZLinkAggregateFence(
            request.AggregateId,
            request.AggregateGeneration);
        if (_staged.TryGetValue(fence, out var existing))
            return existing switch
            {
                TargetStage active =>
                    active.AbortState == TargetStageAbortState.Staged
                    && active.Matches(request, sourceNodeRid),
                TargetStageTombstone terminal => terminal.Matches(
                    request,
                    sourceNodeRid,
                    requireCompleted: true),
                _ => false
            };
        var (envelope, sourceRecoveries) = await BindCanonicalAuthorityInventoryAsync(
                registration.Locations.ResolveStore()
                ?? throw new ZLinkConfigurationException(
                    "Location Store is not registered."),
                transferredEnvelope,
                request,
                cancellationToken)
            .ConfigureAwait(false);
        if (envelope.AggregateId != request.AggregateId
            || envelope.AggregateGeneration != request.AggregateGeneration)
            throw new InvalidDataException(
                "SPOT relocation aggregate fence does not match its root.");
        if (!ValidateActorDescriptors(envelope, request))
            return false;
        if (!await IsSourceFenceCurrentAsync(
                request,
                envelope,
                cancellationToken).ConfigureAwait(false))
            return false;
        TargetStage stage;
        try
        {
            stage = await runtime.StageInboundSpotAggregateAsync(
                    request,
                    envelope,
                    reservedTargetAuthorityOwnerGeneration: 0,
                    cancellationToken,
                    stagedBeforeCutover: true)
                .ConfigureAwait(false);
            stage = stage with { SourceRecoveries = sourceRecoveries };
        }
        catch
        {
            throw;
        }
        if (!TryTrackStage(fence, stage))
        {
            await AbortTargetStageAsync(stage)
                .ConfigureAwait(false);
            return
                _staged.TryGetValue(fence, out var winner)
                && winner is TargetStage winningStage
                && winningStage.Matches(request, sourceNodeRid);
        }
        ScheduleReconciliation();
        return true;
    }

    internal static async ValueTask<(ZLinkRelocationEnvelope Envelope,
        IReadOnlyDictionary<ZLinkAuthorityKey, ReadOnlyMemory<byte>> SourceRecoveries)>
        BindCanonicalAuthorityInventoryAsync(
            IZLinkLocationRepository authorityStore,
            ZLinkRelocationEnvelope envelope,
            ZLinkCanonicalSpotStageContext request,
            CancellationToken cancellationToken)
    {
        if (envelope.CanonicalLogicalStream.IsEmpty)
            return (envelope, envelope.Participants.ToDictionary(
                static state => state.AuthorityKey,
                static state => state.RecoveryPayload));
        var orderedStates = envelope.Participants
            .OrderBy(static participant => participant.CanonicalParticipantId)
            .ToArray();
        if (orderedStates.Length != request.Actors.Length + 1)
            throw new ZLinkRelocationDataLostException(
                "Canonical relocation state count does not match its authority inventory.");
        var keys = new[]
            {
                ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(request.SpotId)
            }
            .Concat(request.Actors
                .Select(static actor =>
                    ZLinkActorAuthorityPayloadCodec.AuthorityKey(actor.ActorId))
                .OrderBy(static key => key.Value, StringComparer.Ordinal))
            .ToArray();
        var bound = new ZLinkRelocationParticipantEnvelope[orderedStates.Length];
        var sourceRecoveries =
            new Dictionary<ZLinkAuthorityKey, ReadOnlyMemory<byte>>();
        for (var index = 0; index < keys.Length; index++)
        {
            var read = await authorityStore.ReadAuthorityAsync(
                    keys[index],
                    cancellationToken)
                .ConfigureAwait(false);
            if (read is not ZLinkAuthorityReadResult.Found found)
                throw new ZLinkRelocationDataLostException(
                    $"Canonical relocation authority '{keys[index].Value}' is unavailable.");
            var state = orderedStates[index];
            var objectKind = index == 0
                ? request.InstanceSpot
                    ? ZLinkPlacementObjectKind.InstanceSpot
                    : ZLinkPlacementObjectKind.UserSpot
                : ZLinkPlacementObjectKind.Actor;
            var stableType = index == 0
                ? request.StableType
                : request.Actors[index - 1].StableType;
            var authorityPayload = index == 0
                ? found.Snapshot.Payload
                : request.Actors[index - 1].AuthorityPayload;
            bound[index] = state with
            {
                AuthorityKey = keys[index],
                ObjectKind = objectKind,
                ObjectGeneration = found.Snapshot.ObjectGeneration,
                AuthorityOwnerGeneration = found.Snapshot.AuthorityOwnerGeneration
            };
            sourceRecoveries.Add(keys[index],
                ZLinkCanonicalParticipantRecoveryCodec.Encode(
                    new ZLinkCanonicalParticipantRecovery(
                        keys[index],
                        objectKind,
                        found.Snapshot.ObjectGeneration,
                        found.Snapshot.AuthorityOwnerGeneration,
                        found.Snapshot.StoreVersion,
                        stableType,
                        authorityPayload,
                        ReadOnlyMemory<byte>.Empty)));
        }
        return (envelope with
        {
            AggregateGeneration = request.AggregateGeneration,
            Participants = bound
        }, sourceRecoveries);
    }

    private async ValueTask<bool> IsSourceFenceCurrentAsync(
        ZLinkCanonicalSpotStageContext request,
        ZLinkRelocationEnvelope envelope,
        CancellationToken cancellationToken)
    {
        var authorityStore = registration.Locations.ResolveStore();
        if (authorityStore is null)
            return false;
        var spot = envelope.Participants.Single(
            static participant => participant.ObjectKind
                is ZLinkPlacementObjectKind.UserSpot
                or ZLinkPlacementObjectKind.InstanceSpot);
        var requestAggregateBytes = request.AggregateId.ToByteArray(
            bigEndian: true);
        var requestRelocationHigh = BinaryPrimitives.ReadUInt64BigEndian(
            requestAggregateBytes.AsSpan(0, 8));
        var requestRelocationLow = BinaryPrimitives.ReadUInt64BigEndian(
            requestAggregateBytes.AsSpan(8, 8));
        ZLinkAuthoritySnapshot? spotSnapshot = null;
        foreach (var participant in envelope.Participants)
        {
            var read = await authorityStore.ReadAuthorityAsync(
                    participant.AuthorityKey,
                    cancellationToken)
                .ConfigureAwait(false);
            if (read is not ZLinkAuthorityReadResult.Found found
                || found.Snapshot.ObjectGeneration
                   != participant.ObjectGeneration
                || found.Snapshot.AuthorityOwnerGeneration
                   != participant.AuthorityOwnerGeneration
                || found.Snapshot.OwnerId != request.SourceOwnerId
                || found.Snapshot.OwnerLeaseGeneration <= 0
                || checked((ulong)found.Snapshot.OwnerLeaseGeneration)
                   != request.SourceOwnerLeaseGeneration)
                return false;

            // A durable relocation state for the same RelocationId that
            // already names a different target fences this staging request:
            // the contract has no different-target transition, so a second
            // target for the same relocation is always illegitimate.
            if (ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                    found.Snapshot.Payload.Span,
                    out var recordedRelocation)
                && recordedRelocation.RelocationHigh == requestRelocationHigh
                && recordedRelocation.RelocationLow == requestRelocationLow
                && !string.IsNullOrEmpty(
                    recordedRelocation.State.TargetNodeRid)
                && !StringComparer.Ordinal.Equals(
                    recordedRelocation.State.TargetNodeRid,
                    request.TargetNodeRid))
                return false;

            if (participant.ObjectKind == ZLinkPlacementObjectKind.Actor)
            {
                if (!ZLinkActorAuthorityPayloadCodec.TryDecode(
                        found.Snapshot.Payload.Span,
                        out var actor)
                    || actor.MeshName != request.MeshName
                    || actor.NodeRid.ToHex() != request.SourceNodeRid
                    || actor.NodeGeneration
                       != request.SourceNodeLifecycleGeneration)
                    return false;
            }
            else
            {
                spotSnapshot = found.Snapshot;
            }
        }
        if (spotSnapshot is null)
            return false;
        if (request.InstanceSpot)
            return ZLinkInstanceSpotAuthorityPayloadCodec.TryDecode(
                       spotSnapshot.Payload.Span,
                   out var instance)
                   && instance.SpotId == request.SpotId
                   && instance.StableType == request.StableType
                   && instance.MeshName == request.MeshName
                   && instance.NodeRid.ToHex() == request.SourceNodeRid
                   && instance.NodeGeneration
                      == request.SourceNodeLifecycleGeneration;
        return ZLinkUserSpotAuthorityPayloadCodec.TryDecode(
                   spotSnapshot.Payload.Span,
               out var user)
               && user.SpotId == request.SpotId
               && user.StableType == request.StableType
               && user.MeshName == request.MeshName
               && user.NodeRid.ToHex() == request.SourceNodeRid
                   && user.NodeGeneration
                  == request.SourceNodeLifecycleGeneration;
    }

    private static bool ValidateActorDescriptors(
        ZLinkRelocationEnvelope envelope,
        ZLinkCanonicalSpotStageContext request)
    {
        var participants = envelope.Participants
            .Where(static participant =>
                participant.ObjectKind == ZLinkPlacementObjectKind.Actor)
            .ToDictionary(
                static participant => participant.AuthorityKey,
                static participant => participant);
        if (participants.Count != request.Actors.Length)
            return false;
        var actorIds = new HashSet<string>(StringComparer.Ordinal);
        foreach (var descriptor in request.Actors)
        {
            if (!actorIds.Add(descriptor.ActorId)
                || !ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
                    descriptor.AuthorityPayload,
                    out var actor)
                || actor.ActorId != descriptor.ActorId
                || actor.StableType != descriptor.StableType
                || !participants.ContainsKey(
                    ZLinkActorAuthorityPayloadCodec.AuthorityKey(
                        descriptor.ActorId)))
                return false;
        }
        return true;
    }

    internal async ValueTask<bool> PublishInboundAsync(
        ZLinkAggregateFence fence,
        RoutingId sourceNodeRid,
        CancellationToken cancellationToken)
    {
        await CleanupExpiredAsync().ConfigureAwait(false);
        if (!_staged.TryGetValue(fence, out var entry))
            return false;
        if (entry is TargetStageTombstone terminal)
            return
                terminal.SourceNodeRid == sourceNodeRid
                && terminal.Outcome == TargetStageTerminalOutcome.Completed;
        if (entry is not TargetStage stage
            || stage.SourceNodeRid != sourceNodeRid
            || stage.AbortState != TargetStageAbortState.Staged)
            return false;
        if (await IsAuthorityNormalizedAsync(stage, cancellationToken)
                .ConfigureAwait(false))
        {
            Volatile.Write(ref stage.AuthorityPublished, 1);
            await FinalizeStageAsync(
                    stage,
                    normalizeAuthority: true,
                    cancellationToken)
                .ConfigureAwait(false);
            CompleteStage(stage, TargetStageTerminalOutcome.Completed);
            return true;
        }
        return
            await ReconcileStageAuthorityAsync(
                    stage,
                    cancellationToken)
                .ConfigureAwait(false) is not null;
    }

    private async ValueTask FinalizeStageAsync(
        TargetStage stage,
        bool normalizeAuthority,
        CancellationToken cancellationToken)
    {
        if (!await IsAuthorityNormalizedAsync(stage, cancellationToken)
                .ConfigureAwait(false))
            await ValidatePublishedRootAsync(
                    stage,
                    cancellationToken)
                .ConfigureAwait(false);
        await runtime.PublishInboundSpotAggregateAsync(
                stage,
                normalizeAuthority
                    ? token => NormalizeAuthorityAsync(stage, token)
                    : null,
                cancellationToken)
            .ConfigureAwait(false);
        runtime.ScheduleRelocationSessionRouteConvergence(stage);
        if (stage.Envelope.CanonicalLogicalStream.IsEmpty)
            return;
        // Admission opens from the publish path once the queue merge is
        // complete: replay orders the staged journal and held ingress, then
        // the trailing-reserve-then-open primitive orders followed frames
        // before direct ingress. Source cleanup, one-way session route update,
        // and steady normalization converge asynchronously and never gate
        // admission.
        await stage.PublishGate.WaitAsync(cancellationToken)
            .ConfigureAwait(false);
        try
        {
            await runtime.CompleteInboundSpotAggregateReplayAsync(
                    stage,
                    cancellationToken)
                .ConfigureAwait(false);
            await runtime.OpenInboundSpotAggregateAdmissionAsync(stage)
                .ConfigureAwait(false);
        }
        finally
        {
            stage.PublishGate.Release();
        }
    }

    private async ValueTask ValidatePublishedRootAsync(
        TargetStage stage,
        CancellationToken cancellationToken)
    {
        var authorityStore = registration.Locations.ResolveStore()
                             ?? throw new ZLinkConfigurationException(
                                 "Location Store is not registered.");
        var relocationStore =
            registration.Locations.ResolveRelocationStore()
            ?? throw new ZLinkConfigurationException(
                "Relocation Store is not registered.");
        var stagedSpot = stage.SpotParticipant;
        var read = await authorityStore.ReadAuthorityAsync(
                stagedSpot.AuthorityKey,
                cancellationToken)
            .ConfigureAwait(false);
        if (read is not ZLinkAuthorityReadResult.Found found
            || !ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                found.Snapshot.Payload.Span,
                out var publication)
            || publication.AggregateId != stage.Envelope.AggregateId
            || publication.AggregateGeneration
               != stage.Envelope.AggregateGeneration
            || string.IsNullOrEmpty(publication.Reference))
            throw new ZLinkRelocationDataLostException(
                "Canonical relocation publication is unavailable.");
        var durable = await ZLinkRelocationTreeStore.GetAsync(
                relocationStore,
                publication.Reference,
                publication.ChecksumCrc32c,
                cancellationToken)
            .ConfigureAwait(false);
        if (!publication.IsCanonical
            || durable.AggregateId != stage.Envelope.AggregateId
            || durable.Participants.Count != stage.Envelope.Participants.Count
            || !durable.InventoryDigest.Span.SequenceEqual(
                stage.Envelope.InventoryDigest.Span))
            throw new ZLinkRelocationDataLostException(
                "Canonical relocation publication does not match target staging.");
        stage.RememberFinalRoot(
            publication.Reference,
            publication.ChecksumCrc32c);
    }

    private async ValueTask AbortTargetStageAsync(TargetStage stage)
    {
        await runtime.AbortInboundSpotAggregateAsync(stage)
            .ConfigureAwait(false);
    }


    internal async ValueTask<bool> AbortInboundAsync(
        ZLinkAggregateFence fence,
        RoutingId sourceNodeRid,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        await CleanupExpiredAsync().ConfigureAwait(false);
        if (!_staged.TryGetValue(fence, out var candidate))
            return true;
        if (candidate is TargetStageTombstone terminal)
            return
                terminal.SourceNodeRid == sourceNodeRid
                && terminal.Outcome == TargetStageTerminalOutcome.Aborted;
        if (candidate is not TargetStage stage
            || stage.SourceNodeRid != sourceNodeRid)
            return false;

        if (stage.AbortState == TargetStageAbortState.Aborted)
            return true;
        if (Volatile.Read(ref stage.AuthorityPublished) != 0
            || Volatile.Read(ref stage.Published) != 0)
            return false;
        var authorityStore = registration.Locations.ResolveStore()
            ?? throw new ZLinkConfigurationException(
                "Location Store is not registered.");
        return await stage.RunAbortCleanupAsync(
                () => TryCleanupExpiredStageAsync(
                    stage,
                    () => ReconcileStageAuthorityAsync(
                        stage, CancellationToken.None),
                    () => authorityStore.AbortAggregateAsync(
                        fence, CancellationToken.None),
                    () => TryCompleteStage(
                        fence, stage, TargetStageTerminalOutcome.Aborted),
                    () => runtime.AbortInboundSpotAggregateAsync(stage)),
                cancellationToken)
            .ConfigureAwait(false);
    }

    internal static bool IsStagingPrefix(
        ZLinkRelocationEnvelope staging,
        ZLinkRelocationEnvelope authoritative,
        Func<ZLinkRelocationParticipantEnvelope, ulong>?
            targetAuthorityOwnerGeneration = null)
    {
        if (staging.AggregateId != authoritative.AggregateId
            || !staging.InventoryDigest.Span.SequenceEqual(
                authoritative.InventoryDigest.Span)
            || staging.Participants.Count
               != authoritative.Participants.Count
            || staging.CanonicalLogicalStream.IsEmpty
               != authoritative.CanonicalLogicalStream.IsEmpty
            || !staging.CanonicalLogicalStream.IsEmpty
               && (staging.CanonicalRelocationHigh
                       != authoritative.CanonicalRelocationHigh
                   || staging.CanonicalRelocationLow
                       != authoritative.CanonicalRelocationLow
                   || staging.CanonicalApplicationVersion
                       != authoritative.CanonicalApplicationVersion))
            return false;
        var canonical = !staging.CanonicalLogicalStream.IsEmpty;
        if (staging.AggregateGeneration != authoritative.AggregateGeneration)
            return false;
        var authoritativeByKey = authoritative.Participants.ToDictionary(
            static participant => participant.AuthorityKey,
            static participant => participant);
        foreach (var expected in staging.Participants)
        {
            if (!authoritativeByKey.TryGetValue(
                    expected.AuthorityKey,
                    out var actual)
                || expected.ObjectKind != actual.ObjectKind
                || expected.ObjectGeneration != actual.ObjectGeneration
                || !AuthorityGenerationMatches(
                    expected,
                    actual,
                    targetAuthorityOwnerGeneration)
                || !expected.ApplicationState.Span.SequenceEqual(
                    actual.ApplicationState.Span)
                || !expected.RecoveryPayload.Span.SequenceEqual(
                    actual.RecoveryPayload.Span)
                || expected.ObjectKind is not (
                    ZLinkPlacementObjectKind.UserSpot
                    or ZLinkPlacementObjectKind.InstanceSpot)
                   && !expected.CompletionPayload.Span.SequenceEqual(
                       actual.CompletionPayload.Span)
                || !SameTimers(
                    expected.LogicalTimers,
                    actual.LogicalTimers)
                || actual.AcceptedJobs.Count
                   < expected.AcceptedJobs.Count
                || expected.ObjectKind is not (
                    ZLinkPlacementObjectKind.UserSpot
                    or ZLinkPlacementObjectKind.InstanceSpot)
                   && actual.AcceptedJobs.Count
                      != expected.AcceptedJobs.Count)
                return false;
            if (canonical
                && expected.CanonicalParticipantId
                   != actual.CanonicalParticipantId)
                return false;
            for (var index = 0;
                 index < expected.AcceptedJobs.Count;
                 index++)
                if (expected.AcceptedJobs[index].AcceptedSequence
                        != actual.AcceptedJobs[index].AcceptedSequence
                    || !expected.AcceptedJobs[index].Payload.Span
                        .SequenceEqual(
                            actual.AcceptedJobs[index].Payload.Span))
                    return false;
            for (var index = 1;
                 index < actual.AcceptedJobs.Count;
                 index++)
                if (actual.AcceptedJobs[index - 1].AcceptedSequence
                    >= actual.AcceptedJobs[index].AcceptedSequence)
                    return false;
        }
        return true;

        static bool AuthorityGenerationMatches(
            ZLinkRelocationParticipantEnvelope expected,
            ZLinkRelocationParticipantEnvelope actual,
            Func<ZLinkRelocationParticipantEnvelope, ulong>?
                targetAuthorityOwnerGeneration)
        {
            if (targetAuthorityOwnerGeneration is not null)
                return actual.AuthorityOwnerGeneration
                       == targetAuthorityOwnerGeneration(expected);
            return expected.AuthorityOwnerGeneration
                   == actual.AuthorityOwnerGeneration;
        }


        static bool SameTimers(
            IReadOnlyList<ZLinkRelocationLogicalTimer> expected,
            IReadOnlyList<ZLinkRelocationLogicalTimer> actual)
        {
            if (expected.Count != actual.Count)
                return false;
            for (var index = 0; index < expected.Count; index++)
                if (expected[index].TimerId != actual[index].TimerId
                    || expected[index].DueUnixTimeMilliseconds
                       != actual[index].DueUnixTimeMilliseconds
                    || expected[index].PeriodMilliseconds
                       != actual[index].PeriodMilliseconds
                    || !expected[index].Payload.Span.SequenceEqual(
                        actual[index].Payload.Span))
                    return false;
            return true;
        }
    }

    private async ValueTask<bool> IsAuthorityNormalizedAsync(
        TargetStage stage,
        CancellationToken cancellationToken)
    {
        var authorityStore = registration.Locations.ResolveStore();
        var localOwner = runtime.LocationLifecycle?.OwnerToken;
        if (authorityStore is null || localOwner is not { } exactLocalOwner)
            return false;
        var reads = await Task.WhenAll(stage.Envelope.Participants.Select(
                async participant => (
                    Participant: participant,
                    Read: await authorityStore.ReadAuthorityAsync(
                            participant.AuthorityKey,
                            cancellationToken)
                        .ConfigureAwait(false))))
            .ConfigureAwait(false);
        foreach (var (participant, read) in reads)
        {
            if (read is not ZLinkAuthorityReadResult.Found found
                || found.Snapshot.ObjectGeneration
                   != participant.ObjectGeneration
                || participant.AuthorityOwnerGeneration
                   is 0 or > long.MaxValue
                || found.Snapshot.AuthorityOwnerGeneration
                   is 0 or > long.MaxValue
                || found.Snapshot.OwnerId != exactLocalOwner.OwnerId
                || found.Snapshot.OwnerLeaseGeneration
                   != exactLocalOwner.LeaseGeneration
                || !ZLinkAggregateRelocationCoordinator
                    .IsExactNormalizedTargetAuthority(
                        found.Snapshot,
                        participant with
                        {
                            RecoveryPayload = stage.SourceRecoveries.GetValueOrDefault(
                                participant.AuthorityKey)
                        },
                        stage.Envelope.AggregateId,
                        new ZLinkMeshNodeDescriptorKey(
                            stage.SourceMeshName,
                            stage.Node.Node.RoutingId),
                        stage.Node.Node.MeshStatus().LifecycleGeneration,
                        exactLocalOwner,
                        allowCurrentAuthorityGeneration:
                            stage.TargetAuthorityOwnerGenerationFor(
                                participant)
                            == participant.AuthorityOwnerGeneration))
                return false;
        }
        return true;
    }

    private async ValueTask NormalizeAuthorityAsync(
        TargetStage stage,
        CancellationToken cancellationToken,
        bool releaseFinalRoot = true)
    {
        if (await IsAuthorityNormalizedAsync(stage, cancellationToken)
                .ConfigureAwait(false))
        {
            if (releaseFinalRoot)
                await ReleaseFinalRootAfterNormalizationAsync(
                        stage,
                        authorityAlreadyNormalized: true,
                        commitNormalization: null,
                        reference => DeleteFinalRootBestEffortAsync(
                            reference,
                            cancellationToken))
                    .ConfigureAwait(false);
            return;
        }
        var authorityStore = registration.Locations.ResolveStore()
                             ?? throw new ZLinkConfigurationException(
                                 "Location Store is not registered.");
        var localOwner = runtime.LocationLifecycle?.OwnerToken
                         ?? throw new ZLinkConfigurationException(
                             "Location runtime is not registered.");
        var participants =
            new List<ZLinkAggregateRelocationParticipant>(
                stage.Envelope.Participants.Count);
        var expectedPublicationGeneration = stage.Envelope.AggregateGeneration;
        string? publicationReference = null;
        uint publicationChecksum = 0;
        ReadOnlyMemory<byte> publicationInventoryDigest = default;
        bool? canonicalPublication = null;
        var authorityReads = await Task.WhenAll(
                stage.Envelope.Participants.Select(
                    async participant => (
                        Participant: participant,
                        Read: await authorityStore.ReadAuthorityAsync(
                                participant.AuthorityKey,
                                cancellationToken)
                            .ConfigureAwait(false))))
            .ConfigureAwait(false);
        foreach (var (participant, read) in authorityReads)
        {
            if (read is not ZLinkAuthorityReadResult.Found found
                || !ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                    found.Snapshot.Payload.Span,
                    out var publication)
                || publication.AggregateId != stage.Envelope.AggregateId
                || publication.AggregateGeneration
                   != expectedPublicationGeneration
                || publication.TargetOwnerId != localOwner.OwnerId
                || publication.TargetOwnerLeaseGeneration
                   != localOwner.LeaseGeneration)
                throw new ZLinkRelocationDataLostException(
                    $"Relocation authority '{participant.AuthorityKey.Value}' cannot be normalized.");
            if (publicationReference is null)
            {
                publicationReference = publication.Reference;
                publicationChecksum = publication.ChecksumCrc32c;
                canonicalPublication = publication.IsCanonical;
                publicationInventoryDigest = publication.IsCanonical
                    ? ReadOnlyMemory<byte>.Empty
                    : publication.InventoryDigest;
            }
            else if (publication.Reference != publicationReference
                     || publication.ChecksumCrc32c
                     != publicationChecksum
                     || canonicalPublication.GetValueOrDefault()
                        != publication.IsCanonical
                     || !publication.IsCanonical
                        && !publication.InventoryDigest.Span.SequenceEqual(
                            publicationInventoryDigest.Span))
            {
                throw new ZLinkRelocationDataLostException(
                    "Relocation authorities disagree on the held ingress completion root.");
            }
            var ready = BuildTargetReadyPayload(
                participant.ObjectKind,
                publication.ApplicationPayload,
                stage,
                localOwner);
            participants.Add(new ZLinkAggregateRelocationParticipant(
                participant,
                found.Snapshot.StoreVersion,
                ZLinkAuthorityGenerationTransition.Preserve,
                ready,
                ReadOnlyMemory<byte>.Empty));
        }
        if (publicationReference is null)
            throw new ZLinkRelocationDataLostException(
                "Relocation authorities have no final completion root.");
        stage.RememberFinalRoot(
            publicationReference,
            publicationChecksum);

        // A retry after process recovery must address the same normalization
        // transaction. Reusing the relocation id with the next generation
        // gives the authority Store an exact idempotency fence.
        var aggregateId = stage.Envelope.AggregateId;
        var aggregateGeneration = checked(
            stage.Envelope.AggregateGeneration
            + (stage.Envelope.CanonicalLogicalStream.IsEmpty ? 1UL : 2UL));
        var targetStatus = stage.Node.Node.MeshStatus();
        const bool allowPreparingTarget = true;
        var prepare = await authorityStore.PrepareAggregateAsync(
                new ZLinkAggregatePrepareRequest(
                    aggregateId,
                    aggregateGeneration,
                    participants.Select(static participant =>
                            new ZLinkAggregateParticipant(
                                participant.Envelope.AuthorityKey,
                                participant.ExpectedStoreVersion,
                                participant.OwnerTransition,
                                participant.ApplicationAuthorityPayload,
                                participant.MembershipMutation))
                        .ToArray(),
                    ZLinkAggregateInventoryDigest.Compute(participants),
                    new ZLinkMeshNodeDescriptorKey(
                        stage.SourceMeshName,
                        stage.Node.Node.RoutingId),
                    targetStatus.LifecycleGeneration,
                    new ZLinkCapacityVector(0, 0, null),
                    localOwner,
                    AllowPreparingTarget: allowPreparingTarget),
                cancellationToken)
            .ConfigureAwait(false);
        var fence = prepare switch
        {
            ZLinkAggregatePrepareResult.Prepared value => value.Fence,
            ZLinkAggregatePrepareResult.AlreadyPrepared value => value.Fence,
            ZLinkAggregatePrepareResult.GenerationExhausted =>
                throw new ZLinkAuthorityGenerationExhaustedException(
                    "preparing SPOT steady authority normalization"),
            _ => throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"SPOT '{stage.Spot.Activation.SpotId}' steady authority normalization conflicted.",
                ZLinkRetryAdvice.RetryAfterBackoff)
        };
        if (releaseFinalRoot)
            await ReleaseFinalRootAfterNormalizationAsync(
                    stage,
                    authorityAlreadyNormalized: false,
                    () => authorityStore.CommitAggregateForRecoveryAsync(
                        fence,
                        cancellationToken),
                    reference => DeleteFinalRootBestEffortAsync(
                        reference,
                        cancellationToken))
                .ConfigureAwait(false);
        else
            await CommitNormalizationRetainingRootAsync(
                    authorityStore,
                    fence,
                    cancellationToken)
                .ConfigureAwait(false);
    }

    private static async ValueTask CommitNormalizationRetainingRootAsync(
        IZLinkLocationRepository authorityStore,
        ZLinkAggregateFence fence,
        CancellationToken cancellationToken)
    {
        var commit = await authorityStore.CommitAggregateForRecoveryAsync(
                fence,
                cancellationToken)
            .ConfigureAwait(false);
        if (commit == ZLinkAggregateCommitResult.GenerationExhausted)
            throw new ZLinkAuthorityGenerationExhaustedException(
                "committing SPOT steady authority normalization");
        if (commit is not (
                ZLinkAggregateCommitResult.Committed
                or ZLinkAggregateCommitResult.AlreadyCommitted))
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                "SPOT steady authority normalization failed.",
                ZLinkRetryAdvice.RetryAfterBackoff);
    }

    internal static async ValueTask ReleaseFinalRootAfterNormalizationAsync(
        TargetStage stage,
        bool authorityAlreadyNormalized,
        Func<ValueTask<ZLinkAggregateCommitResult>>? commitNormalization,
        Func<string, ValueTask> deleteRoot)
    {
        ArgumentNullException.ThrowIfNull(stage);
        ArgumentNullException.ThrowIfNull(deleteRoot);
        if (!authorityAlreadyNormalized)
        {
            ArgumentNullException.ThrowIfNull(commitNormalization);
            var commit = await commitNormalization().ConfigureAwait(false);
            if (commit == ZLinkAggregateCommitResult.GenerationExhausted)
                throw new ZLinkAuthorityGenerationExhaustedException(
                    "committing SPOT steady authority normalization");
            if (commit is not (
                    ZLinkAggregateCommitResult.Committed
                    or ZLinkAggregateCommitResult.AlreadyCommitted))
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    "SPOT steady authority normalization failed.",
                    ZLinkRetryAdvice.RetryAfterBackoff);
        }
        var root = stage.GetFinalRoot()
                   ?? throw new ZLinkRelocationDataLostException(
                       "Normalized SPOT relocation lost its final root reference.");
        await deleteRoot(root.Reference).ConfigureAwait(false);
    }

    private async ValueTask DeleteFinalRootAsync(
        string reference,
        CancellationToken cancellationToken)
    {
        var relocationStore =
            registration.Locations.ResolveRelocationStore()
            ?? throw new ZLinkConfigurationException(
                "Relocation Store is not registered.");
        await ZLinkRelocationTreeStore.DeleteTreeAsync(
                relocationStore,
                reference,
                cancellationToken)
            .ConfigureAwait(false);
    }

    private async ValueTask DeleteFinalRootBestEffortAsync(
        string reference,
        CancellationToken cancellationToken)
    {
        try
        {
            await DeleteFinalRootAsync(reference, cancellationToken)
                .ConfigureAwait(false);
        }
        catch (Exception exception)
        {
            runtime.ErrorSink.ReportRuntimeTaskException(
                "spot-relocation-root-cleanup",
                exception);
        }
    }

    private static ReadOnlyMemory<byte> BuildTargetReadyPayload(
        ZLinkPlacementObjectKind kind,
        ReadOnlyMemory<byte> sourcePayload,
        TargetStage stage,
        ZLinkLocationOwnerToken targetOwner) =>
        BuildTargetReadyPayload(
            kind,
            sourcePayload,
            stage.Node.Node.RoutingId,
            stage.Node.Node.MeshStatus().LifecycleGeneration,
            targetOwner,
            stage.Envelope.AggregateId);

    internal static ReadOnlyMemory<byte> BuildTargetReadyPayload(
        ZLinkPlacementObjectKind kind,
        ReadOnlyMemory<byte> sourcePayload,
        RoutingId targetNode,
        ulong targetNodeGeneration,
        ZLinkLocationOwnerToken targetOwner,
        Guid relocationId)
    {
        var ownerGeneration = checked((ulong)targetOwner.LeaseGeneration);
        if (kind == ZLinkPlacementObjectKind.UserSpot
            && ZLinkUserSpotAuthorityPayloadCodec.TryDecode(
                sourcePayload.Span,
                out var user))
            return ZLinkUserSpotAuthorityPayloadCodec.Encode(user with
            {
                State = ZLinkUserSpotAuthorityState.Ready,
                OwnerId = targetOwner.OwnerId,
                OwnerLeaseGeneration = ownerGeneration,
                NodeRid = targetNode,
                NodeGeneration = targetNodeGeneration
            });
        if (kind == ZLinkPlacementObjectKind.InstanceSpot
            && ZLinkInstanceSpotAuthorityPayloadCodec.TryDecode(
                sourcePayload.Span,
                out var instance))
            return ZLinkInstanceSpotAuthorityPayloadCodec.Encode(
                instance with
                {
                    State = ZLinkInstanceSpotAuthorityState.Ready,
                    OwnerId = targetOwner.OwnerId,
                    OwnerLeaseGeneration = ownerGeneration,
                    NodeRid = targetNode,
                    NodeGeneration = targetNodeGeneration
                });
        if (kind == ZLinkPlacementObjectKind.Actor
            && ZLinkActorRelocationAuthorityPayloadCodec.TryDecode(
                sourcePayload.Span,
                out var relocation)
            && relocation.RelocationId == relocationId
            && relocation.Phase
               == ZLinkActorRelocationAuthorityPhase.Activated
            && ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
                relocation.ApplicationPayload.Span,
                out var actor))
            return ZLinkActorRelocationAuthorityPayloadCodec.Encode(
                relocation with
                {
                    Phase = ZLinkActorRelocationAuthorityPhase.Steady,
                    ApplicationPayload =
                        ZLinkActorAuthorityPayloadCodec.Encode(actor with
                        {
                            State = ZLinkActorAuthorityState.Ready,
                            OwnerId = targetOwner.OwnerId,
                            OwnerLeaseGeneration = ownerGeneration,
                            NodeRid = targetNode,
                            NodeGeneration = targetNodeGeneration
                        })
                });
        if (kind == ZLinkPlacementObjectKind.Actor
            && ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
                sourcePayload.Span,
                out var canonicalActor))
            return ZLinkActorAuthorityPayloadCodec.Encode(
                canonicalActor with
                {
                    State = ZLinkActorAuthorityState.Ready,
                    OwnerId = targetOwner.OwnerId,
                    OwnerLeaseGeneration = ownerGeneration,
                    NodeRid = targetNode,
                    NodeGeneration = targetNodeGeneration
                });
        throw new ZLinkRelocationDataLostException(
            $"Relocation participant kind '{kind}' has an invalid authority payload.");
    }

    private void ScheduleReconciliation()
    {
        if (_reconciliationWake.CurrentCount == 0)
            _reconciliationWake.Release();
        if (Interlocked.CompareExchange(
                ref _reconciliationRunning,
                1,
                0) != 0)
            return;
        if (!runtime.TryRunDetached(
                "spot-retire-target-reconciliation",
                RunReconciliationLoopAsync))
            Interlocked.Exchange(ref _reconciliationRunning, 0);
    }

    private async ValueTask RunReconciliationLoopAsync(
        CancellationToken cancellationToken)
    {
        try
        {
            Task? woken = null;
            while (!cancellationToken.IsCancellationRequested)
            {
                await CleanupExpiredAsync().ConfigureAwait(false);
                foreach (var stage in _staged.Values.OfType<TargetStage>().ToArray())
                {
                    // Published stages that are not yet steady-normalized stay
                    // in the loop so reconciliation can converge them within
                    // the final-root retention window.
                    if (stage.AbortState != TargetStageAbortState.Staged)
                        continue;
                    try
                    {
                        await ReconcileStageAuthorityAsync(
                                stage,
                                cancellationToken)
                            .ConfigureAwait(false);
                    }
                    catch (OperationCanceledException)
                        when (cancellationToken.IsCancellationRequested)
                    {
                        return;
                    }
                    catch (Exception exception)
                    {
                        runtime.ErrorSink.ReportRuntimeTaskException(
                            $"spot-retire-target-reconciliation:{stage.Envelope.AggregateId:N}",
                            exception);
                        if (exception is ZLinkAuthorityGenerationExhaustedException)
                        {
                            services.GetService<IZLinkRuntimeTerminalFailureSink>()?
                                .SealError(exception);
                            return;
                        }
                    }
                }

                var delay = Task.Delay(
                    registration.Locations.Options.PollingInterval,
                    cancellationToken);
                woken ??= _reconciliationWake.WaitAsync(cancellationToken);
                await Task.WhenAny(delay, woken).ConfigureAwait(false);
                if (woken.IsCompleted)
                    woken = null;
            }
        }
        catch (OperationCanceledException)
            when (cancellationToken.IsCancellationRequested)
        {
        }
        finally
        {
            Interlocked.Exchange(ref _reconciliationRunning, 0);
        }
    }

    private async ValueTask<ZLinkRelocationRecoveryCandidate?>
        ReconcileStageAuthorityAsync(
            TargetStage stage,
            CancellationToken cancellationToken)
    {
        if (await IsAuthorityNormalizedAsync(stage, cancellationToken)
                .ConfigureAwait(false))
        {
            Volatile.Write(ref stage.AuthorityPublished, 1);
            await FinalizeStageAsync(
                    stage,
                    normalizeAuthority: true,
                    cancellationToken)
                .ConfigureAwait(false);
            CompleteStage(stage, TargetStageTerminalOutcome.Completed);
            return null;
        }
        var authorityStore = registration.Locations.ResolveStore()
                             ?? throw new ZLinkConfigurationException(
                                 "Location Store is not registered.");
        var relocationStore =
            registration.Locations.ResolveRelocationStore()
            ?? throw new ZLinkConfigurationException(
                "Relocation Store is not registered.");
        var candidate = await new ZLinkRelocationStartupRecovery(
                authorityStore,
                relocationStore)
            .TryReadExactPublishedAsync(
                stage.Envelope,
                cancellationToken)
            .ConfigureAwait(false);
        if (candidate is not null)
            await ReconcilePublishedStageAsync(
                    stage,
                    candidate,
                    cancellationToken)
                .ConfigureAwait(false);
        return candidate;
    }

    private async ValueTask CleanupExpiredAsync()
    {
        var now = Stopwatch.GetElapsedTime(0);
        RemoveExpiredTombstones(now);
        await CleanupExpiredStagesAsync(now).ConfigureAwait(false);
    }

    internal void RemoveExpiredTombstones(TimeSpan now)
    {
        while (_terminalOrder.TryPeek(out var terminalFence)
               && (!_staged.TryGetValue(terminalFence, out var terminalEntry)
                   || terminalEntry is TargetStageTombstone tombstone
                   && tombstone.ExpiresAt <= now))
        {
            _terminalOrder.TryDequeue(out _);
            if (terminalEntry is TargetStageTombstone)
                _staged.TryRemove(terminalFence, out _);
        }
    }

    private async ValueTask CleanupExpiredStagesAsync(TimeSpan now)
    {
        foreach (var entry in _staged.Where(
                     entry => entry.Value is TargetStage stage
                              && IsCleanupCandidate(stage, now))
                 .ToArray())
        {
            var stage = (TargetStage)entry.Value;
            if (!_staged.TryGetValue(entry.Key, out var current)
                || !ReferenceEquals(current, stage))
                continue;
            var authorityStore = registration.Locations.ResolveStore()
                                 ?? throw new ZLinkConfigurationException(
                                     "Location Store is not registered.");
            _ = await stage.RunAbortCleanupAsync(
                    () => TryCleanupExpiredStageAsync(
                        stage,
                        () => ReconcileStageAuthorityAsync(
                            stage,
                            CancellationToken.None),
                        () => authorityStore.AbortAggregateAsync(
                            new ZLinkAggregateFence(
                                stage.Envelope.AggregateId,
                                stage.Envelope.AggregateGeneration),
                            CancellationToken.None),
                        () => TryCompleteStage(
                            entry.Key,
                            stage,
                            Volatile.Read(ref stage.Published) == 0
                                ? TargetStageTerminalOutcome.Aborted
                                : TargetStageTerminalOutcome.Completed),
                        () => runtime.AbortInboundSpotAggregateAsync(
                            stage)),
                    CancellationToken.None)
                .ConfigureAwait(false);
        }
    }

    internal static async ValueTask<bool> TryCleanupExpiredStageAsync(
        TargetStage stage,
        Func<ValueTask<ZLinkRelocationRecoveryCandidate?>> reconcileAuthority,
        Func<ValueTask<ZLinkAggregateAbortResult>> abortAggregate,
        Func<bool> remove,
        Func<ValueTask> abort)
    {
        ArgumentNullException.ThrowIfNull(stage);
        ArgumentNullException.ThrowIfNull(reconcileAuthority);
        ArgumentNullException.ThrowIfNull(abortAggregate);
        ArgumentNullException.ThrowIfNull(remove);
        ArgumentNullException.ThrowIfNull(abort);
        if (Volatile.Read(ref stage.Published) == 0)
        {
            var published = await reconcileAuthority().ConfigureAwait(false);
            if (published is not null
                || Volatile.Read(ref stage.AuthorityPublished) != 0)
                return false;
            var durableAbort = await abortAggregate().ConfigureAwait(false);
            if (durableAbort == ZLinkAggregateAbortResult.Stale)
            {
                _ = await reconcileAuthority().ConfigureAwait(false);
                return false;
            }
            if (durableAbort is not (
                    ZLinkAggregateAbortResult.Aborted
                    or ZLinkAggregateAbortResult.AlreadyAborted))
                return false;
        }
        if (Volatile.Read(ref stage.Published) == 0)
            await abort().ConfigureAwait(false);
        return remove();
    }

    private static bool IsCleanupCandidate(
        TargetStage stage,
        TimeSpan now) =>
        stage.ExpiresAt <= now
        && (Volatile.Read(ref stage.AuthorityPublished) == 0
            || Volatile.Read(ref stage.Published) != 0
               // Session route convergence runs detached after admission
               // opens; an expired but unconverged stage must survive so the
               // reconciliation poller can keep re-driving the route commit.
               && Volatile.Read(ref stage.SessionRoutesConverged) != 0);

    internal void CompleteStage(
        TargetStage stage,
        TargetStageTerminalOutcome outcome)
    {
        if (outcome == TargetStageTerminalOutcome.Completed
            && Volatile.Read(ref stage.Published) != 0
            && Volatile.Read(ref stage.SessionRoutesConverged) == 0)
            return;
        var fence = new ZLinkAggregateFence(
            stage.Envelope.AggregateId,
            stage.Envelope.AggregateGeneration);
        _ = TryCompleteStage(fence, stage, outcome);
    }

    private bool TryCompleteStage(
        ZLinkAggregateFence fence,
        TargetStage stage,
        TargetStageTerminalOutcome outcome)
    {
        var heldRelayDigest = stage.GetHeldRelayDigest();
        var tombstone = new TargetStageTombstone(
            stage.SourceNodeRid,
            stage.StageRequestDigest.ToArray(),
            heldRelayDigest,
            outcome,
            Stopwatch.GetElapsedTime(0) + TombstoneRetention);
        if (!_staged.TryUpdate(fence, tombstone, stage))
            return false;
        _terminalOrder.Enqueue(fence);
        return true;
    }

    private static byte[] ComputeHeldDigest(
        IReadOnlyList<ZLinkSpotRetireHeldRecord> records)
    {
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        Span<byte> number = stackalloc byte[8];
        BinaryPrimitives.WriteUInt64BigEndian(
            number,
            checked((ulong)records.Count));
        hash.AppendData(number);
        foreach (var record in records)
        {
            BinaryPrimitives.WriteUInt64BigEndian(
                number,
                record.AcceptedSequence);
            hash.AppendData(number);
            BinaryPrimitives.WriteUInt64BigEndian(
                number,
                checked((ulong)record.Payload.LongLength));
            hash.AppendData(number);
            hash.AppendData(record.Payload);
        }
        return hash.GetHashAndReset();
    }
}

internal interface ITargetStageEntry
{
    TimeSpan ExpiresAt { get; }
}

internal enum TargetStageAbortState
{
    Staged = 0,
    Aborting = 1,
    Aborted = 2
}

internal enum TargetStageTerminalOutcome
{
    Completed = 0,
    Aborted = 1
}

internal sealed record TargetStageTombstone(
    RoutingId SourceNodeRid,
    byte[] StageRequestDigest,
    byte[]? HeldRelayDigest,
    TargetStageTerminalOutcome Outcome,
    TimeSpan ExpiresAt) : ITargetStageEntry
{
    internal bool Matches(
        ZLinkCanonicalSpotStageContext request,
        RoutingId sourceNodeRid,
        bool requireCompleted) =>
        SourceNodeRid == sourceNodeRid
        && (!requireCompleted
            || Outcome == TargetStageTerminalOutcome.Completed)
        && StageRequestDigest.AsSpan().SequenceEqual(
            ZLinkSpotRetireTargetRuntime.ComputeStageRequestDigest(request));

    internal bool MatchesRelay(
        RoutingId sourceNodeRid,
        ReadOnlySpan<byte> relayDigest) =>
        SourceNodeRid == sourceNodeRid
        && Outcome == TargetStageTerminalOutcome.Completed
        && HeldRelayDigest is { } expected
        && expected.AsSpan().SequenceEqual(relayDigest);
}

internal sealed record TargetStage(
    ZLinkSpotNodeRuntime Node,
    PreparedReservedSpot Spot,
    ZLinkRelocationEnvelope Envelope,
    IReadOnlyList<ZLinkActorRuntimeState> ActorStates,
    string StableType,
    string SourceMeshName,
    RoutingId SourceNodeRid,
    ulong SourceNodeLifecycleGeneration,
    ZLinkLocationOwnerToken SourceOwner,
    string CoordinatorExpectedAuthorityStoreVersion,
    string RelocationReference,
    uint RelocationChecksum,
    TimeSpan ExpiresAt,
    byte[] StageRequestDigest,
    ZLinkSpotRelocationSeal TargetAdmissionSeal,
    IReadOnlyDictionary<ZLinkActorId, ulong>
        ActorTargetAuthorityOwnerGenerations,
    ulong SourceAuthorityOwnerGeneration,
    ulong TargetAuthorityOwnerGeneration,
    string TargetMeshName,
    ulong TargetNodeLifecycleGeneration,
    ulong TargetOwnerLeaseGeneration,
    ulong TargetAttemptGeneration) : ITargetStageEntry
{
    internal IReadOnlyDictionary<ZLinkAuthorityKey, ReadOnlyMemory<byte>>
        SourceRecoveries { get; init; } = Envelope.Participants.ToDictionary(
            static participant => participant.AuthorityKey,
            static participant => participant.RecoveryPayload);

    public int AuthorityPublished;
    public int Published;
    public int AdmissionOpened;
    public int SessionRoutesConverged;
    public Task? AdmissionDrainTask;
    public int ReplayedJobCount;
    public int LocalCatalogPublished;
    public int RelocationReadyDelivered;
    public int RelocatedInitializationCompleted;
    private readonly ZLinkStateLane _lane = new();
    private int _sessionRouteConvergenceRunning;
    private ZLinkRelocationParticipantEnvelope? _spotParticipant;
    public SemaphoreSlim PublishGate { get; } = new(1, 1);

    //  The stage's SPOT participant is fixed once the immutable Envelope is
    //  staged — resolve the single-scan once and reuse the materialized
    //  result instead of re-interpreting the same query per consumer (the
    //  command-31 path re-ran it for every inbound record).
    internal ZLinkRelocationParticipantEnvelope SpotParticipant =>
        _spotParticipant ??= Envelope.Participants.Single(
            static participant => participant.ObjectKind
                is ZLinkPlacementObjectKind.UserSpot
                or ZLinkPlacementObjectKind.InstanceSpot);
    public byte[]? HeldDigest;
    public byte[]? HeldRelayDigest;
    //  Owned by the state lane — append and read through the stage helpers;
    //  use SnapshotHeldRecords for lock-free consumption.
    public List<ZLinkRelocationQueuedJob> HeldRecords = [];

    internal ZLinkRelocationQueuedJob[] SnapshotHeldRecords()
    {
        return AwaitStateLane(_lane.RunAsync(
            () => HeldRecords.ToArray()));
    }
    private uint _relayCrcState = uint.MaxValue;
    private ulong _relayRecordCount;

    private void TrackRelayRecord(ReadOnlySpan<byte> encodedFrozenRecord)
    {
        ZLinkCrc32C.Append(ref _relayCrcState, encodedFrozenRecord);
        _relayRecordCount = checked(_relayRecordCount + 1);
    }

    //  Spec 28 §4.4: on the ordered connection the cutover boundary values
    //  always match the staged relay span; a mismatch is an implementation
    //  defect, not a retryable condition.
    internal void ValidateBoundary(
        ZLinkServiceWireCodec.RelocationCutoverRecord cutover)
    {
        AwaitStateLane(_lane.RunAsync(() =>
        {
            if (cutover.BoundaryRecordCount != _relayRecordCount
                || cutover.BoundaryChecksumCrc32c != ~_relayCrcState)
                throw new InvalidDataException(
                    "Command 34 boundary values do not match the staged relay span.");
        }));
    }
    private string? _finalRootReference;
    private uint _finalRootChecksum;
    private int _abortState;
    private TaskCompletionSource<bool>? _abortCleanup;

    internal TargetStageAbortState AbortState
    {
        get => AwaitStateLane(_lane.RunAsync(
            () => (TargetStageAbortState)_abortState));
        set => AwaitStateLane(_lane.RunAsync(
            () => _abortState = (int)value));
    }

    internal ulong TargetActorAuthorityOwnerGeneration(string actorId) =>
        TargetActorAuthorityOwnerGeneration(
            ZLinkActorId.FromBoundary(actorId, nameof(actorId)));

    internal ulong TargetActorAuthorityOwnerGeneration(ZLinkActorId actorId) =>
        ActorTargetAuthorityOwnerGenerations.TryGetValue(
            actorId,
            out var generation)
            ? generation
            : throw new ZLinkRelocationDataLostException(
                $"Actor '{actorId}' does not have a prepared target authority generation.");

    internal ulong TargetAuthorityOwnerGenerationFor(
        ZLinkRelocationParticipantEnvelope participant)
    {
        if (participant.ObjectKind is ZLinkPlacementObjectKind.UserSpot
            or ZLinkPlacementObjectKind.InstanceSpot)
            return TargetAuthorityOwnerGeneration;
        var actor = ActorTargetAuthorityOwnerGenerations.SingleOrDefault(
            candidate =>
                ZLinkActorAuthorityPayloadCodec.AuthorityKey(candidate.Key.Value)
                == participant.AuthorityKey);
        return actor.Key != default
            ? actor.Value
            : throw new ZLinkRelocationDataLostException(
                $"Actor authority '{participant.AuthorityKey.Value}' does not have a prepared target generation.");
    }

    internal async ValueTask<bool> RunAbortCleanupAsync(
        Func<ValueTask> cleanup,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(cleanup);
        return await RunAbortCleanupAsync(
                async () =>
                {
                    await cleanup().ConfigureAwait(false);
                    return true;
                },
                cancellationToken)
            .ConfigureAwait(false);
    }

    internal async ValueTask<bool> RunAbortCleanupAsync(
        Func<ValueTask<bool>> cleanup,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(cleanup);
        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var preparation = await _lane.RunAsync(PrepareAbortCleanup)
                .ConfigureAwait(false);
            if (preparation.Completed)
                return false;
            if (preparation.Existing is { } existing)
            {
                if (await existing.ConfigureAwait(false))
                    return true;
                continue;
            }

            try
            {
                //  The state transition and placeholder claim happened in turn A.
                //  This callback deliberately runs outside the state lane so it can
                //  re-enter the target runtime without recursive lane admission.
                var removed = await cleanup().ConfigureAwait(false);
                await _lane.RunAsync(
                        () => CompleteAbortCleanup(preparation.Claim!, removed))
                    .ConfigureAwait(false);
                return removed;
            }
            catch (Exception exception)
            {
                await _lane.RunAsync(
                        () => FailAbortCleanup(preparation.Claim!, exception))
                    .ConfigureAwait(false);
                throw;
            }
        }
    }

    internal bool TryBeginSessionRouteConvergence() =>
        Interlocked.CompareExchange(
            ref _sessionRouteConvergenceRunning,
            1,
            0) == 0;

    internal void EndSessionRouteConvergence() =>
        Volatile.Write(ref _sessionRouteConvergenceRunning, 0);

    internal void RememberFinalRoot(string reference, uint checksum)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(reference);
        AwaitStateLane(_lane.RunAsync(() =>
        {
            // Accepted replay and reply ACKs publish successor immutable roots
            // before steady normalization. Every caller validates the
            // relocation/attempt and authority fence before recording the
            // pointer, so cleanup must retain the latest verified root.
            _finalRootReference = reference;
            _finalRootChecksum = checksum;
        }));
    }

    internal (string Reference, uint Checksum)? GetFinalRoot()
    {
        return AwaitStateLane<(string Reference, uint Checksum)?>(
            _lane.RunAsync<(string Reference, uint Checksum)?>(() =>
        {
            return _finalRootReference is null
                ? null
                : (_finalRootReference, _finalRootChecksum);
        }));
    }

    internal bool TrySetHeldRecords(
        IReadOnlyList<ZLinkSpotRetireHeldRecord> records,
        byte[] digest) =>
        AwaitStateLane(_lane.RunAsync(() =>
        {
            if (HeldDigest is { } priorDigest)
                return priorDigest.AsSpan().SequenceEqual(digest);
            HeldDigest = digest;
            HeldRecords = records.Select(
                    static record => new ZLinkRelocationQueuedJob(
                        record.AcceptedSequence,
                        record.Payload.ToArray()))
                .ToList();
            return true;
        }));

    internal byte[]? GetHeldRelayDigest() =>
        AwaitStateLane(_lane.RunAsync(
            () => HeldRelayDigest?.ToArray()));

    internal ValueTask AppendCanonicalInboundDataAsync(
        ZLinkServiceWireCodec.RelocationDataRecord data) =>
        _lane.RunAsync(() =>
        {
            if (Volatile.Read(ref AuthorityPublished) != 0)
                throw new InvalidDataException(
                    "Command 31 arrived after target cutover.");
            var spot = SpotParticipant;
            var previous = HeldRecords.Count == 0
                ? spot.AcceptedJobs
                    .Select(static job => job.AcceptedSequence)
                    .DefaultIfEmpty(0UL)
                    .Max()
                : HeldRecords[^1].AcceptedSequence;
            HeldRecords.Add(
                new ZLinkRelocationQueuedJob(
                    checked(previous + 1),
                    data.FrozenRecord.Encoded.ToArray()));
            TrackRelayRecord(data.FrozenRecord.Encoded.Span);
        });

    private AbortCleanupPreparation PrepareAbortCleanup()
    {
        if ((TargetStageAbortState)_abortState == TargetStageAbortState.Aborted)
            return new AbortCleanupPreparation(null, null, true);
        if (_abortCleanup is { } existing)
            return new AbortCleanupPreparation(null, existing.Task, false);

        var claim = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        _abortState = (int)TargetStageAbortState.Aborting;
        _abortCleanup = claim;
        return new AbortCleanupPreparation(claim, null, false);
    }

    private void CompleteAbortCleanup(TaskCompletionSource<bool> claim, bool removed)
    {
        if (!ReferenceEquals(_abortCleanup, claim))
            throw new InvalidOperationException(
                "Target stage abort cleanup lost its state-lane claim.");
        _abortState = (int)(removed
            ? TargetStageAbortState.Aborted
            : TargetStageAbortState.Staged);
        _abortCleanup = null;
        claim.TrySetResult(removed);
    }

    private void FailAbortCleanup(TaskCompletionSource<bool> claim, Exception exception)
    {
        if (!ReferenceEquals(_abortCleanup, claim))
            throw new InvalidOperationException(
                "Target stage abort cleanup lost its state-lane claim.");
        // Preserve the old retry contract: a failed callback leaves the stage
        // aborting, but releases the placeholder so the next attempt can own it.
        _abortCleanup = null;
        claim.TrySetException(exception);
    }

    private static T AwaitStateLane<T>(ValueTask<T> operation) =>
        operation.GetAwaiter().GetResult();

    private static void AwaitStateLane(ValueTask operation) =>
        operation.GetAwaiter().GetResult();

    private sealed record AbortCleanupPreparation(
        TaskCompletionSource<bool>? Claim,
        Task<bool>? Existing,
        bool Completed);

    internal bool Matches(
        ZLinkCanonicalSpotStageContext request,
        RoutingId sourceNodeRid) =>
        SourceNodeRid == sourceNodeRid
        && StageRequestDigest.AsSpan().SequenceEqual(
            ZLinkSpotRetireTargetRuntime.ComputeStageRequestDigest(request));
}
