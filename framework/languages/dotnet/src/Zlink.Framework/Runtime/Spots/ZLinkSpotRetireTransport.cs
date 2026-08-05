using Microsoft.Extensions.DependencyInjection;
using System.Collections.Concurrent;
using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

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
    ZLinkCanonicalSpotActorDescriptor[] Actors);

internal sealed record ZLinkSpotRetireHeldRecord(
    ulong AcceptedSequence,
    byte[] Payload);

internal sealed class ZLinkCanonicalRelocationDurablyAbortedException(
    string message) : Exception(message);

internal sealed record ZLinkCanonicalHeldIngress(
    Guid AggregateId,
    ulong AggregateGeneration,
    string SpotId,
    ulong ObjectGeneration,
    ulong SourceAuthorityOwnerGeneration,
    ulong TargetAuthorityOwnerGeneration,
    ulong SourceNodeLifecycleGeneration,
    string SourceOwnerId,
    ulong SourceOwnerLeaseGeneration,
    ulong TargetNodeLifecycleGeneration,
    string TargetOwnerId,
    ulong TargetOwnerLeaseGeneration,
    int HopCount,
    ZLinkSpotRetireHeldRecord[] Records);

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
    private const int MaxStagedAggregates = 1_024;
    private const int MaxTerminalTombstones = 1_024;
    internal static readonly TimeSpan StageRetention = TimeSpan.FromHours(24);
    internal static readonly TimeSpan TombstoneRetention = TimeSpan.FromMinutes(5);
    internal static Func<CancellationToken, ValueTask>?
        PostPublicationBeforeNormalizationTestHook;
    private readonly ConcurrentDictionary<
        ZLinkAggregateFence,
        ITargetStageEntry> _staged = new();
    private readonly ConcurrentQueue<ZLinkAggregateFence> _terminalOrder = new();
    private readonly ConcurrentDictionary<ZLinkAggregateFence,
        ZLinkServiceWireCodec.RelocationPrepareRecord> _sourceAttempts = new();
    private readonly SemaphoreSlim _reconciliationWake = new(0, 1);
    private readonly ZLinkStageSlotPool _stageSlots =
        new(MaxStagedAggregates);
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
            .OrderBy(static candidate => candidate.Rid.ToHex(), StringComparer.Ordinal)
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

    internal static bool MatchesTakeoverApplicationVersion(
        long candidateApplicationVersion,
        long requiredApplicationVersion) =>
        candidateApplicationVersion == requiredApplicationVersion;

    public async ValueTask StageAsync(
        ZLinkSpotRetireReservation reservation,
        ZLinkPreparedSpotRetireStaging relocation,
        CancellationToken cancellationToken)
    {
        if (runtime.GetSpotNodeRuntime(reservation.Inventory.SourceNodeRid).Node
                is IZLinkBackendCanonicalRelocationReservation canonical)
        {
            var spot = relocation.Envelope.Participants.Single(
                static participant => participant.ObjectKind
                    is ZLinkPlacementObjectKind.UserSpot
                    or ZLinkPlacementObjectKind.InstanceSpot);
            var authority = relocation.Participants.Single(participant =>
                participant.Envelope.AuthorityKey == spot.AuthorityKey);
            var participants = relocation.Envelope.Participants
                .OrderBy(static participant => participant.CanonicalParticipantId)
                .Select(participant =>
                {
                    if (participant.CanonicalParticipantId == 0)
                        throw new InvalidDataException(
                            "Canonical relocation participant IDs must be assigned before command 40.");
                    ulong bytes = 0;
                    foreach (var job in participant.AcceptedJobs)
                        bytes = checked(bytes + (ulong)job.Payload.Length);
                    return new ZLinkServiceWireCodec.RelocationParticipantRecord(
                        participant.CanonicalParticipantId, 1, default, 0, null,
                        0, default, 0,
                        checked((ulong)participant.AcceptedJobs.Count), bytes);
                }).ToArray();
            var requiredMessages = participants.Aggregate(0UL,
                static (sum, participant) => checked(sum
                    + participant.AllowanceMessages));
            var requiredBytes = checked((ulong)
                ZLinkRelocationEnvelopeCodec.MeasureEncodedLength(
                    relocation.Envelope));
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
            var prepare = new ZLinkServiceWireCodec.RelocationPrepareRecord(
                relocationId,
                targetAttemptGeneration,
                1,
                new ZLinkServiceWireCodec.RelocationCoordinatorFence(
                    reservation.Inventory.SourceOwner.OwnerId,
                    checked((ulong)reservation.Inventory.SourceOwner
                        .LeaseGeneration),
                    reservation.Inventory.SourceNodeRid,
                    reservation.Inventory.SourceNodeLifecycleGeneration,
                    authority.ExpectedStoreVersion),
                new ZLinkServiceWireCodec.RelocationCandidateRecord(
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
                requiredMessages,
                requiredBytes,
                participants,
                new ZLinkServiceWireCodec.RelocationRootRecord(
                    relocation.Relocation.Reference,
                    relocation.Relocation.ChecksumCrc32c),
                checked((ulong)registration.ApplicationVersion));
            var data = relocation.Envelope.Participants
                .OrderBy(static participant =>
                    participant.CanonicalParticipantId)
                .SelectMany(participant => participant.AcceptedJobs
                    .OrderBy(static job => job.AcceptedSequence)
                    .Select((job, index) =>
                        new ZLinkServiceWireCodec.RelocationDataRecord(
                            relocationId,
                            targetAttemptGeneration,
                            prepare.Coordinator,
                            1,
                            participant.CanonicalParticipantId,
                            checked((ulong)index + 1),
                            new ZLinkServiceWireCodec.FrozenRecord(
                                job.Payload))))
                .ToArray();
            await canonical.StageCanonicalRelocationAsync(
                    reservation.TargetDescriptor.Rid,
                    prepare,
                    data,
                    Timeout.InfiniteTimeSpan,
                    cancellationToken)
                .ConfigureAwait(false);
            _sourceAttempts[new ZLinkAggregateFence(
                relocation.Envelope.AggregateId,
                relocation.Envelope.AggregateGeneration)] = prepare;
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
                reservation, relocation, cancellationToken)
            .ConfigureAwait(false);
    }

    public async ValueTask AbortAsync(
        ZLinkSpotRetireReservation reservation,
        ZLinkAggregateFence? fence)
    {
        _ = reservation;
        if (fence is { } exactFence
            && _sourceAttempts.TryGetValue(exactFence, out var prepare)
            && runtime.GetSpotNodeRuntime(
                    reservation.Inventory.SourceNodeRid).Node
                is IZLinkBackendCanonicalRelocationReservation canonical)
        {
            canonical.CancelCanonicalRelocation(
                reservation.TargetDescriptor.Rid,
                prepare.RelocationId,
                prepare.TargetAttemptGeneration,
                prepare.Coordinator);
            _sourceAttempts.TryRemove(
                new KeyValuePair<
                    ZLinkAggregateFence,
                    ZLinkServiceWireCodec.RelocationPrepareRecord>(
                    exactFence,
                    prepare));
        }
        // There is no invented wire abort. The canonical reservation owner
        // releases uncommitted aggregate/capacity fences on its bounded expiry.
        await ValueTask.CompletedTask;
    }

    private async ValueTask<ulong> ReconcilePublishedAuthorityAsync(
        ZLinkSpotRetireReservation reservation,
        ZLinkAggregateRelocationPublished relocation,
        CancellationToken cancellationToken)
    {
        var store = registration.Locations.ResolveStore()
            ?? throw new ZLinkConfigurationException(
                "Location Store is not registered.");
        var deadline = DateTimeOffset.UtcNow
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
                && ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                    found.Snapshot.Payload.Span, out var publication)
                && publication.AggregateId == relocation.Fence.AggregateId
                && publication.AggregateGeneration
                   == relocation.Fence.AggregateGeneration
                && publication.Reference == relocation.Relocation.Reference
                && publication.ChecksumCrc32c
                   == relocation.Relocation.ChecksumCrc32c)
                return found.Snapshot.AuthorityOwnerGeneration;
            if (DateTimeOffset.UtcNow >= deadline)
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

    private async ValueTask CompleteCanonicalSourceAsync(
        ZLinkSpotRetireReservation reservation,
        ZLinkAggregateRelocationPublished relocation,
        IReadOnlyList<ZLinkAcceptedWorkRecord> held,
        CancellationToken cancellationToken)
    {
        await runtime.RelayCommittedSpotRecordsAsync(
                reservation, relocation, held, cancellationToken)
            .ConfigureAwait(false);
        if (!_sourceAttempts.TryGetValue(relocation.Fence, out var prepare))
            return;
        if (runtime.GetSpotNodeRuntime(reservation.Inventory.SourceNodeRid).Node
            is not IZLinkBackendCanonicalRelocationReservation canonical)
            throw new ZLinkConfigurationException(
                "The MeshNode backend does not support canonical relocation completion.");
        await canonical.CompleteCanonicalRelocationAsync(
                reservation.TargetDescriptor.Rid,
                new ZLinkServiceWireCodec.RelocationCompleteRecord(
                    prepare.RelocationId,
                    prepare.TargetAttemptGeneration,
                    prepare.Coordinator,
                    1,
                    new ZLinkServiceWireCodec.RequestSourceFence(
                        prepare.Coordinator.OwnerId,
                        prepare.Coordinator.LeaseGeneration,
                        prepare.SourceNodeRid,
                        prepare.SourceNodeGeneration),
                    1),
                cancellationToken)
            .ConfigureAwait(false);
        _sourceAttempts.TryRemove(
            new KeyValuePair<
                ZLinkAggregateFence,
                ZLinkServiceWireCodec.RelocationPrepareRecord>(
                relocation.Fence,
                prepare));
    }

    internal async ValueTask RecoverPublishedAsync(
        ZLinkRelocationRecoveryCandidate candidate,
        CancellationToken cancellationToken)
    {
        candidate = BindRecoveredCanonicalInventory(candidate);
        var replacement = await TryTakeOverFailedTargetAsync(
                candidate,
                cancellationToken)
            .ConfigureAwait(false);
        if (replacement is not null)
            candidate = BindRecoveredCanonicalInventory(replacement);
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
            && (candidate.Envelope.AggregateGeneration
                == stage.Envelope.AggregateGeneration
                || candidate.Envelope.AggregateGeneration
                   == checked(stage.Envelope.AggregateGeneration + 1)));
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

        var encodedLength = ZLinkRelocationEnvelopeCodec.MeasureEncodedLength(
            candidate.Envelope);
        if (!_stageSlots.TryAcquire(out var activeSlot))
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"Inbound recovery staging is full for SPOT '{spotId}'.",
                ZLinkRetryAdvice.RetryAfterBackoff);
        if (!runtime.TryAcquireInboundSpotRelocation(
                encodedLength,
                RequiresRestore(recoveryNode, request),
                allowOversizedPayload: !request.InstanceSpot,
                out var permit))
        {
            activeSlot.Dispose();
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"Inbound recovery permit is unavailable for SPOT '{spotId}'.",
                ZLinkRetryAdvice.RetryAfterBackoff);
        }
        TargetStage stage;
        try
        {
            stage = await runtime.StageInboundSpotAggregateAsync(
                    request,
                    candidate.Envelope,
                    permit,
                    null,
                    reservedTargetAuthorityOwnerGeneration: 0,
                    cancellationToken)
                .ConfigureAwait(false);
            stage.AttachActiveSlot(activeSlot);
        }
        catch
        {
            permit.Dispose();
            activeSlot.Dispose();
            throw;
        }
        if (!TryTrackStage(fence, stage))
        {
            await AbortTargetStageAsync(stage)
                .ConfigureAwait(false);
            ReleaseStageResources(stage);
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

    private async ValueTask<ZLinkRelocationRecoveryCandidate?>
        TryTakeOverFailedTargetAsync(
            ZLinkRelocationRecoveryCandidate candidate,
            CancellationToken cancellationToken)
    {
        var projections = candidate.Authorities.Select(entry =>
        {
            if (!ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                    entry.Snapshot.Payload.Span,
                    out var publication)
                || !publication.IsCanonical)
                throw new ZLinkRelocationDataLostException(
                    $"Relocation authority '{entry.Key.Value}' has no canonical publication.");
            if (!ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                    entry.Snapshot.Payload.Span,
                    out var projection))
                throw new ZLinkRelocationDataLostException(
                    $"Relocation authority '{entry.Key.Value}' has no canonical target fence.");
            return (Entry: entry, Projection: projection,
                Publication: publication);
        }).ToArray();
        var shared = projections[0].Projection;
        var currentAggregateGeneration =
            projections[0].Publication.AggregateGeneration;
        if (projections.Any(item =>
                item.Projection.RelocationHigh != shared.RelocationHigh
                || item.Projection.RelocationLow != shared.RelocationLow
                || item.Publication.AggregateGeneration
                   != currentAggregateGeneration
                || item.Projection.TargetAttemptGeneration
                   != shared.TargetAttemptGeneration
                || !StringComparer.Ordinal.Equals(
                    item.Projection.State.TargetNodeRid,
                    shared.State.TargetNodeRid)
                || item.Projection.State.TargetNodeGeneration
                   != shared.State.TargetNodeGeneration
                || !StringComparer.Ordinal.Equals(
                    item.Projection.TargetOwnerId,
                    shared.TargetOwnerId)
                || item.Projection.TargetOwnerLeaseGeneration
                   != shared.TargetOwnerLeaseGeneration
                || !HasExactTargetOwnedSnapshot(
                    item.Entry.Snapshot,
                    item.Projection)))
            throw new ZLinkRelocationDataLostException(
                "Canonical relocation authorities disagree on the current target fence.");
        if (currentAggregateGeneration == 0)
        {
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"aggregate_target_takeover_unknown_generation relocation={candidate.Envelope.AggregateId:N}");
            return null;
        }

        var spotAuthority = projections.Single(item =>
            item.Entry.Snapshot.Allocation.ObjectKind
            is ZLinkPlacementObjectKind.UserSpot
                or ZLinkPlacementObjectKind.InstanceSpot);
        var meshName = spotAuthority.Entry.Snapshot.Allocation.Descriptor.MeshName;
        var resolver = services.GetService<IZLinkMeshNodeLocationResolver>();
        if (resolver is null)
            return null;
        var live = await resolver.ListLiveMeshNodesAsync(
                meshName,
                cancellationToken)
            .ConfigureAwait(false);
        if (live.Any(descriptor =>
                StringComparer.Ordinal.Equals(
                    descriptor.Rid.ToHex(),
                    shared.State.TargetNodeRid)
                && descriptor.LifecycleGeneration
                   == shared.State.TargetNodeGeneration
                && StringComparer.Ordinal.Equals(
                    descriptor.OwnerId,
                    shared.TargetOwnerId)
                && checked((ulong)descriptor.LeaseGeneration)
                   == shared.TargetOwnerLeaseGeneration))
        {
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"aggregate_target_takeover_wait relocation={candidate.Envelope.AggregateId:N}"
                + $" target={shared.State.TargetNodeRid}");
            return null;
        }

        var localCandidates = live
            .Where(descriptor =>
                runtime.TryGetSpotNodeRuntime(descriptor.Rid) is not null
                && descriptor.State is ZLinkFrameworkRuntimeState.Preparing
                    or ZLinkFrameworkRuntimeState.Serving
                && descriptor.ObjectRole == ZLinkMeshNodeObjectRole.Server
                && descriptor.PlacementWeight > 0
                && descriptor.LeaseGeneration > 0
                && MatchesTakeoverApplicationVersion(
                    descriptor.ApplicationVersion,
                    candidate.Envelope.CanonicalApplicationVersion)
                && !StringComparer.Ordinal.Equals(
                    descriptor.Rid.ToHex(),
                    shared.State.TargetNodeRid)
                && SupportsRecoveredInventory(
                    descriptor,
                    candidate.Envelope))
            .OrderByDescending(static descriptor => descriptor.PlacementWeight)
            .ThenBy(static descriptor => descriptor.Rid.ToHex(),
                StringComparer.Ordinal)
            .ToArray();
        if (localCandidates.Length == 0)
        {
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"aggregate_target_takeover_no_candidate relocation={candidate.Envelope.AggregateId:N}"
                + $" live={live.Count} candidates="
                + string.Join(';', live.Select(descriptor =>
                    $"{descriptor.Rid.ToHex()}"
                    + $":local={runtime.TryGetSpotNodeRuntime(descriptor.Rid) is not null}"
                    + $":state={descriptor.State}"
                    + $":role={descriptor.ObjectRole}"
                    + $":weight={descriptor.PlacementWeight}"
                    + $":lease={descriptor.LeaseGeneration}"
                    + $":app={descriptor.ApplicationVersion}"
                    + $":inventory={SupportsRecoveredInventory(descriptor, candidate.Envelope)}")));
            return null;
        }

        var store = registration.Locations.ResolveStore()
                    ?? throw new ZLinkConfigurationException(
                        "Location Store is not registered.");
        var relocationStore =
            registration.Locations.ResolveRelocationStore()
            ?? throw new ZLinkConfigurationException(
                "Relocation Store is not registered.");
        foreach (var replacement in localCandidates)
        {
            var committed = await TryCommitTakeoverPublicationAsync(
                    store,
                    relocationStore,
                    candidate,
                    replacement,
                    StageRetention,
                    cancellationToken)
                .ConfigureAwait(false);
            if (committed is not null)
                return committed;
        }
        return null;
    }

    internal static async ValueTask<ZLinkRelocationRecoveryCandidate?>
        TryCommitTakeoverPublicationAsync(
            IZLinkLocationRepository store,
            IZLinkRelocationRepository relocationStore,
            ZLinkRelocationRecoveryCandidate candidate,
            ZLinkMeshNodeDescriptor replacement,
            TimeSpan retention,
            CancellationToken cancellationToken)
    {
        var projections = candidate.Authorities.Select(entry =>
        {
            if (!ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                    entry.Snapshot.Payload.Span,
                    out var projection)
                || !ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                    entry.Snapshot.Payload.Span,
                    out var publication))
                throw new ZLinkRelocationDataLostException(
                    "Canonical relocation takeover publication is invalid.");
            return (Entry: entry, Projection: projection,
                Publication: publication);
        }).ToArray();
        var shared = projections[0];
        var (nextAttempt, nextGeneration) = NextTakeoverFence(
            shared.Projection.TargetAttemptGeneration,
            shared.Publication.AggregateGeneration);
        var envelope = candidate.Envelope with
        {
            AggregateGeneration = nextGeneration
        };
        var root = await ZLinkRelocationTreeStore.PutAsync(
                relocationStore,
                envelope,
                retention,
                cancellationToken)
            .ConfigureAwait(false);
        var owner = new ZLinkLocationOwnerToken(
            replacement.OwnerId,
            replacement.LeaseGeneration);
        var ownerGeneration = checked((ulong)replacement.LeaseGeneration);
        var mutations = projections.Select(item =>
        {
            var state = item.Projection.State with
            {
                TargetAttemptGeneration = nextAttempt,
                TargetNodeRid = replacement.Rid.ToHex(),
                TargetNodeGeneration = replacement.LifecycleGeneration,
                TargetOwnerId = replacement.OwnerId,
                TargetOwnerLeaseGeneration = ownerGeneration,
                ReservationGeneration = nextAttempt,
                CoordinatorOwnerId = replacement.OwnerId,
                CoordinatorLeaseGeneration = ownerGeneration,
                CoordinatorNodeRid = replacement.Rid.ToHex(),
                CoordinatorNodeGeneration = replacement.LifecycleGeneration,
                RelocationReference = root.Root.Reference,
                RelocationChecksumCrc32c = root.Root.ChecksumCrc32c,
                AggregateGeneration = nextGeneration
            };
            return new ZLinkAggregateParticipant(
                item.Entry.Key,
                item.Entry.Snapshot.StoreVersion,
                ZLinkAuthorityGenerationTransition.NewOwner,
                ZLinkCanonicalRelocationAuthorityStateCodec
                    .ReplaceRelocationState(
                        item.Entry.Snapshot.Payload.Span,
                        state,
                        envelope),
                ReadOnlyMemory<byte>.Empty);
        }).ToArray();
        var spot = candidate.Authorities.Single(entry =>
            entry.Snapshot.Allocation.ObjectKind
            is ZLinkPlacementObjectKind.UserSpot
                or ZLinkPlacementObjectKind.InstanceSpot);
        var prepare = await store.PrepareAggregateAsync(
                new ZLinkAggregatePrepareRequest(
                    candidate.Envelope.AggregateId,
                    nextGeneration,
                    mutations,
                    candidate.Envelope.InventoryDigest,
                    new ZLinkMeshNodeDescriptorKey(
                        replacement.MeshName,
                        replacement.Rid),
                    replacement.LifecycleGeneration,
                    new ZLinkCapacityVector(
                        candidate.Envelope.Participants.Count(
                            static participant =>
                                participant.ObjectKind
                                == ZLinkPlacementObjectKind.Actor),
                        1,
                        new ZLinkSpotTypeCapacityDelta(
                            spot.Snapshot.Allocation.ObjectKind,
                            spot.Snapshot.Allocation.StableType,
                            1)),
                    owner,
                    AllowPreparingTarget: true),
                cancellationToken)
            .ConfigureAwait(false);
        var ownsFence = prepare is ZLinkAggregatePrepareResult.Prepared;
        var fence = prepare switch
        {
            ZLinkAggregatePrepareResult.Prepared value => value.Fence,
            ZLinkAggregatePrepareResult.AlreadyPrepared value => value.Fence,
            ZLinkAggregatePrepareResult.GenerationExhausted =>
                throw new ZLinkAuthorityGenerationExhaustedException(
                    "preparing a replacement SPOT relocation target"),
            _ => default
        };
        if (fence == default)
            return null;
        var commit = await CommitTakeoverFenceAsync(
                store,
                fence,
                ownsFence,
                cancellationToken)
            .ConfigureAwait(false);
        if (commit == ZLinkAggregateCommitResult.GenerationExhausted)
            throw new ZLinkAuthorityGenerationExhaustedException(
                "committing a replacement SPOT relocation target");
        if (commit is not (
                ZLinkAggregateCommitResult.Committed
                or ZLinkAggregateCommitResult.AlreadyCommitted))
            return null;
        return await new ZLinkRelocationStartupRecovery(
                store,
                relocationStore)
            .TryReadExactPublishedAsync(
                envelope,
                cancellationToken)
            .ConfigureAwait(false);
    }

    internal static bool HasExactTargetOwnedSnapshot(
        ZLinkAuthoritySnapshot snapshot,
        ZLinkCanonicalRelocationAuthorityProjection projection) =>
        projection.Phase is not (>= 4 and <= 8)
        || StringComparer.Ordinal.Equals(
            snapshot.Allocation.Descriptor.Rid.ToHex(),
            projection.State.TargetNodeRid)
        && snapshot.Allocation.DescriptorLifecycleGeneration
           == projection.State.TargetNodeGeneration
        && StringComparer.Ordinal.Equals(
            snapshot.OwnerId,
            projection.TargetOwnerId)
        && snapshot.OwnerLeaseGeneration > 0
        && checked((ulong)snapshot.OwnerLeaseGeneration)
           == projection.TargetOwnerLeaseGeneration;

    internal static (ulong TargetAttemptGeneration, ulong AggregateGeneration)
        NextTakeoverFence(
            ulong currentTargetAttemptGeneration,
            ulong currentAggregateGeneration)
    {
        if (currentTargetAttemptGeneration >= long.MaxValue
            || currentAggregateGeneration >= long.MaxValue)
            throw new ZLinkAuthorityGenerationExhaustedException(
                "replacing a failed SPOT relocation target");
        return (
            checked(currentTargetAttemptGeneration + 1),
            checked(currentAggregateGeneration + 1));
    }

    internal static async ValueTask<ZLinkAggregateCommitResult>
        CommitTakeoverFenceAsync(
            IZLinkLocationRepository store,
            ZLinkAggregateFence fence,
            bool ownsPreparedFence,
            CancellationToken cancellationToken)
    {
        ZLinkAggregateCommitResult commit;
        try
        {
            commit = await store.CommitAggregateForRecoveryAsync(
                    fence,
                    cancellationToken)
                .ConfigureAwait(false);
        }
        catch
        {
            if (ownsPreparedFence)
                await AbortOwnedTakeoverAsync(store, fence)
                    .ConfigureAwait(false);
            throw;
        }
        if (ownsPreparedFence
            && commit is not (
                ZLinkAggregateCommitResult.Committed
                or ZLinkAggregateCommitResult.AlreadyCommitted))
            await AbortOwnedTakeoverAsync(store, fence).ConfigureAwait(false);
        return commit;
    }

    private static async ValueTask AbortOwnedTakeoverAsync(
        IZLinkLocationRepository store,
        ZLinkAggregateFence fence)
    {
        try
        {
            _ = await store.AbortAggregateAsync(
                    fence,
                    CancellationToken.None)
                .ConfigureAwait(false);
        }
        catch
        {
            // The original commit outcome remains authoritative. A later
            // recovery scan reconciles or aborts the exact same fence.
        }
    }

    private static bool SupportsRecoveredInventory(
        ZLinkMeshNodeDescriptor descriptor,
        ZLinkRelocationEnvelope envelope) =>
        envelope.Participants.All(participant =>
        {
            var recovery = ZLinkCanonicalParticipantRecoveryCodec.Decode(
                participant.RecoveryPayload.Span);
            return descriptor.ObjectCapabilities.Any(capability =>
                capability.ObjectKind == participant.ObjectKind
                && StringComparer.Ordinal.Equals(
                    capability.StableType,
                    recovery.StableType)
                && capability.Policy == recovery.MaintenancePolicy
                && (recovery.MaintenancePolicy
                    != ZLinkObjectMaintenancePolicyKind.Snapshot
                    || capability.HasSnapshotAdapter));
        });

    private static ZLinkRelocationRecoveryCandidate BindRecoveredCanonicalInventory(
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
        byte cleanupState = 0;
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
            if (publication.AggregateGeneration != aggregateGeneration
                || index != 0 && publication.SourceCleanupState != cleanupState)
                throw new ZLinkRelocationDataLostException(
                    "Canonical relocation authorities disagree on progress.");
            cleanupState = publication.SourceCleanupState;
            return state with
            {
                AuthorityKey = authority.Key,
                ObjectKind = authority.Snapshot.Allocation.ObjectKind,
                ObjectGeneration = authority.Snapshot.ObjectGeneration,
                AuthorityOwnerGeneration = authority.Snapshot.AuthorityOwnerGeneration,
                CompletionPayload = index == 0
                    ? cleanupState == 0
                        ? ZLinkSpotRetireCompletionMarker.CreatePending()
                        : ZLinkSpotRetireCompletionMarker.CreateCompleted()
                    : ReadOnlyMemory<byte>.Empty
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

    private async ValueTask<bool> TryPromotePendingRecoveryAsync(
        ZLinkRelocationRecoveryCandidate candidate,
        TargetStage stage,
        ZLinkAuthoritySnapshot spotAuthority,
        string meshName,
        RoutingId sourceNodeRid,
        ulong sourceNodeGeneration,
        string sourceOwnerId,
        ulong sourceOwnerLeaseGeneration,
        CancellationToken cancellationToken)
    {
        var spot = candidate.Envelope.Participants.Single(
            static participant => participant.ObjectKind
                is ZLinkPlacementObjectKind.UserSpot
                or ZLinkPlacementObjectKind.InstanceSpot);
        if (!ZLinkSpotRetireCompletionMarker.IsPending(
                spot.CompletionPayload.Span))
            throw new ZLinkRelocationDataLostException(
                "Relocation recovery root has an unknown source-cleanup phase.");
        cancellationToken.ThrowIfCancellationRequested();

        // Replay is completed while target admission remains sealed. It is
        // safe to publish Completed only after the exact source lifecycle can
        // no longer execute its queue.
        await runtime.PrepareInboundSpotAggregateAsync(
                stage,
                cancellationToken)
            .ConfigureAwait(false);
        if (!await IsSourceLifecycleFencedAsync(
                meshName,
                sourceNodeRid,
                sourceNodeGeneration,
                sourceOwnerId,
                sourceOwnerLeaseGeneration,
                cancellationToken).ConfigureAwait(false))
            return false;

        var targetOwner = new ZLinkLocationOwnerToken(
            spotAuthority.OwnerId,
            checked((ulong)spotAuthority.OwnerLeaseGeneration));
        if (!await new ZLinkAggregateRelocationCoordinator(
                registration.Locations.ResolveStore()
                ?? throw new ZLinkConfigurationException(
                    "Location Store is not registered."),
                registration.Locations.ResolveRelocationStore()
                ?? throw new ZLinkConfigurationException(
                    "Relocation Store is not registered."))
            .TryCompleteSourceCleanupAsync(
                new ZLinkAggregateRelocationPublished(
                    new ZLinkAggregateFence(
                        candidate.Envelope.AggregateId,
                        candidate.Envelope.AggregateGeneration),
                    new ZLinkRelocationStored(
                        candidate.Reference.Reference,
                        candidate.Reference.ChecksumCrc32c,
                        default,
                        spotAuthority.StoreNow),
                    candidate.Envelope),
                spotAuthority.Allocation.Descriptor,
                spotAuthority.Allocation.DescriptorLifecycleGeneration,
                targetOwner,
                cancellationToken)
            .ConfigureAwait(false))
            return false;
        return true;
    }

    private async ValueTask<bool> IsSourceLifecycleFencedAsync(
        string meshName,
        RoutingId sourceNodeRid,
        ulong sourceNodeGeneration,
        string sourceOwnerId,
        ulong sourceOwnerLeaseGeneration,
        CancellationToken cancellationToken)
    {
        var resolver = services.GetService<IZLinkMeshNodeLocationResolver>();
        if (resolver is null)
            return false;
        var live = await resolver.ListLiveMeshNodesAsync(
                meshName,
                cancellationToken)
            .ConfigureAwait(false);
        return !IsExactSourceLifecycleStillLive(
            live,
            sourceNodeRid,
            sourceNodeGeneration,
            sourceOwnerId,
            sourceOwnerLeaseGeneration);
    }

    internal static bool IsExactSourceLifecycleStillLive(
        IReadOnlyList<ZLinkMeshNodeDescriptor> live,
        RoutingId sourceNodeRid,
        ulong sourceNodeGeneration,
        string sourceOwnerId,
        ulong sourceOwnerLeaseGeneration) =>
        live.Any(descriptor =>
            descriptor.Rid == sourceNodeRid
            && descriptor.LifecycleGeneration == sourceNodeGeneration
            && descriptor.OwnerId == sourceOwnerId
            && descriptor.LeaseGeneration > 0
            && checked((ulong)descriptor.LeaseGeneration)
               == sourceOwnerLeaseGeneration);

    internal async ValueTask ReconcilePublishedStageAsync(
        TargetStage stage,
        ZLinkRelocationRecoveryCandidate candidate,
        CancellationToken cancellationToken)
    {
        // Both startup recovery and an in-process exact publication retry
        // must derive canonical source-cleanup progress from authority state.
        candidate = BindRecoveredCanonicalInventory(candidate);
        if (!IsStagingPrefix(
                stage.Envelope,
                candidate.Envelope,
                stage.TargetAuthorityOwnerGenerationFor))
            throw new ZLinkRelocationDataLostException(
                "Published SPOT relocation root does not extend its staged root.");
        RestoreHeldRecords(stage, candidate.Envelope);
        Volatile.Write(ref stage.AuthorityPublished, 1);

        var spotAuthority = candidate.Authorities.Single(
            static entry => entry.Snapshot.Allocation.ObjectKind
                is ZLinkPlacementObjectKind.UserSpot
                or ZLinkPlacementObjectKind.InstanceSpot);
        var durableSpot = candidate.Envelope.Participants.Single(
            static participant => participant.ObjectKind
                is ZLinkPlacementObjectKind.UserSpot
                or ZLinkPlacementObjectKind.InstanceSpot);
        var completionFinalized =
            ZLinkSpotRetireCompletionMarker.IsCompleted(
                durableSpot.CompletionPayload.Span);
        if (completionFinalized)
            stage.RememberFinalRoot(
                candidate.Reference.Reference,
                candidate.Reference.ChecksumCrc32c);
        if (!completionFinalized)
            completionFinalized = await TryPromotePendingRecoveryAsync(
                    candidate,
                    stage,
                    spotAuthority.Snapshot,
                    stage.SourceMeshName,
                    stage.SourceNodeRid,
                    stage.SourceNodeLifecycleGeneration,
                    stage.SourceOwner.OwnerId,
                    checked((ulong)stage.SourceOwner.LeaseGeneration),
                    cancellationToken)
                .ConfigureAwait(false);
        if (!completionFinalized)
        {
            // Standalone Instance SPOT authority is published by the
            // reservation owner. Materialize the staged target immediately,
            // while keeping admission sealed until command 35 confirms source
            // cleanup and normalizes the authority.
            await FinalizeStageAsync(
                    stage,
                    normalizeAuthority: false,
                    cancellationToken)
                .ConfigureAwait(false);
            return;
        }
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
        var stagedSpot = stage.Envelope.Participants.Single(
            static participant => participant.ObjectKind
                is ZLinkPlacementObjectKind.UserSpot
                or ZLinkPlacementObjectKind.InstanceSpot);
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
        lock (stage.HeldGate)
        {
            if (stage.HeldDigest is { } priorDigest)
                return priorDigest.AsSpan().SequenceEqual(digest);
            stage.HeldDigest = digest;
            stage.HeldRecords = records.Select(
                    static record => new ZLinkRelocationQueuedJob(
                        record.AcceptedSequence,
                        record.Payload.ToArray()))
                .ToArray();
            stage.HeldHighWater = records.Count == 0
                ? 0
                : records[^1].AcceptedSequence;
            return true;
        }
    }

    internal async ValueTask<bool> ApplyCanonicalHeldIngressAsync(
        ZLinkCanonicalHeldIngress message,
        RoutingId sourceNodeRid,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var relayDigest = SHA256.HashData(
            JsonSerializer.SerializeToUtf8Bytes(message));
        var fence = new ZLinkAggregateFence(
            message.AggregateId,
            message.AggregateGeneration);
        await CleanupExpiredAsync().ConfigureAwait(false);
        if (!_staged.TryGetValue(fence, out var entry))
            return false;
        if (entry is TargetStageTombstone terminal)
            return terminal.MatchesRelay(sourceNodeRid, relayDigest);
        if (entry is not TargetStage stage
            || stage.AbortState != TargetStageAbortState.Staged
            || Volatile.Read(ref stage.AuthorityPublished) == 0
            || stage.SourceNodeRid != sourceNodeRid
            || stage.Spot.Activation.SpotId != message.SpotId
            || stage.Spot.Activation.ObjectGeneration
               != message.ObjectGeneration
            || stage.SourceAuthorityOwnerGeneration
               != message.SourceAuthorityOwnerGeneration
            || stage.TargetAuthorityOwnerGeneration
               != message.TargetAuthorityOwnerGeneration
            || stage.SourceNodeLifecycleGeneration
               != message.SourceNodeLifecycleGeneration
            || stage.SourceOwner.OwnerId != message.SourceOwnerId
            || stage.SourceOwner.LeaseGeneration <= 0
            || checked((ulong)stage.SourceOwner.LeaseGeneration)
               != message.SourceOwnerLeaseGeneration
            || stage.TargetNodeLifecycleGeneration
               != message.TargetNodeLifecycleGeneration
            || stage.TargetOwnerLeaseGeneration
               != message.TargetOwnerLeaseGeneration
            || runtime.LocationLifecycle?.OwnerToken is not { } targetOwner
            || targetOwner.OwnerId != message.TargetOwnerId
            || targetOwner.LeaseGeneration <= 0
            || checked((ulong)targetOwner.LeaseGeneration)
               != message.TargetOwnerLeaseGeneration
            || message.SourceAuthorityOwnerGeneration
               is 0 or > long.MaxValue
            || message.TargetAuthorityOwnerGeneration
               is 0 or > long.MaxValue
            || message.TargetAuthorityOwnerGeneration
               <= message.SourceAuthorityOwnerGeneration
            || message.HopCount is < 1 or > 8)
            return false;

        ValidateHeldRecords(message.Records);
        lock (stage.HeldGate)
        {
            if (stage.HeldRelayDigest is { } prior
                && !prior.AsSpan().SequenceEqual(relayDigest))
                return false;
            stage.HeldRelayDigest ??= relayDigest;
        }
        if (!TrySetHeldRecords(stage, message.Records))
            return false;
        await FinalizeStageAsync(
                stage,
                normalizeAuthority: false,
                cancellationToken)
            .ConfigureAwait(false);
        await CompleteInboundThenNormalizeAsync(stage, cancellationToken)
            .ConfigureAwait(false);
        return true;
    }

    internal static void ValidateHeldRecords(
        IReadOnlyList<ZLinkSpotRetireHeldRecord> records)
    {
        if (records.Count > 1_024)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Rejected,
                "A Spot relocation held-ingress queue cannot exceed 1,024 messages.");
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
            if (bytes > 16L * 1024 * 1024)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Rejected,
                    "A Spot relocation held-ingress queue cannot exceed 16 MiB.");
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
        RoutingId sourceNodeRid,
        CancellationToken cancellationToken)
    {
        if (prepare.Object.Kind == 1)
            throw new InvalidOperationException(
                "Standalone Actor relocation must be handled by the Actor maintenance owner.");
        var root = prepare.Root
            ?? throw new InvalidDataException(
                "Canonical SPOT relocation requires an immutable root.");
        var relocationStore = registration.Locations.ResolveRelocationStore()
            ?? throw new ZLinkConfigurationException(
                "Relocation Store is not registered.");
        var tree = await ZLinkRelocationTreeStore.ReadAsync(
                relocationStore, root.Reference, root.ChecksumCrc32c,
                cancellationToken)
            .ConfigureAwait(false);
        var recoveries = tree.Envelope.Participants
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
        var spotRecovery = recoveries.Single(static recovery =>
            recovery.ObjectKind is ZLinkPlacementObjectKind.UserSpot
                or ZLinkPlacementObjectKind.InstanceSpot);
        var context = new ZLinkCanonicalSpotStageContext(
            tree.Envelope.AggregateId,
            tree.Envelope.AggregateGeneration,
            prepare.TargetAttemptGeneration,
            runtime.GetSpotNodeRuntime(
                    prepare.Candidate.NodeRid)
                .Node.MeshStatus().MeshName,
            prepare.SourceNodeRid.ToHex(),
            prepare.SourceNodeGeneration,
            prepare.Coordinator.OwnerId,
            prepare.Coordinator.LeaseGeneration,
            prepare.Candidate.NodeRid.ToHex(),
            prepare.Candidate.NodeGeneration,
            prepare.Candidate.OwnerId,
            prepare.Candidate.OwnerLeaseGeneration,
            prepare.Object.ObjectId,
            spotRecovery.StableType,
            prepare.Object.Kind == 3,
            root.Reference,
            root.ChecksumCrc32c,
            actors);
        var staged = await StageInboundAsync(
                context,
                sourceNodeRid,
                cancellationToken)
            .ConfigureAwait(false);
        if (!staged)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                "Canonical target staging was rejected.", ZLinkRetryAdvice.RetryAfterBackoff);
    }

    internal async ValueTask PublishCanonicalInboundAsync(
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
        RoutingId sourceNodeRid,
        CancellationToken cancellationToken)
    {
        var root = prepare.Root
            ?? throw new InvalidDataException(
                "Canonical SPOT relocation requires an immutable root.");
        var relocationStore = registration.Locations.ResolveRelocationStore()
            ?? throw new ZLinkConfigurationException(
                "Relocation Store is not registered.");
        var tree = await ZLinkRelocationTreeStore.ReadAsync(
                relocationStore, root.Reference, root.ChecksumCrc32c,
                cancellationToken)
            .ConfigureAwait(false);
        if (!await PublishInboundAsync(
                new ZLinkAggregateFence(tree.Envelope.AggregateId,
                    tree.Envelope.AggregateGeneration),
                sourceNodeRid, cancellationToken).ConfigureAwait(false))
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                "Canonical target publication was rejected.", ZLinkRetryAdvice.RetryAfterBackoff);
    }

    internal async ValueTask CompleteCanonicalInboundAsync(
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
        RoutingId sourceNodeRid,
        CancellationToken cancellationToken)
    {
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"aggregate_target_complete_begin relocation={prepare.RelocationId.High:x16}{prepare.RelocationId.Low:x16}");
        var root = prepare.Root
            ?? throw new InvalidDataException(
                "Canonical SPOT relocation requires an immutable root.");
        var fence = new ZLinkAggregateFence(
            DecodeRelocationId(prepare.RelocationId),
            prepare.TargetAttemptGeneration);
        if (!_staged.TryGetValue(fence, out var entry))
        {
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"aggregate_target_complete_lookup relocation={prepare.RelocationId.High:x16}{prepare.RelocationId.Low:x16} "
                + $"attempt={prepare.TargetAttemptGeneration} found=False active={ActiveStageCount} "
                + $"tombstones={TerminalTombstoneCount}");
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                "Canonical target completion has no staged relocation.",
                ZLinkRetryAdvice.RetryAfterBackoff);
        }
        if (entry is TargetStageTombstone terminal)
        {
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"aggregate_target_complete_lookup relocation={prepare.RelocationId.High:x16}{prepare.RelocationId.Low:x16} "
                + $"attempt={prepare.TargetAttemptGeneration} found=True entry=tombstone "
                + $"outcome={terminal.Outcome}");
            if (terminal.SourceNodeRid != sourceNodeRid
                || terminal.Outcome
                   != TargetStageTerminalOutcome.Completed)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    "Canonical target completion conflicts with its terminal relocation.",
                    ZLinkRetryAdvice.RetryAfterBackoff);
            return;
        }
        if (entry is not TargetStage stage
            || stage.SourceNodeRid != sourceNodeRid)
        {
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"aggregate_target_complete_lookup relocation={prepare.RelocationId.High:x16}{prepare.RelocationId.Low:x16} "
                + $"attempt={prepare.TargetAttemptGeneration} found=True entry={entry.GetType().Name} "
                + $"source_match={entry is TargetStage candidate && candidate.SourceNodeRid == sourceNodeRid} "
                + $"published={entry is TargetStage published && Volatile.Read(ref published.Published) != 0}");
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                "Canonical target completion overtook target publication.",
                ZLinkRetryAdvice.RetryAfterBackoff);
        }

        await stage.PublishGate.WaitAsync(cancellationToken)
            .ConfigureAwait(false);
        try
        {
            // Target publication writes the exact authority and catalog before
            // it records Published. A command 35 arriving in that interval
            // must wait for the same gate instead of rejecting a valid target
            // as if publication had been overtaken.
            if (Volatile.Read(ref stage.Published) == 0)
            {
                ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"aggregate_target_complete_lookup relocation={prepare.RelocationId.High:x16}{prepare.RelocationId.Low:x16} "
                    + $"attempt={prepare.TargetAttemptGeneration} found=True entry={entry.GetType().Name} "
                    + $"source_match=True published=False after_gate=True");
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    "Canonical target completion overtook target publication.",
                    ZLinkRetryAdvice.RetryAfterBackoff);
            }
            if (!await IsAuthorityNormalizedAsync(stage, cancellationToken)
                    .ConfigureAwait(false))
                await ValidateDurableCompletionRootAsync(
                        stage,
                        cancellationToken,
                        root)
                    .ConfigureAwait(false);
            await CompleteInboundThenNormalizeCoreAsync(
                    stage,
                    cancellationToken)
                .ConfigureAwait(false);
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"aggregate_target_complete_end relocation={prepare.RelocationId.High:x16}{prepare.RelocationId.Low:x16}");
        }
        finally
        {
            stage.PublishGate.Release();
        }
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
            $"aggregate_target_complete_gate aggregate={stage.Envelope.AggregateId:N}");
        if (stage.AdmissionDrainTask is { } existingDrain)
            await existingDrain.ConfigureAwait(false);
        if (Volatile.Read(
                ref PostPublicationBeforeNormalizationTestHook) is { } hook)
            await hook(cancellationToken).ConfigureAwait(false);

        // Replay remains sealed until the plain Ready authority is durable.
        // Admission opens synchronously after the CAS and before the immutable
        // recovery root is deleted.
        await runtime.CompleteInboundSpotAggregateReplayAsync(
                stage,
                cancellationToken)
            .ConfigureAwait(false);
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
        var relocationStore = registration.Locations.ResolveRelocationStore()
                              ?? throw new ZLinkConfigurationException(
                                  "Relocation Store is not registered.");
        var tree = await ZLinkRelocationTreeStore.ReadAsync(
                relocationStore,
                request.RelocationReference,
                request.RelocationChecksum,
                cancellationToken)
            .ConfigureAwait(false);
        var envelope = await BindCanonicalAuthorityInventoryAsync(
                tree.Envelope,
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
        if (!_stageSlots.TryAcquire(out var activeSlot))
            return false;
        var aggregateBytes = request.AggregateId.ToByteArray(bigEndian: true);
        var canonicalRelocationId =
            new ZLinkServiceWireCodec.RelocationWireId(
                BinaryPrimitives.ReadUInt64BigEndian(
                    aggregateBytes.AsSpan(0, 8)),
                BinaryPrimitives.ReadUInt64BigEndian(
                    aggregateBytes.AsSpan(8, 8)));
        targetNode.BeginCanonicalRelocationStaging(
            canonicalRelocationId,
            request.TargetAttemptGeneration);
        if (!targetNode.TryTakeCanonicalRelocationPermit(
                canonicalRelocationId,
                request.TargetAttemptGeneration,
                tree.LogicalLength,
                out var inboundPermit,
                out var preparedAggregate,
                out var targetAuthorityOwnerGeneration))
        {
            activeSlot.Dispose();
            return false;
        }
        TargetStage stage;
        try
        {
            stage = await runtime.StageInboundSpotAggregateAsync(
                    request,
                    envelope,
                    inboundPermit,
                    preparedAggregate,
                    targetAuthorityOwnerGeneration,
                    cancellationToken)
                .ConfigureAwait(false);
            stage.AttachActiveSlot(activeSlot);
        }
        catch
        {
            if (preparedAggregate is not null)
                await AbortPreparedAggregateAsync(preparedAggregate)
                    .ConfigureAwait(false);
            inboundPermit.Dispose();
            activeSlot.Dispose();
            throw;
        }
        if (!TryTrackStage(fence, stage))
        {
            await AbortTargetStageAsync(stage)
                .ConfigureAwait(false);
            ReleaseStageResources(stage);
            return
                _staged.TryGetValue(fence, out var winner)
                && winner is TargetStage winningStage
                && winningStage.Matches(request, sourceNodeRid);
        }
        ScheduleReconciliation();
        return true;
    }

    private async ValueTask<ZLinkRelocationEnvelope>
        BindCanonicalAuthorityInventoryAsync(
            ZLinkRelocationEnvelope envelope,
            ZLinkCanonicalSpotStageContext request,
            CancellationToken cancellationToken)
    {
        if (envelope.CanonicalLogicalStream.IsEmpty)
            return envelope;
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
        var authorityStore = registration.Locations.ResolveStore()
                             ?? throw new ZLinkConfigurationException(
                                 "Location Store is not registered.");
        var bound = new ZLinkRelocationParticipantEnvelope[orderedStates.Length];
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
            bound[index] = state with
            {
                AuthorityKey = keys[index],
                ObjectKind = index == 0
                    ? request.InstanceSpot
                        ? ZLinkPlacementObjectKind.InstanceSpot
                        : ZLinkPlacementObjectKind.UserSpot
                    : ZLinkPlacementObjectKind.Actor,
                ObjectGeneration = found.Snapshot.ObjectGeneration,
                AuthorityOwnerGeneration = found.Snapshot.AuthorityOwnerGeneration,
                CompletionPayload = index == 0
                    ? ZLinkSpotRetireCompletionMarker.CreatePending()
                    : ReadOnlyMemory<byte>.Empty
            };
        }
        return envelope with
        {
            AggregateGeneration = request.AggregateGeneration,
            Participants = bound
        };
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

    private static bool RequiresRestore(
        ZLinkSpotNodeRuntime node,
        ZLinkCanonicalSpotStageContext request)
    {
        var spotRelocations = request.InstanceSpot
            ? node.Registration.InstanceSpotRelocations
            : node.Registration.SpotRelocations;
        if (spotRelocations.TryGetValue(
                request.StableType,
                out var spot)
            && spot.PolicyKind == 2)
            return true;
        foreach (var actor in request.Actors)
            if (ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
                    actor.AuthorityPayload,
                    out var authority)
                && node.Registration.ActorRelocations.TryGetValue(
                    authority.StableType,
                    out var relocation)
                && relocation.PolicyKind == 2)
                return true;
        return false;
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
        if (stage.PreparedAggregate is { } preparedAggregate)
        {
            _ = await new ZLinkAggregateRelocationCoordinator(
                    registration.Locations.ResolveStore()
                    ?? throw new ZLinkConfigurationException(
                        "Location Store is not registered."),
                    registration.Locations.ResolveRelocationStore()
                    ?? throw new ZLinkConfigurationException(
                        "Relocation Store is not registered."))
                .CommitAsync(preparedAggregate, cancellationToken)
                .ConfigureAwait(false);
            Volatile.Write(ref stage.AuthorityPublished, 1);
            await FinalizeStageAsync(
                    stage,
                    normalizeAuthority: stage.Envelope.CanonicalLogicalStream
                        .IsEmpty,
                    cancellationToken)
                .ConfigureAwait(false);
            if (stage.Envelope.CanonicalLogicalStream.IsEmpty)
                CompleteStage(stage, TargetStageTerminalOutcome.Completed);
            return true;
        }
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
            await ValidateDurableCompletionRootAsync(
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
    }

    private async ValueTask ValidateDurableCompletionRootAsync(
        TargetStage stage,
        CancellationToken cancellationToken,
        ZLinkServiceWireCodec.RelocationRootRecord? expectedRoot = null)
    {
        var authorityStore = registration.Locations.ResolveStore()
                             ?? throw new ZLinkConfigurationException(
                                 "Location Store is not registered.");
        var relocationStore =
            registration.Locations.ResolveRelocationStore()
            ?? throw new ZLinkConfigurationException(
                "Relocation Store is not registered.");
        var stagedSpot = stage.Envelope.Participants.Single(
            static participant => participant.ObjectKind
                is ZLinkPlacementObjectKind.UserSpot
                or ZLinkPlacementObjectKind.InstanceSpot);
        if (expectedRoot is { } commandRoot
            && (!StringComparer.Ordinal.Equals(
                    commandRoot.Reference,
                    stage.RelocationReference)
                || commandRoot.ChecksumCrc32c
                   != stage.RelocationChecksum))
            throw new ZLinkRelocationDataLostException(
                "Canonical relocation completion changed its initial root.");
        var read = await authorityStore.ReadAuthorityAsync(
                stagedSpot.AuthorityKey,
                cancellationToken)
            .ConfigureAwait(false);
        if (read is not ZLinkAuthorityReadResult.Found found
            || !ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                found.Snapshot.Payload.Span,
                out var publication)
            || publication.AggregateId != stage.Envelope.AggregateId)
            throw new ZLinkRelocationDataLostException(
                "Held ingress completion authority is unavailable.");
        var durable = await ZLinkRelocationTreeStore.GetAsync(
                relocationStore,
                publication.Reference,
                publication.ChecksumCrc32c,
                cancellationToken)
            .ConfigureAwait(false);
        if (publication.IsCanonical)
        {
            // Canonical participant components keep the original signed
            // logical stream. A target takeover advances the generation in
            // the authority-linked manifest without rewriting that stream.
            // Bind the generation from the exact published reference before
            // comparing it with the recovered staging state.
            var stagedByParticipantId = stage.Envelope.Participants
                .ToDictionary(
                    static participant =>
                        participant.CanonicalParticipantId);
            var publishedDurable = durable with
            {
                AggregateGeneration = publication.AggregateGeneration,
                Participants = durable.Participants.Select(participant =>
                {
                    if (!stagedByParticipantId.TryGetValue(
                            participant.CanonicalParticipantId,
                            out var stagedIdentity))
                        throw new ZLinkRelocationDataLostException(
                            "Canonical relocation completion contains an unknown participant.");
                    return participant with
                    {
                        AuthorityKey = stagedIdentity.AuthorityKey,
                        ObjectKind = stagedIdentity.ObjectKind,
                        ObjectGeneration = stagedIdentity.ObjectGeneration,
                        AuthorityOwnerGeneration =
                            stage.TargetAuthorityOwnerGenerationFor(
                                stagedIdentity),
                        CompletionPayload =
                            stagedIdentity.ObjectKind is
                                ZLinkPlacementObjectKind.UserSpot
                                or ZLinkPlacementObjectKind.InstanceSpot
                                ? publication.SourceCleanupState == 0
                                    ? ZLinkSpotRetireCompletionMarker
                                        .CreatePending()
                                    : ZLinkSpotRetireCompletionMarker
                                        .CreateCompleted()
                                : ReadOnlyMemory<byte>.Empty
                    };
                }).ToArray()
            };
            if (!CanonicalCompletionMatchesTargetStaging(
                    stage.Envelope,
                    publishedDurable,
                    publication,
                    stage.TargetAuthorityOwnerGenerationFor))
            {
                throw new ZLinkRelocationDataLostException(
                    "Canonical relocation completion does not match target staging.");
            }
            stage.RememberFinalRoot(
                publication.Reference,
                publication.ChecksumCrc32c);
            return;
        }
        var durableSpot = durable.Participants.Single(
            static participant => participant.ObjectKind
                is ZLinkPlacementObjectKind.UserSpot
                or ZLinkPlacementObjectKind.InstanceSpot);
        var expectedJobs = stagedSpot.AcceptedJobs
            .Concat(stage.HeldRecords)
            .OrderBy(static job => job.AcceptedSequence)
            .ToArray();
        if (!ZLinkSpotRetireCompletionMarker.IsCompleted(
                durableSpot.CompletionPayload.Span)
            || durable.AggregateId != publication.AggregateId
            || durable.AggregateGeneration
               != publication.AggregateGeneration
            || !durable.InventoryDigest.Span.SequenceEqual(
                publication.InventoryDigest.Span)
            || durable.Participants.Count != stage.Envelope.Participants.Count
            || !durable.Participants.Select(static item => item.AuthorityKey)
                .OrderBy(static key => key.Value, StringComparer.Ordinal)
                .SequenceEqual(
                    stage.Envelope.Participants
                        .Select(static item => item.AuthorityKey)
                        .OrderBy(static key => key.Value,
                            StringComparer.Ordinal))
            || durableSpot.AcceptedJobs.Count != expectedJobs.Length)
            throw new ZLinkRelocationDataLostException(
                "Held ingress completion root does not match target staging.");
        for (var index = 0; index < expectedJobs.Length; index++)
            if (durableSpot.AcceptedJobs[index].AcceptedSequence
                != expectedJobs[index].AcceptedSequence
                || !durableSpot.AcceptedJobs[index].Payload.Span.SequenceEqual(
                    expectedJobs[index].Payload.Span))
                throw new ZLinkRelocationDataLostException(
                    "Held ingress completion journal does not match target staging.");
        stage.RememberFinalRoot(
            publication.Reference,
            publication.ChecksumCrc32c);
    }

    internal static bool CanonicalCompletionMatchesTargetStaging(
        ZLinkRelocationEnvelope staged,
        ZLinkRelocationEnvelope durable,
        ZLinkRelocationAuthorityPayload publication,
        Func<ZLinkRelocationParticipantEnvelope, ulong>?
            targetAuthorityOwnerGeneration = null)
    {
        var terminalCompletionCount = checked((uint)durable.Participants.Sum(
            static participant => participant.TerminalCompletions.Count));
        var pendingRelayCount = checked((uint)durable.Participants.Sum(
            static participant => participant.PendingRelayCount));
        var common = !durable.CanonicalLogicalStream.IsEmpty
                     && durable.AggregateId == publication.AggregateId
                     && publication.AggregateGeneration
                        == durable.AggregateGeneration
                     && durable.AggregateGeneration
                        >= staged.AggregateGeneration
                     && publication.TerminalCompletionCount
                        == terminalCompletionCount
                     && publication.PendingRelayCount == pendingRelayCount
                     && durable.Participants.Count == staged.Participants.Count;
        if (!common)
            return false;

        var normalizedStaged = targetAuthorityOwnerGeneration is null
            ? staged
            : staged with
            {
                Participants = staged.Participants.Select(participant =>
                    participant with
                    {
                        AuthorityOwnerGeneration =
                            targetAuthorityOwnerGeneration(participant)
                }).ToArray()
            };
        var primaryKind = normalizedStaged.Participants
            .Single(participant =>
                participant.ObjectKind is
                    ZLinkPlacementObjectKind.UserSpot
                    or ZLinkPlacementObjectKind.InstanceSpot)
            .ObjectKind;
        var isSuccessor =
            ZLinkCanonicalRelocationReservationOwner.IsCanonicalSuccessor(
            checked((byte)primaryKind),
            normalizedStaged,
            durable);
        if (!isSuccessor)
            return false;

        // Target publication can contain replayed requests whose jobs were
        // removed from the remaining journal and replaced by terminal
        // completions. Canonical successor validation proves that transition
        // against the immutable staged journal. Source cleanup additionally
        // requires every reply relay to have reached a terminal state.
        return publication.SourceCleanupState switch
        {
            0 => true,
            1 => pendingRelayCount == 0,
            _ => false
        };
    }

    private async ValueTask AbortTargetStageAsync(TargetStage stage)
    {
        await runtime.AbortInboundSpotAggregateAsync(stage)
            .ConfigureAwait(false);
        if (stage.PreparedAggregate is { } prepared)
            await AbortPreparedAggregateAsync(prepared).ConfigureAwait(false);
    }

    private async ValueTask AbortPreparedAggregateAsync(
        ZLinkPreparedAggregateRelocation prepared)
    {
        var authorityStore = registration.Locations.ResolveStore()
            ?? throw new ZLinkConfigurationException(
                "Location Store is not registered.");
        var relocationStore =
            registration.Locations.ResolveRelocationStore()
            ?? throw new ZLinkConfigurationException(
                "Relocation Store is not registered.");
        await new ZLinkAggregateRelocationCoordinator(
                authorityStore, relocationStore)
            .AbortAsync(prepared)
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

        await stage.AbortGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            if (stage.AbortState == TargetStageAbortState.Aborted)
                return true;
            if (Volatile.Read(ref stage.AuthorityPublished) != 0
                || Volatile.Read(ref stage.Published) != 0)
                return false;
            stage.AbortState = TargetStageAbortState.Aborting;
            var authorityStore = registration.Locations.ResolveStore()
                ?? throw new ZLinkConfigurationException(
                    "Location Store is not registered.");
            var removed = await TryCleanupExpiredStageAsync(
                    stage,
                    () => ReconcileStageAuthorityAsync(
                        stage, CancellationToken.None),
                    () => authorityStore.AbortAggregateAsync(
                        fence, CancellationToken.None),
                    () => TryCompleteStage(
                        fence, stage, TargetStageTerminalOutcome.Aborted),
                    () => runtime.AbortInboundSpotAggregateAsync(stage))
                .ConfigureAwait(false);
            stage.AbortState = removed
                ? TargetStageAbortState.Aborted
                : TargetStageAbortState.Staged;
            return removed;
        }
        finally
        {
            stage.AbortGate.Release();
        }
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
        var stagedSpot = staging.Participants.Single(
            static participant => participant.ObjectKind
                is ZLinkPlacementObjectKind.UserSpot
                or ZLinkPlacementObjectKind.InstanceSpot);
        var authoritativeSpot = authoritative.Participants.Single(
            static participant => participant.ObjectKind
                is ZLinkPlacementObjectKind.UserSpot
                or ZLinkPlacementObjectKind.InstanceSpot);
        var samePhase = staging.AggregateGeneration
                        == authoritative.AggregateGeneration
                        && stagedSpot.CompletionPayload.Span.SequenceEqual(
                            authoritativeSpot.CompletionPayload.Span);
        var completedAfterStaging =
            ZLinkSpotRetireCompletionMarker.IsPending(
                stagedSpot.CompletionPayload.Span)
            && (authoritative.AggregateGeneration
                == checked(staging.AggregateGeneration + 1)
                || !staging.CanonicalLogicalStream.IsEmpty
                   && authoritative.AggregateGeneration
                      == staging.AggregateGeneration)
            && ZLinkSpotRetireCompletionMarker.IsCompleted(
                authoritativeSpot.CompletionPayload.Span);
        if (!samePhase && !completedAfterStaging)
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
                && (expected.CanonicalParticipantId
                        != actual.CanonicalParticipantId
                    || expected.AcceptedBoundary
                        != actual.AcceptedBoundary
                    || actual.ReplayCursor < expected.ReplayCursor
                    || actual.ReplayCursor > actual.AcceptedBoundary
                    || !TerminalCompletionsExtend(
                        expected,
                        actual,
                        staging.Participants)))
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

        static bool TerminalCompletionsExtend(
            ZLinkRelocationParticipantEnvelope expected,
            ZLinkRelocationParticipantEnvelope actual,
            IReadOnlyList<ZLinkRelocationParticipantEnvelope> participants)
        {
            if (actual.TerminalCompletions.Count
                < expected.TerminalCompletions.Count)
                return false;
            var actualByOperation = new Dictionary<
                (ulong High, ulong Low, string OwnerId,
                    ulong OwnerLeaseGeneration, string NodeRid,
                    ulong NodeGeneration),
                ZLinkCanonicalTerminalCompletion>();
            foreach (var completion in actual.TerminalCompletions)
            {
                var key = CompletionKey(completion);
                if (!actualByOperation.TryAdd(key, completion)
                    || !MatchesAcceptedRequest(completion, participants))
                    return false;
            }
            foreach (var previous in expected.TerminalCompletions)
            {
                if (!actualByOperation.TryGetValue(
                        CompletionKey(previous),
                        out var current)
                    || !SameTerminal(previous, current)
                    || previous.DeliveryState != 0
                       && current.DeliveryState != previous.DeliveryState)
                    return false;
            }
            return true;
        }

        static bool MatchesAcceptedRequest(
            ZLinkCanonicalTerminalCompletion completion,
            IReadOnlyList<ZLinkRelocationParticipantEnvelope> participants)
        {
            var participant = participants.SingleOrDefault(candidate =>
                candidate.CanonicalParticipantId == completion.ParticipantId);
            if (participant is null)
                return false;
            var request = participant.AcceptedJobs
                .SingleOrDefault(job =>
                    job.AcceptedSequence == completion.AcceptedSequence)
                ?.CanonicalRequest;
            return request is not null
                   && request.OperationHigh == completion.OperationHigh
                   && request.OperationLow == completion.OperationLow
                   && request.Source.OwnerId == completion.SourceOwnerId
                   && request.Source.OwnerLeaseGeneration
                      == completion.SourceOwnerLeaseGeneration
                   && request.Source.NodeRid == completion.SourceNodeRid
                   && request.Source.NodeGeneration
                      == completion.SourceNodeGeneration;
        }

        static bool SameTerminal(
            ZLinkCanonicalTerminalCompletion expected,
            ZLinkCanonicalTerminalCompletion actual) =>
            expected.ParticipantId == actual.ParticipantId
            && expected.AcceptedSequence == actual.AcceptedSequence
            && expected.TerminalResult == actual.TerminalResult
            && expected.ErrorCode == actual.ErrorCode
            && SamePayload(expected.Payload, actual.Payload);

        static bool SamePayload(
            ZLinkCanonicalApplicationPayload? expected,
            ZLinkCanonicalApplicationPayload? actual) =>
            expected is null
                ? actual is null
                : actual is not null
                  && expected.PacketName == actual.PacketName
                  && expected.ContentType == actual.ContentType
                  && expected.Payload.Span.SequenceEqual(actual.Payload.Span);

        static (
            ulong High,
            ulong Low,
            string OwnerId,
            ulong OwnerLeaseGeneration,
            string NodeRid,
            ulong NodeGeneration)
            CompletionKey(ZLinkCanonicalTerminalCompletion completion) =>
            (
                completion.OperationHigh,
                completion.OperationLow,
                completion.SourceOwnerId,
                completion.SourceOwnerLeaseGeneration,
                completion.SourceNodeRid,
                completion.SourceNodeGeneration);

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
                        participant,
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
        var stagedSpotForGeneration = stage.Envelope.Participants.Single(
            static participant => participant.ObjectKind
                is ZLinkPlacementObjectKind.UserSpot
                or ZLinkPlacementObjectKind.InstanceSpot);
        var expectedPublicationGeneration = checked(
            stage.Envelope.AggregateGeneration
            + (stage.Envelope.CanonicalLogicalStream.IsEmpty
               && !ZLinkSpotRetireCompletionMarker.IsCompleted(
                   stagedSpotForGeneration.CompletionPayload.Span)
                ? 1UL
                : 0UL));
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
                   != localOwner.LeaseGeneration
                || publication.IsCanonical
                   && (publication.SourceCleanupState != 1
                       || publication.PendingRelayCount != 0))
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
        var spotParticipant = stage.Envelope.Participants.Single(
            static participant => participant.ObjectKind
                is ZLinkPlacementObjectKind.UserSpot
                or ZLinkPlacementObjectKind.InstanceSpot);
        var aggregateGeneration = checked(
            stage.Envelope.AggregateGeneration
            + (!stage.Envelope.CanonicalLogicalStream.IsEmpty
               || !ZLinkSpotRetireCompletionMarker.IsCompleted(
                   spotParticipant.CompletionPayload.Span)
                ? 2UL
                : 1UL));
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
                    if (Volatile.Read(ref stage.Published) != 0
                        || stage.AbortState != TargetStageAbortState.Staged)
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
        var now = DateTimeOffset.UtcNow;
        RemoveExpiredTombstones(now);
        await CleanupExpiredStagesAsync(now).ConfigureAwait(false);
    }

    internal void RemoveExpiredTombstones(DateTimeOffset now)
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

    private async ValueTask CleanupExpiredStagesAsync(DateTimeOffset now)
    {
        foreach (var entry in _staged.Where(
                     entry => entry.Value is TargetStage stage
                              && IsCleanupCandidate(stage, now))
                 .ToArray())
        {
            var stage = (TargetStage)entry.Value;
            await stage.AbortGate.WaitAsync(CancellationToken.None)
                .ConfigureAwait(false);
            try
            {
                if (!_staged.TryGetValue(entry.Key, out var current)
                    || !ReferenceEquals(current, stage))
                    continue;
                if (Volatile.Read(ref stage.Published) == 0)
                    stage.AbortState = TargetStageAbortState.Aborting;
                var authorityStore = registration.Locations.ResolveStore()
                                     ?? throw new ZLinkConfigurationException(
                                         "Location Store is not registered.");
                var removed = await TryCleanupExpiredStageAsync(
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
                            stage))
                    .ConfigureAwait(false);
                if (removed
                    && Volatile.Read(ref stage.Published) == 0)
                    stage.AbortState = TargetStageAbortState.Aborted;
                else if (!removed)
                    stage.AbortState = TargetStageAbortState.Staged;
            }
            finally
            {
                stage.AbortGate.Release();
            }
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
        DateTimeOffset now) =>
        stage.ExpiresAt <= now
        && (Volatile.Read(ref stage.AuthorityPublished) == 0
            || Volatile.Read(ref stage.Published) != 0);

    internal void CompleteStage(
        TargetStage stage,
        TargetStageTerminalOutcome outcome)
    {
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
        var tombstone = new TargetStageTombstone(
            stage.SourceNodeRid,
            stage.StageRequestDigest.ToArray(),
            stage.HeldRelayDigest?.ToArray(),
            outcome,
            DateTimeOffset.UtcNow + TombstoneRetention);
        if (!_staged.TryUpdate(fence, tombstone, stage))
            return false;
        ReleaseStageResources(stage);
        _terminalOrder.Enqueue(fence);
        while (_terminalOrder.Count > MaxTerminalTombstones
               && _terminalOrder.TryDequeue(out var expired))
            _staged.TryRemove(expired, out _);
        return true;
    }

    private static void ReleaseStageResources(TargetStage stage)
    {
        stage.ReleasePermit();
        stage.ReleaseActiveSlot();
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

internal sealed class ZLinkStageSlotPool(int capacity)
{
    private int _active;

    internal int ActiveCount => Volatile.Read(ref _active);

    internal bool TryAcquire(out IDisposable lease)
    {
        var active = Interlocked.Increment(ref _active);
        if (active <= capacity)
        {
            lease = new SlotLease(this);
            return true;
        }
        Interlocked.Decrement(ref _active);
        lease = null!;
        return false;
    }

    private void Release()
    {
        if (Interlocked.Decrement(ref _active) < 0)
            throw new InvalidOperationException(
                "SPOT relocation active slot accounting became negative.");
    }

    private sealed class SlotLease(ZLinkStageSlotPool owner) : IDisposable
    {
        private ZLinkStageSlotPool? _owner = owner;

        public void Dispose() =>
            Interlocked.Exchange(ref _owner, null)?.Release();
    }
}

internal interface ITargetStageEntry
{
    DateTimeOffset ExpiresAt { get; }
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
    DateTimeOffset ExpiresAt) : ITargetStageEntry
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
    string RelocationReference,
    uint RelocationChecksum,
    DateTimeOffset ExpiresAt,
    byte[] StageRequestDigest,
    ZLinkSpotRelocationSeal TargetAdmissionSeal,
    IDisposable InboundPermit,
    ZLinkPreparedAggregateRelocation? PreparedAggregate,
    IReadOnlyDictionary<string, ulong>
        ActorTargetAuthorityOwnerGenerations,
    ulong SourceAuthorityOwnerGeneration,
    ulong TargetAuthorityOwnerGeneration,
    string TargetMeshName,
    ulong TargetNodeLifecycleGeneration,
    ulong TargetOwnerLeaseGeneration,
    ulong TargetAttemptGeneration) : ITargetStageEntry
{
    public int AuthorityPublished;
    public int Published;
    public int AdmissionOpened;
    public Task? AdmissionDrainTask;
    public int ReplayedJobCount;
    public int LocalCatalogPublished;
    public int RelocationReadyCompletionDelivered;
    public SemaphoreSlim PublishGate { get; } = new(1, 1);
    public object HeldGate { get; } = new();
    public byte[]? HeldDigest;
    public byte[]? HeldRelayDigest;
    public ulong HeldHighWater;
    public IReadOnlyList<ZLinkRelocationQueuedJob> HeldRecords = [];
    private readonly object _finalRootGate = new();
    private string? _finalRootReference;
    private uint _finalRootChecksum;
    private IDisposable? _activeSlot;
    private int _permitReleased;
    private int _abortState;

    internal SemaphoreSlim AbortGate { get; } = new(1, 1);

    internal TargetStageAbortState AbortState
    {
        get => (TargetStageAbortState)Volatile.Read(ref _abortState);
        set => Volatile.Write(ref _abortState, (int)value);
    }

    internal ulong TargetActorAuthorityOwnerGeneration(string actorId) =>
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
                ZLinkActorAuthorityPayloadCodec.AuthorityKey(candidate.Key)
                == participant.AuthorityKey);
        return actor.Key is not null
            ? actor.Value
            : throw new ZLinkRelocationDataLostException(
                $"Actor authority '{participant.AuthorityKey.Value}' does not have a prepared target generation.");
    }

    internal async ValueTask<bool> RunAbortCleanupAsync(
        Func<ValueTask> cleanup,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(cleanup);
        await AbortGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            if (AbortState == TargetStageAbortState.Aborted)
                return false;
            AbortState = TargetStageAbortState.Aborting;
            await cleanup().ConfigureAwait(false);
            AbortState = TargetStageAbortState.Aborted;
            return true;
        }
        finally
        {
            AbortGate.Release();
        }
    }

    internal void AttachActiveSlot(IDisposable activeSlot)
    {
        ArgumentNullException.ThrowIfNull(activeSlot);
        if (Interlocked.CompareExchange(
                ref _activeSlot,
                activeSlot,
                null) is not null)
            throw new InvalidOperationException(
                "SPOT relocation stage already owns an active slot.");
    }

    internal void ReleaseActiveSlot() =>
        Interlocked.Exchange(ref _activeSlot, null)?.Dispose();

    internal void RememberFinalRoot(string reference, uint checksum)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(reference);
        lock (_finalRootGate)
        {
            // Accepted replay and reply ACKs publish successor immutable roots
            // before steady normalization. Every caller validates the
            // relocation/attempt and authority fence before recording the
            // pointer, so cleanup must retain the latest verified root.
            _finalRootReference = reference;
            _finalRootChecksum = checksum;
        }
    }

    internal (string Reference, uint Checksum)? GetFinalRoot()
    {
        lock (_finalRootGate)
            return _finalRootReference is null
                ? null
                : (_finalRootReference, _finalRootChecksum);
    }

    internal void ReleasePermit()
    {
        if (Interlocked.Exchange(ref _permitReleased, 1) == 0)
            InboundPermit.Dispose();
    }

    internal bool Matches(
        ZLinkCanonicalSpotStageContext request,
        RoutingId sourceNodeRid) =>
        SourceNodeRid == sourceNodeRid
        && StageRequestDigest.AsSpan().SequenceEqual(
            ZLinkSpotRetireTargetRuntime.ComputeStageRequestDigest(request));
}
