using System.Runtime.ExceptionServices;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Diagnostics;

namespace Zlink.Framework.Runtime.Spots;

/// <summary>
/// Target-side bridge for the service-wire relocation operation. The bridge
/// stages factory, application Restore, accepted journal and timers without
/// making the activation visible. It publishes only after the source commits
/// the aggregate authority fence.
/// </summary>
internal interface IZLinkSpotRetireTarget
{
    ValueTask<ZLinkSpotRetireReservation?> TryReserveAsync(
        ZLinkSpotRetireInventory inventory,
        ZLinkRelocationTargetSelection selection,
        CancellationToken cancellationToken);

    ValueTask<ZLinkSpotRetireReservation?> TryReserveForPreflightAsync(
        ZLinkSpotRetireInventory inventory,
        ZLinkRetirePreflightPlan plan,
        ZLinkRelocationTargetSelection selection,
        CancellationToken cancellationToken) =>
        TryReserveAsync(inventory, selection, cancellationToken);

    ValueTask StageAsync(
        ZLinkSpotRetireReservation reservation,
        ZLinkPreparedSpotRetireStaging relocation,
        CancellationToken cancellationToken);

    ValueTask<ulong> PublishAsync(
        ZLinkSpotRetireReservation reservation,
        ZLinkAggregateRelocationPublished relocation,
        CancellationToken cancellationToken);

    ValueTask AbortAsync(
        ZLinkSpotRetireReservation reservation,
        ZLinkAggregateFence? fence);

    ValueTask RelayCommittedAsync(
        ZLinkSpotRetireReservation reservation,
        ZLinkAggregateRelocationPublished relocation,
        IReadOnlyList<ZLinkAcceptedWorkRecord> held,
        CancellationToken cancellationToken);
}

internal sealed class ZLinkRetirePreflightPlan
{
    private readonly Dictionary<string, Usage> _usage = new(StringComparer.Ordinal);

    internal bool TryReserve(
        ZLinkMeshNodeDescriptor descriptor,
        ZLinkCapacityVector capacity,
        int activationCount = 1)
    {
        var key = $"{descriptor.MeshName}\0{descriptor.Rid.ToHex()}";
        _usage.TryGetValue(key, out var used);
        used ??= new Usage();
        if (!ZLinkSpotRetireTargetRuntime.HasHeadroom(
                descriptor.Capacity.Actors,
                checked(used.Actors + capacity.Actors))
            || !ZLinkSpotRetireTargetRuntime.HasHeadroom(
                descriptor.Capacity.Spots,
                checked(used.Spots + capacity.Spots))
            || descriptor.ActivationConcurrency.Limit
               - descriptor.ActivationConcurrency.Active
               < checked(used.Activations + activationCount))
            return false;

        if (capacity.SpotType is { } delta)
        {
            var typeCapacity = descriptor.Capacity.SpotTypes.SingleOrDefault(candidate =>
                candidate.ObjectKind == delta.ObjectKind
                && StringComparer.Ordinal.Equals(
                    candidate.StableType,
                    delta.StableType));
            used.SpotTypes.TryGetValue(
                (delta.ObjectKind, delta.StableType),
                out var usedForType);
            if (typeCapacity is null
                || !ZLinkSpotRetireTargetRuntime.HasHeadroom(
                    new ZLinkPopulationCapacity(
                        typeCapacity.Active,
                        typeCapacity.Reserved,
                        typeCapacity.Limit),
                    checked(usedForType + delta.Count)))
                return false;
        }

        used.Actors = checked(used.Actors + capacity.Actors);
        used.Spots = checked(used.Spots + capacity.Spots);
        used.Activations = checked(used.Activations + activationCount);
        if (capacity.SpotType is { } committedType)
            used.SpotTypes[(committedType.ObjectKind, committedType.StableType)] =
                checked(used.SpotTypes.GetValueOrDefault(
                    (committedType.ObjectKind, committedType.StableType))
                    + committedType.Count);
        _usage[key] = used;
        return true;
    }

    private sealed class Usage
    {
        internal int Actors { get; set; }
        internal int Spots { get; set; }
        internal int Activations { get; set; }
        internal Dictionary<(ZLinkPlacementObjectKind Kind, string StableType), int>
            SpotTypes { get; } = [];
    }
}

internal sealed record ZLinkSpotRetireInventory(
    string MeshName,
    RoutingId SourceNodeRid,
    ulong SourceNodeLifecycleGeneration,
    ZLinkLocationOwnerToken SourceOwner,
    string SpotId,
    string StableType,
    Type SpotType,
    bool InstanceSpot,
    bool PerActorShell,
    ulong ObjectGeneration,
    IReadOnlyList<string> ActorIds,
    IReadOnlyList<ZLinkObjectCapability> RequiredCapabilities);

internal sealed record ZLinkSpotRetireReservation(
    ZLinkSpotRetireInventory Inventory,
    ZLinkMeshNodeDescriptorKey TargetDescriptor,
    ulong TargetDescriptorLifecycleGeneration,
    ZLinkCapacityVector Capacity,
    ZLinkLocationOwnerToken TargetOwner);

internal sealed record ZLinkPreparedSpotRetireStaging(
    ZLinkPreparedRelocation Root,
    IReadOnlyList<ZLinkAggregateRelocationParticipant> Participants)
{
    internal ZLinkRelocationStored Relocation => Root.Relocation;

    internal ZLinkRelocationEnvelope Envelope => Root.Envelope;
}

internal static class ZLinkSpotRetireCompletionMarker
{
    private static ReadOnlySpan<byte> SourceCleanupPending =>
        "zlink.spot.source.pending.v1"u8;

    private static ReadOnlySpan<byte> SourceCleanupCompleted =>
        "zlink.spot.source.completed.v1"u8;

    internal static byte[] CreatePending() =>
        SourceCleanupPending.ToArray();

    internal static byte[] CreateCompleted() =>
        SourceCleanupCompleted.ToArray();

    internal static bool IsCompleted(ReadOnlySpan<byte> payload) =>
        payload.SequenceEqual(SourceCleanupCompleted);

    internal static bool IsPending(ReadOnlySpan<byte> payload) =>
        payload.SequenceEqual(SourceCleanupPending);
}

/// <summary>
/// Runs one source Spot relocation transaction. Queue and timer dispatch stay
/// available until every process-wide permit is acquired. Any failure before
/// authority commit aborts target staging and resumes the exact source seal.
/// </summary>
internal sealed class ZLinkSpotRetireScheduler(
    IZLinkLocationRepository authorityStore,
    IZLinkRelocationRepository relocationStore,
    IZLinkSpotRetireTarget target,
    ZLinkRelocationPermitPool permits,
    ZLinkFrameworkRuntime runtime)
{
    private const int MaxConcurrentParticipantIo = 8;
    private const long SnapshotReservationBytes = 64L * 1024 * 1024;
    private const long SourceIngressHoldReservationBytes =
        16L * 1024 * 1024;
    private const long EnvelopeHeaderBytes =
        sizeof(uint) + sizeof(ushort) + 16 + sizeof(ulong)
        + sizeof(int) + 32 + sizeof(int);

    internal static ZLinkFrameworkRelocationReason MapFailureReason(
        Exception exception,
        bool committed) =>
        committed
            ? ZLinkFrameworkRelocationReason.RelocationFailed
            : ZLinkActorRelocationFailureException.MapReason(exception);

    private static CancellationTokenSource CreateDeadlineTokenSource(
        DateTimeOffset deadline)
    {
        var remaining = deadline - DateTimeOffset.UtcNow;
        var source = new CancellationTokenSource();
        source.CancelAfter(
            remaining > TimeSpan.Zero
                ? remaining
                : TimeSpan.FromTicks(1));
        return source;
    }

    internal async ValueTask<ZLinkFrameworkRelocationReason?> PreflightAsync(
        IReadOnlyList<(ZLinkSpotActivation Activation, bool Instance)> units,
        ZLinkRetirePreflightPlan plan,
        ZLinkRelocationTargetSelection selection,
        CancellationToken cancellationToken)
    {
        foreach (var unit in units)
        {
            var inventory = CreateInventory(unit.Activation, unit.Instance);
            if (inventory.RequiredCapabilities.Any(static capability =>
                    capability.Policy == ZLinkObjectMaintenancePolicyKind.Disabled))
                return ZLinkFrameworkRelocationReason.RelocationDisabled;
            if (await target.TryReserveForPreflightAsync(
                    inventory,
                    plan,
                    selection,
                    cancellationToken)
                    .ConfigureAwait(false) is null)
                return ZLinkFrameworkRelocationReason.TargetUnavailable;
        }
        return null;
    }

    private static ZLinkSpotRetireInventory CreateInventory(
        ZLinkSpotActivation activation,
        bool instanceSpot,
        IReadOnlyList<string>? actorIds = null,
        bool perActorShell = false)
    {
        actorIds ??= activation.SnapshotActorIds();
        var includeActors = actorIds.Count != 0;
        return new ZLinkSpotRetireInventory(
            activation.MeshName,
            activation.NodeRid,
            activation.SourceNodeLifecycleGeneration,
            activation.SourceOwnerToken,
            activation.SpotId,
            activation.ResolveStableTypeForRetire(),
            activation.Spot.GetType(),
            instanceSpot,
            perActorShell,
            activation.ObjectGeneration,
            actorIds,
            activation.ResolveRetireCapabilities(
                instanceSpot,
                includeActors));
    }

    internal async ValueTask<ZLinkRelocationUnitResult> TryRelocateAsync(
        ZLinkSpotActivation activation,
        bool instanceSpot,
        ZLinkRelocationTargetSelection selection,
        DateTimeOffset deadline,
        Func<ZLinkSpotActivation, CancellationToken, ValueTask> completeSource,
        CancellationToken cancellationToken)
    {
        var policy = activation.ResolveSpotRelocationRegistrationForRetire().PolicyKind
                     == 2
            ? ZLinkRelocationMetricPolicy.Snapshot
            : ZLinkRelocationMetricPolicy.Recreate;
        var metric = ZLinkRuntimeMetrics.CreateRelocation(
            activation.MeshName,
            instanceSpot
                ? ZLinkRelocationMetricObjectKind.InstanceSpot
                : ZLinkRelocationMetricObjectKind.UserSpot,
            policy);
        metric.Start();
        try
        {
            var result = await TryRelocateCoreAsync(
                    activation,
                    instanceSpot,
                    selection,
                    deadline,
                    completeSource,
                    cancellationToken)
                .ConfigureAwait(false);
            metric.Complete(result.Outcome switch
            {
                ZLinkRelocationUnitOutcome.Completed =>
                    ZLinkRelocationMetricOutcome.Completed,
                ZLinkRelocationUnitOutcome.Pending =>
                    ZLinkRelocationMetricOutcome.Aborted,
                _ when result.TerminalReason
                    == ZLinkFrameworkRelocationReason.ShutdownRequested =>
                    ZLinkRelocationMetricOutcome.Shutdown,
                _ => ZLinkRelocationMetricOutcome.Failed
            });
            return result;
        }
        catch (OperationCanceledException)
        {
            metric.Complete(ZLinkRelocationMetricOutcome.Shutdown);
            throw;
        }
        catch
        {
            metric.Complete(ZLinkRelocationMetricOutcome.Failed);
            throw;
        }
    }

    private async ValueTask<ZLinkRelocationUnitResult> TryRelocateCoreAsync(
        ZLinkSpotActivation activation,
        bool instanceSpot,
        ZLinkRelocationTargetSelection selection,
        DateTimeOffset deadline,
        Func<ZLinkSpotActivation, CancellationToken, ValueTask> completeSource,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(activation);
        ArgumentNullException.ThrowIfNull(completeSource);
        cancellationToken.ThrowIfCancellationRequested();
        using var cleanupDeadline = CreateDeadlineTokenSource(deadline);

        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"relocation_begin spot={activation.SpotId} instance={instanceSpot}");
        var perActorShell = !instanceSpot
                            && activation.ExecutionMode
                            == ZLinkUserSpotExecutionMode.PerActor;
        var actorIds = perActorShell
            ? Array.Empty<string>()
            : activation.SnapshotActorIds();
        var participantKeys = new[]
            {
                ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(
                    activation.SpotId)
            }
            .Concat(actorIds.Select(
                ZLinkActorAuthorityPayloadCodec.AuthorityKey))
            .ToArray();
        var snapshotParticipantCount = CountSnapshotParticipants(
            activation,
            actorIds);
        var requiresCapture = snapshotParticipantCount > 0;
        var inventory = CreateInventory(
            activation,
            instanceSpot,
            perActorShell ? activation.SnapshotActorIds() : actorIds,
            perActorShell);
        var reservation = await target.TryReserveAsync(
                inventory,
                selection,
                cancellationToken)
            .ConfigureAwait(false);
        if (reservation is null)
        {
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"relocation_waiting_for_reservation spot={activation.SpotId}");
            return ZLinkRelocationUnitResult.Pending();
        }
        ZLinkSpotRetireReservation activeReservation = reservation;
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"relocation_reserved spot={activation.SpotId} target={reservation.TargetDescriptor.Rid}");

        ZLinkSpotRelocationSeal admittedSeal;
        try
        {
            admittedSeal = activation.RelocationReadiness
                           == ZLinkSpotRelocationReadinessMode.ApplicationSignaled
                ? await activation
                    .WaitForRelocationReadyTurnAsync(cancellationToken)
                    .ConfigureAwait(false)
                : await activation.SealRelocationAsync(
                        allowActorClaims: perActorShell,
                        cancellationToken)
                    .ConfigureAwait(false);
        }
        catch
        {
            activation.CancelRelocationReadyWait();
            await target.AbortAsync(reservation, fence: null)
                .ConfigureAwait(false);
            throw;
        }

        async ValueTask ReleaseUncommittedSealAsync(
            ZLinkSpotRelocationSeal currentSeal)
        {
            try
            {
                await activation
                    .CompleteRelocationReadyBeforeAbortAsync(
                        currentSeal,
                        cleanupDeadline.Token)
                    .ConfigureAwait(false);
            }
            finally
            {
                try
                {
                    if (!activation.AbortRelocation(currentSeal))
                        throw new ZLinkRelocationDataLostException(
                            $"SPOT '{activation.SpotId}' could not restore its source admission seal.");
                }
                finally
                {
                    await target.AbortAsync(reservation, fence: null)
                        .ConfigureAwait(false);
                }
            }
        }

        ZLinkRelocationPermitPool.ZLinkRelocationPermitLease permit = default;
        bool permitAcquired;
        try
        {
            permitAcquired = permits.TryAcquire(
                ZLinkRelocationPermitRequest.Outbound(
                    CalculatePayloadReservation(
                        snapshotParticipantCount,
                        participantKeys,
                        admittedSeal.QueueSeal.QueueSeal.Captured,
                        admittedSeal.LogicalTimers),
                    requiresCapture,
                    allowOversizedPayload: !instanceSpot),
                out permit);
        }
        catch
        {
            permit.Dispose();
            await ReleaseUncommittedSealAsync(admittedSeal)
                .ConfigureAwait(false);
            throw;
        }
        if (!permitAcquired)
        {
            await ReleaseUncommittedSealAsync(admittedSeal)
                .ConfigureAwait(false);
            return ZLinkRelocationUnitResult.Pending();
        }
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"relocation_sealed spot={activation.SpotId}");
        var interruption =
            activation.StartRelocationInterruption(instanceSpot);

        using (permit)
        {
            ZLinkSpotRelocationSeal? seal = admittedSeal;
            ZLinkPreparedSpotRetireStaging? staging = null;
            ZLinkAggregateRelocationPublished? published = null;
            IReadOnlyList<ZLinkAcceptedWorkRecord> heldAtCutoff = [];
            IReadOnlyList<ZLinkAcceptedWorkRecord> committedHeld = [];
            var committed = false;
            var stageStarted = false;
            var targetPublished = false;
            ulong targetAuthorityOwnerGeneration = 0;
            var messageFollowStarted = false;
            var sourceCommitted = false;
            var committedHeldValidated = false;
            var sourceCleanupCompleted = false;
            var targetCompletionDelivered = false;
            var sourceCompleted = false;
            var aggregateId = Guid.NewGuid();
            var actorCaptures =
                new Dictionary<string, SourceActorCapture>(
                    StringComparer.Ordinal);
            var actorMessageFollowBacklog = new List<Task<bool>>();
            Dictionary<string, SourceActorCapture> activeActorCaptures = actorCaptures;
            List<Task<bool>> activeActorMessageFollowBacklog = actorMessageFollowBacklog;
            ZLinkSpotRetireInventory activeInventory = inventory;
            var sealedSessionRoutes =
                new Dictionary<string, ZLinkRemoteActorBoundSessionRoute>(
                    StringComparer.Ordinal);
            try
            {
                var sealedActorIds = activation.SnapshotActorIds();
                if (!perActorShell && !sealedActorIds.SequenceEqual(
                        actorIds,
                        StringComparer.Ordinal))
                    throw new InvalidOperationException(
                        $"SPOT '{activation.SpotId}' participant inventory changed before the relocation seal.");
                var handoffId = aggregateId.ToString("N");
                var sourceActorStates = actorIds
                    .Select(runtime.GetOrCreateActorState)
                    .ToArray();
                foreach (var actorState in sourceActorStates)
                {
                    var sourceActor = actorState.NativeActorRef
                                      ?? throw new ZLinkFrameworkException(
                                          ZLinkFrameworkErrorKind.NotFound,
                                          $"Actor '{actorState.ActorId}' has no source reference.");
                    actorCaptures.Add(
                        actorState.ActorId,
                        new SourceActorCapture(actorState, sourceActor));
                }
                for (var first = 0;
                     first < sourceActorStates.Length;
                     first += MaxConcurrentParticipantIo)
                {
                    var count = Math.Min(
                        MaxConcurrentParticipantIo,
                        sourceActorStates.Length - first);
                    await Task.WhenAll(
                            sourceActorStates
                                .Skip(first)
                                .Take(count)
                                .Select(async state =>
                                {
                                    _ = await state.BeginHandoffCaptureAsync(
                                            cancellationToken)
                                        .ConfigureAwait(false);
                                    actorCaptures[state.ActorId].CaptureStarted =
                                        true;
                                }))
                        .ConfigureAwait(false);
                }
                var sealedRoutes = new ZLinkRemoteActorBoundSessionRoute[
                    actorIds.Count];
                for (var first = 0;
                     first < actorIds.Count;
                     first += MaxConcurrentParticipantIo)
                {
                    var count = Math.Min(
                        MaxConcurrentParticipantIo,
                        actorIds.Count - first);
                    await Task.WhenAll(
                            Enumerable.Range(first, count)
                                .Select(async index =>
                                {
                                    sealedRoutes[index] = await activation
                                        .SealActorBoundSessionRouteForRetireAsync(
                                            actorIds[index],
                                            handoffId,
                                            cancellationToken)
                                        .ConfigureAwait(false);
                                }))
                        .ConfigureAwait(false);
                }
                for (var index = 0; index < actorIds.Count; index++)
                {
                    var actorId = actorIds[index];
                    var route = sealedRoutes[index];
                    if (route.IsBound)
                        sealedSessionRoutes.Add(actorId, route);
                }
                var application = await activation
                    .CaptureSealedRelocationApplicationStateAsync(
                        seal,
                        actorIds.ToHashSet(StringComparer.Ordinal),
                        cancellationToken)
                    .ConfigureAwait(false);
                ValidateSnapshotPayloadSize(application.SpotState);
                foreach (var actorState in application.ActorStates.Values)
                    ValidateSnapshotPayloadSize(actorState);
                foreach (var capture in actorCaptures.Values)
                {
                    capture.State.Handoff.SealCapture();
                    capture.CommitBoundary = capture.State.Handoff
                        .FreezeCaptureCommitBoundary();
                }
                var participants = await BuildParticipantsAsync(
                        activation,
                        seal,
                        application,
                        actorIds,
                        aggregateId,
                        sealedSessionRoutes,
                        actorCaptures,
                        inventory.RequiredCapabilities,
                        includeSpotTimers: !perActorShell,
                        cancellationToken)
                    .ConfigureAwait(false);
                if (!activation.FreezeRelocationIngress(
                        seal,
                        out heldAtCutoff))
                    throw new InvalidOperationException(
                        "SPOT could not freeze its bounded ingress hold at the commit boundary.");
                ZLinkSpotRetireTargetRuntime.ValidateHeldRecords(
                    heldAtCutoff.Select(
                            static record => new ZLinkSpotRetireHeldRecord(
                                record.AcceptedSequence,
                                record.Payload.ToArray()))
                        .ToArray());
                participants = await AppendHeldIngressAsync(
                        activation.MeshName,
                        participants,
                        heldAtCutoff,
                        cancellationToken)
                    .ConfigureAwait(false);
                var sourceDescriptor = await ReadSourceDescriptorAsync(
                        activation.MeshName,
                        inventory.SourceNodeRid,
                        inventory.SourceNodeLifecycleGeneration,
                        inventory.SourceOwner,
                        cancellationToken)
                    .ConfigureAwait(false);
                var stagingEnvelope = ZLinkCanonicalSpotRelocationWriter.CreateInitial(
                    new ZLinkRelocationEnvelope(
                    aggregateId,
                    1,
                    ZLinkAggregateInventoryDigest.Compute(participants),
                    participants.Select(static participant =>
                            participant.Envelope)
                        .ToArray()),
                    activation.SpotId,
                    inventory.StableType,
                    inventory.SourceNodeRid,
                    sourceDescriptor.ApplicationVersion);
                var stagingRoot = await new ZLinkRelocationPublicationCoordinator(
                        authorityStore,
                        relocationStore)
                    .PrepareAsync(stagingEnvelope, cancellationToken)
                    .ConfigureAwait(false);
                staging = new ZLinkPreparedSpotRetireStaging(
                    stagingRoot,
                    participants);
                var actualPayloadBytes =
                    ZLinkRelocationEnvelopeCodec.MeasureEncodedLength(
                        staging.Envelope);
                if (!permit.TryShrinkPayload(checked(
                        actualPayloadBytes
                        + SourceIngressHoldReservationBytes)))
                    throw new InvalidOperationException(
                        $"SPOT '{activation.SpotId}' relocation payload exceeded its sealed reservation"
                        + $" (actual={actualPayloadBytes + SourceIngressHoldReservationBytes},"
                        + $" reserved={permit.ReservedPayloadBytes}).");

                ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"relocation_stage_begin spot={activation.SpotId} aggregate={aggregateId:N}");
                published = new ZLinkAggregateRelocationPublished(
                    new ZLinkAggregateFence(aggregateId, 1),
                    staging.Relocation,
                    staging.Envelope);
                stageStarted = true;
                await target.StageAsync(
                        reservation,
                        staging,
                        cancellationToken)
                    .ConfigureAwait(false);
                ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"relocation_stage_end spot={activation.SpotId} aggregate={aggregateId:N}");
                // Command 34 is acknowledged only after the target has
                // restored the complete staged journal and published
                // authority. Target execution remains bounded and sealed
                // until command 35 closes the source cutover.
                interruption.Complete();
                // StageAsync returns only after the source sends command 34
                // response=true. That response authorizes the target commit,
                // so cancellation or an unobserved commit must never reopen
                // source admission from this point forward.
                committed = true;
                await CompleteCommittedWithinDeadlineAsync()
                    .ConfigureAwait(false);
                return ZLinkRelocationUnitResult.Completed();
            }
            catch (ZLinkCanonicalRelocationDurablyAbortedException)
            {
                committed = false;
                try
                {
                    await ExecutePrecommitAbortAsync(
                            null,
                            async () =>
                            {
                                var handoffId = aggregateId.ToString("N");
                                foreach (var (actorId, route) in sealedSessionRoutes)
                                    await activation
                                        .AbortActorBoundSessionRouteSealForRetireAsync(
                                            actorId,
                                            route,
                                            handoffId,
                                            cleanupDeadline.Token)
                                        .ConfigureAwait(false);
                            },
                            () => target.AbortAsync(
                                reservation,
                                new ZLinkAggregateFence(aggregateId, 1)),
                            async () =>
                            {
                                if (seal is not null)
                                {
                                    await activation
                                        .CompleteRelocationReadyBeforeAbortAsync(
                                            seal,
                                            cleanupDeadline.Token)
                                        .ConfigureAwait(false);
                                    if (!activation.AbortRelocation(seal))
                                        throw new ZLinkRelocationDataLostException(
                                            $"SPOT '{activation.SpotId}' could not restore its source admission seal.");
                                }
                                await RestoreSourceActorsAsync()
                                    .ConfigureAwait(false);
                            })
                        .ConfigureAwait(false);
                    await DiscardStagingAsync().ConfigureAwait(false);
                    return ZLinkRelocationUnitResult.Terminal(
                        ZLinkFrameworkRelocationReason.RelocationFailed,
                        ZLinkRelocationCommitKnowledge.NotCommitted,
                        sourceTerminalized: true);
                }
                catch
                {
                    return ZLinkRelocationUnitResult.Terminal(
                        ZLinkFrameworkRelocationReason.RelocationFailed,
                        ZLinkRelocationCommitKnowledge.Unknown,
                        sourceTerminalized: false);
                }
            }
            catch (Exception exception)
            {
                if (stageStarted && !committed)
                {
                    return await ReconcileUncertainCommitAsync()
                        .ConfigureAwait(false);
                }
                if (committed)
                {
                    // Authority is the visibility boundary. Never resume the
                    // source after it points at the target; finish idempotent
                    // target publication and source cleanup instead.
                    try
                    {
                        await CompleteCommittedWithinDeadlineAsync()
                            .ConfigureAwait(false);
                        return ZLinkRelocationUnitResult.Terminal(
                            ZLinkFrameworkRelocationReason.RelocationFailed,
                            targetPublished
                                ? ZLinkRelocationCommitKnowledge.Committed
                                : ZLinkRelocationCommitKnowledge.Unknown,
                            sourceTerminalized: sourceCompleted);
                    }
                    catch
                    {
                        return ZLinkRelocationUnitResult.Terminal(
                            ZLinkFrameworkRelocationReason.RelocationFailed,
                            targetPublished
                                ? ZLinkRelocationCommitKnowledge.Committed
                                : ZLinkRelocationCommitKnowledge.Unknown,
                            sourceTerminalized: sourceCompleted);
                    }
                }
                else
                {
                    try
                    {
                        await ExecutePrecommitAbortAsync(
                                null,
                                async () =>
                                {
                                    var handoffId = aggregateId.ToString("N");
                                    foreach (var (actorId, route)
                                             in sealedSessionRoutes)
                                        await activation
                                            .AbortActorBoundSessionRouteSealForRetireAsync(
                                                actorId,
                                                route,
                                                handoffId,
                                                cleanupDeadline.Token)
                                            .ConfigureAwait(false);
                                },
                                () => target.AbortAsync(
                                    reservation,
                                    new ZLinkAggregateFence(aggregateId, 1)),
                                async () =>
                                {
                                    if (seal is not null)
                                    {
                                        await activation
                                            .CompleteRelocationReadyBeforeAbortAsync(
                                                seal,
                                                cleanupDeadline.Token)
                                            .ConfigureAwait(false);
                                        if (!activation.AbortRelocation(seal))
                                            throw new ZLinkRelocationDataLostException(
                                                $"SPOT '{activation.SpotId}' could not restore its source admission seal.");
                                    }
                                    await RestoreSourceActorsAsync()
                                        .ConfigureAwait(false);
                                })
                            .ConfigureAwait(false);
                        await DiscardStagingAsync().ConfigureAwait(false);
                        return ZLinkRelocationUnitResult.Terminal(
                            MapFailureReason(exception, committed: false),
                            ZLinkRelocationCommitKnowledge.NotCommitted,
                            sourceTerminalized: true);
                    }
                    catch
                    {
                        return ZLinkRelocationUnitResult.Terminal(
                            ZLinkFrameworkRelocationReason.RelocationFailed,
                            ZLinkRelocationCommitKnowledge.Unknown,
                            sourceTerminalized: false);
                    }
                }
            }

            async ValueTask<ZLinkRelocationUnitResult>
                ReconcileUncertainCommitAsync()
            {
                try
                {
                    // StageAsync may have published the target before its ACK
                    // was observed. PublishAsync performs the exact aggregate
                    // fence and Location authority reconciliation; only its
                    // durable-abort result permits source restoration.
                    await CompleteCommittedWithinDeadlineAsync()
                        .ConfigureAwait(false);
                    return ZLinkRelocationUnitResult.Terminal(
                        ZLinkFrameworkRelocationReason.RelocationFailed,
                        targetPublished
                            ? ZLinkRelocationCommitKnowledge.Committed
                            : ZLinkRelocationCommitKnowledge.Unknown,
                        sourceTerminalized: sourceCompleted);
                }
                catch (ZLinkCanonicalRelocationDurablyAbortedException)
                {
                    committed = false;
                    try
                    {
                        await AbortUncommittedAsync().ConfigureAwait(false);
                        return ZLinkRelocationUnitResult.Terminal(
                            ZLinkFrameworkRelocationReason.RelocationFailed,
                            ZLinkRelocationCommitKnowledge.NotCommitted,
                            sourceTerminalized: true);
                    }
                    catch
                    {
                        return ZLinkRelocationUnitResult.Terminal(
                            ZLinkFrameworkRelocationReason.RelocationFailed,
                            ZLinkRelocationCommitKnowledge.Unknown,
                            sourceTerminalized: false);
                    }
                }
                catch (Exception exception)
                {
                    return ZLinkRelocationUnitResult.Terminal(
                        MapFailureReason(
                            exception,
                            committed: targetPublished),
                        targetPublished
                            ? ZLinkRelocationCommitKnowledge.Committed
                            : ZLinkRelocationCommitKnowledge.Unknown,
                        sourceTerminalized: sourceCompleted);
                }
            }

            async ValueTask AbortUncommittedAsync()
            {
                await ExecutePrecommitAbortAsync(
                        null,
                        async () =>
                        {
                            var handoffId = aggregateId.ToString("N");
                            foreach (var (actorId, route) in sealedSessionRoutes)
                                await activation
                                    .AbortActorBoundSessionRouteSealForRetireAsync(
                                        actorId,
                                        route,
                                        handoffId,
                                        cleanupDeadline.Token)
                                    .ConfigureAwait(false);
                        },
                        () => target.AbortAsync(
                            reservation,
                            new ZLinkAggregateFence(aggregateId, 1)),
                        async () =>
                        {
                            if (seal is not null)
                            {
                                await activation
                                    .CompleteRelocationReadyBeforeAbortAsync(
                                        seal,
                                        cleanupDeadline.Token)
                                    .ConfigureAwait(false);
                                if (!activation.AbortRelocation(seal))
                                    throw new ZLinkRelocationDataLostException(
                                        $"SPOT '{activation.SpotId}' could not restore its source admission seal.");
                            }
                            await RestoreSourceActorsAsync()
                                .ConfigureAwait(false);
                        })
                    .ConfigureAwait(false);
                await DiscardStagingAsync().ConfigureAwait(false);
            }

            async ValueTask CompleteCommittedAsync(
                CancellationToken completionToken)
            {
                var committedPublication = published
                    ?? throw new InvalidOperationException(
                        "Committed SPOT relocation lost its publication state.");
                if (seal is null)
                    throw new InvalidOperationException(
                        "Committed SPOT relocation lost its publication state.");
                if (!targetPublished)
                {
                    targetAuthorityOwnerGeneration =
                        await target.PublishAsync(
                            reservation,
                            committedPublication,
                            completionToken)
                        .ConfigureAwait(false);
                    targetPublished = true;
                }
                // Reconciliation can discover a target publication after the
                // original StageAsync acknowledgement was lost. Complete the
                // source interruption at the same boundary before cutover.
                interruption.Complete();
                committed = true;
                if (perActorShell)
                    await activation.PublishPerActorShellRelocationPlanAsync(
                            new ZLinkPerActorShellRelocationPlan(
                                reservation.TargetDescriptor.Rid,
                                reservation.TargetDescriptorLifecycleGeneration,
                                reservation.TargetOwner,
                                targetAuthorityOwnerGeneration,
                                deadline),
                            completionToken)
                        .ConfigureAwait(false);
                if (!messageFollowStarted)
                {
                    var spotParticipant = committedPublication.Envelope.Participants.Single(
                        static participant => participant.ObjectKind
                            is ZLinkPlacementObjectKind.UserSpot
                            or ZLinkPlacementObjectKind.InstanceSpot);
                    activation.BeginMessageFollow(
                        reservation.TargetDescriptor.Rid,
                        reservation.TargetDescriptorLifecycleGeneration,
                        spotParticipant.AuthorityOwnerGeneration,
                        targetAuthorityOwnerGeneration,
                        reservation.TargetOwner);
                    await StartActorMessageFollowAsync(
                            committedPublication,
                            completionToken)
                        .ConfigureAwait(false);
                    messageFollowStarted = true;
                }
                if (!sourceCommitted)
                {
                    if (!activation.CommitRelocation(
                            seal,
                            out var releasedHeld,
                            preserveActorExecution: perActorShell)
                        || !SameAcceptedWork(
                            heldAtCutoff,
                            releasedHeld))
                        throw new ZLinkRelocationDataLostException(
                            $"SPOT '{activation.SpotId}' accepted ingress changed after its durable root was prepared.");
                    committedHeld = releasedHeld;
                    sourceCommitted = true;
                }
                if (!committedHeldValidated)
                {
                    ZLinkSpotRetireTargetRuntime.ValidateHeldRecords(
                        committedHeld.Select(
                                static record =>
                                    new ZLinkSpotRetireHeldRecord(
                                        record.AcceptedSequence,
                                        record.Payload.ToArray()))
                            .ToArray());
                    committedHeldValidated = true;
                }
                if (!sourceCleanupCompleted)
                {
                    var cleanup = new ZLinkAggregateRelocationCoordinator(
                        authorityStore,
                        relocationStore);
                        var cleanupReconciliationDelay =
                        TimeSpan.FromMilliseconds(1);
                    while (!await cleanup.TryCompleteSourceCleanupAsync(
                               committedPublication,
                               activeReservation.TargetDescriptor,
                               activeReservation.TargetDescriptorLifecycleGeneration,
                               activeReservation.TargetOwner,
                               completionToken).ConfigureAwait(false))
                    {
                        var remaining = deadline - DateTimeOffset.UtcNow;
                        if (remaining <= TimeSpan.Zero)
                            throw new ZLinkFrameworkException(
                                ZLinkFrameworkErrorKind.DeadlineExceeded,
                                $"SPOT '{activation.SpotId}' relocation reply delivery did not complete before its fixed deadline.",
                                ZLinkRetryAdvice.RetryAfterBackoff);
                        await Task.Delay(
                                cleanupReconciliationDelay < remaining
                                    ? cleanupReconciliationDelay
                                    : remaining,
                                completionToken)
                            .ConfigureAwait(false);
                        cleanupReconciliationDelay = TimeSpan.FromMilliseconds(
                            Math.Min(
                                cleanupReconciliationDelay.TotalMilliseconds
                                * 2,
                                100));
                    }
                    sourceCleanupCompleted = true;
                }
                if (!targetCompletionDelivered)
                {
                    if (activeActorMessageFollowBacklog.Count != 0
                        && (await Task.WhenAll(activeActorMessageFollowBacklog)
                                .ConfigureAwait(false))
                            .Any(static delivered => !delivered))
                        throw new ZLinkRelocationDataLostException(
                            $"SPOT '{activation.SpotId}' could not deliver every pre-cutover Actor Message Follow frame.");
                    await target.RelayCommittedAsync(
                            activeReservation,
                            committedPublication,
                            committedHeld,
                            completionToken)
                        .ConfigureAwait(false);
                    targetCompletionDelivered = true;
                }
                if (!sourceCompleted)
                {
                    if (!perActorShell)
                        await activation.InvokeRelocationClosingAfterCommitAsync(
                                deadline)
                            .ConfigureAwait(false);
                    foreach (var capture in activeActorCaptures.Values)
                        await runtime.FinalizeMigratedActorSourceAsync(
                                capture.State,
                                capture.SourceActor)
                            .ConfigureAwait(false);
                    await completeSource(activation, completionToken)
                        .ConfigureAwait(false);
                    sourceCompleted = true;
                }
            }

            async ValueTask CompleteCommittedWithinDeadlineAsync()
            {
                using var completionDeadline = CreateDeadlineTokenSource(deadline);
                await CompleteCommittedAsync(completionDeadline.Token)
                    .ConfigureAwait(false);
            }

            async ValueTask StartActorMessageFollowAsync(
                ZLinkAggregateRelocationPublished committedPublication,
                CancellationToken completionToken)
            {
                var followTasks = activeActorCaptures.Values.Select(
                    async capture =>
                    {
                        var key =
                            ZLinkActorAuthorityPayloadCodec.AuthorityKey(
                                capture.State.ActorId);
                        var participant =
                            committedPublication.Envelope.Participants.Single(
                                candidate =>
                                    candidate.ObjectKind
                                    == ZLinkPlacementObjectKind.Actor
                                    && candidate.AuthorityKey == key);
                        var targetRead =
                            await authorityStore.ReadAuthorityAsync(
                                    key,
                                    completionToken)
                                .ConfigureAwait(false);
                        if (targetRead
                            is not ZLinkAuthorityReadResult.Found targetFound)
                            throw new ZLinkRelocationDataLostException(
                                $"Actor '{capture.State.ActorId}' target authority is unavailable after aggregate commit.");
                        var targetActor = new ZLinkBackendActorRef(
                            activeReservation.TargetDescriptor.Rid,
                            capture.State.ActorId,
                            capture.SourceActor.Generation);
                        var trailing = capture.State.Handoff
                            .CutoverCaptureToMessageFollow(
                                capture.CommitBoundary.Frames.Count,
                                capture.SourceActor,
                                targetActor,
                                activation.MeshName,
                                activeInventory.SourceNodeLifecycleGeneration,
                                activeReservation.TargetDescriptorLifecycleGeneration,
                                participant.AuthorityOwnerGeneration,
                                targetFound.Snapshot
                                    .AuthorityOwnerGeneration,
                                checked((ulong)activeInventory.SourceOwner
                                    .LeaseGeneration),
                                checked((ulong)activeReservation.TargetOwner
                                    .LeaseGeneration));
                        var backlog = runtime
                            .RelayStandaloneActorRelocationTrailing(
                                capture.State,
                                capture.SourceActor,
                                trailing);
                        capture.State.Handoff.CommitMessageFollow(
                            runtime.Registration.Locations.Options
                                .MessageFollowDuration);
                        return backlog;
                    });
                foreach (var backlog in await Task.WhenAll(followTasks)
                             .ConfigureAwait(false))
                    activeActorMessageFollowBacklog.AddRange(backlog);
            }

            async ValueTask RestoreSourceActorsAsync()
            {
                foreach (var capture in actorCaptures.Values.OrderBy(
                             static capture => capture.State.ActorId,
                             StringComparer.Ordinal))
                {
                    if (!capture.CaptureStarted)
                        continue;
                    await runtime.RestoreStandaloneActorRelocationSourceAsync(
                            capture.State)
                        .ConfigureAwait(false);
                }
            }

            async ValueTask DiscardStagingAsync()
            {
                if (staging is null)
                    return;
                var discard = staging;
                staging = null;
                await new ZLinkRelocationPublicationCoordinator(
                        authorityStore,
                        relocationStore)
                    .DiscardPreparedAsync(discard.Root)
                    .ConfigureAwait(false);
            }

        }
    }

    internal static async ValueTask ExecutePrecommitAbortAsync(
        Func<ValueTask>? abortDurableAggregate,
        Func<ValueTask> restoreSessionRoutes,
        Func<ValueTask> cleanupTarget,
        Func<ValueTask> resumeSource)
    {
        ArgumentNullException.ThrowIfNull(restoreSessionRoutes);
        ArgumentNullException.ThrowIfNull(cleanupTarget);
        ArgumentNullException.ThrowIfNull(resumeSource);
        if (abortDurableAggregate is not null)
            await abortDurableAggregate().ConfigureAwait(false);
        Exception? routeRestoreFailure = null;
        try
        {
            await restoreSessionRoutes().ConfigureAwait(false);
        }
        catch (Exception exception)
        {
            routeRestoreFailure = exception;
        }
        try
        {
            await cleanupTarget().ConfigureAwait(false);
        }
        catch when (routeRestoreFailure is not null)
        {
            // Restoring the source route remains the primary correctness
            // failure. Target cleanup is still attempted so source-side wire
            // retries and target reservations do not remain active.
        }
        if (routeRestoreFailure is not null)
            ExceptionDispatchInfo.Capture(routeRestoreFailure).Throw();
        await resumeSource().ConfigureAwait(false);
    }

    private static int CountSnapshotParticipants(
        ZLinkSpotActivation activation,
        IReadOnlyList<string> actorIds)
    {
        var spot = activation.ResolveSpotRelocationRegistrationForRetire();
        if (spot.PolicyKind == 0)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Rejected,
                $"Relocation is disabled for SPOT '{activation.SpotId}'.");
        var count = spot.PolicyKind == 2 ? 1 : 0;
        foreach (var actorId in actorIds)
        {
            var actor =
                activation.ResolveActorRelocationRegistrationForRetire(actorId);
            if (actor.PolicyKind == 0)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Rejected,
                    $"Relocation is disabled for Actor '{actorId}'.");
            if (actor.PolicyKind == 2)
                count++;
        }
        return count;
    }

    internal static long CalculatePayloadReservation(
        int snapshotParticipantCount,
        IReadOnlyList<ZLinkAuthorityKey> participantKeys,
        IReadOnlyList<ZLinkAcceptedWorkRecord> captured,
        IReadOnlyList<ZLinkRelocationLogicalTimer> timers)
    {
        if (snapshotParticipantCount < 0)
            throw new ArgumentOutOfRangeException(
                nameof(snapshotParticipantCount));
        ArgumentNullException.ThrowIfNull(participantKeys);
        ArgumentNullException.ThrowIfNull(captured);
        ArgumentNullException.ThrowIfNull(timers);
        if (participantKeys.Count < 1)
            throw new ArgumentOutOfRangeException(nameof(participantKeys));

        // This is the exact encoded size of the framework-owned portion of
        // ZLinkRelocationEnvelopeCodec: header, participant manifest, accepted
        // journal and logical timers. Application state remains empty here;
        // Snapshot adapters reserve their documented maximum separately.
        long frameworkBytes = EnvelopeHeaderBytes;
        foreach (var key in participantKeys)
        {
            var keyBytes =
                System.Text.Encoding.UTF8.GetByteCount(key.Value);
            frameworkBytes = checked(
                frameworkBytes
                + sizeof(ushort) + keyBytes
                + sizeof(byte)
                + sizeof(ulong) + sizeof(ulong)
                + sizeof(int) // application state length
                + sizeof(int) // accepted job count
                + sizeof(int) // logical timer count
                + sizeof(int) // recovery payload length
                + sizeof(int)); // completion payload length
        }
        foreach (var record in captured)
            frameworkBytes = checked(
                frameworkBytes
                + sizeof(ulong)
                + sizeof(int)
                + record.Payload.Length);
        foreach (var timer in timers)
            frameworkBytes = checked(
                frameworkBytes
                + sizeof(ushort)
                + System.Text.Encoding.UTF8.GetByteCount(timer.TimerId)
                + sizeof(long)
                + sizeof(long)
                + sizeof(int)
                + timer.Payload.Length);
        return checked(
            frameworkBytes
            + ZLinkSpotRetireCompletionMarker.CreatePending().LongLength
            + SourceIngressHoldReservationBytes
            + (long)ZLinkCanonicalParticipantRecoveryCodec
                .MaximumEncodedBytesWithEmptyMembership
              * participantKeys.Count
            + SnapshotReservationBytes * snapshotParticipantCount);
    }

    internal static void ValidateSnapshotPayloadSize(
        ReadOnlyMemory<byte> payload)
    {
        if (payload.Length > SnapshotReservationBytes)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Rejected,
                "A relocation Snapshot payload cannot exceed 64 MiB.");
    }

    private async ValueTask<ZLinkAggregateRelocationParticipant[]>
        BuildParticipantsAsync(
            ZLinkSpotActivation activation,
            ZLinkSpotRelocationSeal seal,
            ZLinkSpotRelocationApplicationState application,
            IReadOnlyList<string> actorIds,
        Guid aggregateId,
        IReadOnlyDictionary<string, ZLinkRemoteActorBoundSessionRoute>
            sealedSessionRoutes,
        IReadOnlyDictionary<string, SourceActorCapture> actorCaptures,
        IReadOnlyList<ZLinkObjectCapability> requiredCapabilities,
        bool includeSpotTimers,
        CancellationToken cancellationToken)
    {
        var participants = new List<ZLinkAggregateRelocationParticipant>(
            checked(actorIds.Count + 1));
        var spotKey = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(
            activation.SpotId);
        var spotTask = ReadOwnedAsync(
                spotKey,
                cancellationToken)
            .AsTask();
        var capturedJobsTask = ResolveAcceptedJobsAsync(
                activation.MeshName,
                seal.QueueSeal.QueueSeal.Captured,
                cancellationToken)
            .AsTask();
        var actorAuthoritiesTask = ReadActorAuthoritiesAsync(
                actorIds,
                cancellationToken)
            .AsTask();
        var spot = await spotTask.ConfigureAwait(false);
        var capturedJobs = await capturedJobsTask.ConfigureAwait(false);
        var actorAuthorities = await actorAuthoritiesTask
            .ConfigureAwait(false);
        var spotKind = activation.Spot is IZLinkInstanceSpot
            ? ZLinkPlacementObjectKind.InstanceSpot
            : ZLinkPlacementObjectKind.UserSpot;
        var spotStableType = spotKind == ZLinkPlacementObjectKind.InstanceSpot
            ? ZLinkInstanceSpotAuthorityPayloadCodec.TryDecode(
                    spot.Payload.Span, out var instanceAuthority)
                ? instanceAuthority.StableType
                : throw new ZLinkRelocationDataLostException(
                    "The Instance SPOT authority payload is invalid.")
            : ZLinkUserSpotAuthorityPayloadCodec.TryDecode(
                    spot.Payload.Span, out var userAuthority)
                ? userAuthority.StableType
                : throw new ZLinkRelocationDataLostException(
                    "The User SPOT authority payload is invalid.");
        var spotRecovery = ZLinkCanonicalParticipantRecoveryCodec.Encode(
            new ZLinkCanonicalParticipantRecovery(
                spotKey,
                spotKind,
                spot.ObjectGeneration,
                spot.AuthorityOwnerGeneration,
                spot.StoreVersion,
                spotStableType,
                spot.Payload,
                ReadOnlyMemory<byte>.Empty,
                MaintenancePolicy: requiredCapabilities.Single(capability =>
                    capability.ObjectKind == spotKind
                    && StringComparer.Ordinal.Equals(
                        capability.StableType,
                        spotStableType)).Policy));
        participants.Add(new ZLinkAggregateRelocationParticipant(
            new ZLinkRelocationParticipantEnvelope(
                spotKey,
                spotKind,
                spot.ObjectGeneration,
                spot.AuthorityOwnerGeneration,
                application.SpotState,
                capturedJobs,
                includeSpotTimers
                    ? seal.LogicalTimers
                    : [],
                RecoveryPayload: spotRecovery,
                CompletionPayload:
                    ZLinkSpotRetireCompletionMarker.CreatePending()),
            spot.StoreVersion,
            ZLinkAuthorityGenerationTransition.NewOwner,
            spot.Payload,
            ReadOnlyMemory<byte>.Empty));

        for (var index = 0; index < actorIds.Count; index++)
        {
            var actorId = actorIds[index];
            var key = ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId);
            var actor = actorAuthorities[index];
            var boundRoute = sealedSessionRoutes.TryGetValue(
                actorId,
                out var sealedRoute)
                ? sealedRoute
                : default;
            var relocationAuthority =
                ZLinkActorRelocationAuthorityPayloadCodec.Encode(
                    new ZLinkActorRelocationAuthorityPayload(
                        aggregateId,
                        ZLinkActorRelocationAuthorityPhase.Activated,
                        boundRoute,
                        actor.Payload));
            if (!ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
                    actor.Payload.Span, out var actorAuthority))
                throw new ZLinkRelocationDataLostException(
                    $"Actor authority '{key.Value}' is invalid.");
            var actorRecovery = ZLinkCanonicalParticipantRecoveryCodec.Encode(
                new ZLinkCanonicalParticipantRecovery(
                    key,
                    ZLinkPlacementObjectKind.Actor,
                    actor.ObjectGeneration,
                    actor.AuthorityOwnerGeneration,
                    actor.StoreVersion,
                    actorAuthority.StableType,
                    relocationAuthority,
                    ReadOnlyMemory<byte>.Empty,
                    MaintenancePolicy: requiredCapabilities.Single(capability =>
                        capability.ObjectKind
                        == ZLinkPlacementObjectKind.Actor
                        && StringComparer.Ordinal.Equals(
                            capability.StableType,
                            actorAuthority.StableType)).Policy));
            participants.Add(new ZLinkAggregateRelocationParticipant(
                new ZLinkRelocationParticipantEnvelope(
                    key,
                    ZLinkPlacementObjectKind.Actor,
                    actor.ObjectGeneration,
                    actor.AuthorityOwnerGeneration,
                    application.ActorStates[actorId],
                    CreateActorAcceptedJobs(
                        actorCaptures[actorId],
                        actor.ObjectGeneration),
                    [],
                    RecoveryPayload: actorRecovery)
                {
                    AcceptedBoundary = actorCaptures[actorId]
                        .CommitBoundary.AcceptedHighWater
                },
                actor.StoreVersion,
                ZLinkAuthorityGenerationTransition.NewOwner,
                relocationAuthority,
                ReadOnlyMemory<byte>.Empty));
        }
        return participants.ToArray();
    }

    private static IReadOnlyList<ZLinkRelocationQueuedJob>
        CreateActorAcceptedJobs(
            SourceActorCapture capture,
            ulong objectGeneration)
    {
        if (capture.SourceActor.Generation != objectGeneration)
            throw new ZLinkRelocationDataLostException(
                $"Actor '{capture.State.ActorId}' changed ObjectGeneration during aggregate capture.");
        return ZLinkStandaloneActorRelocationRuntime.CreateAcceptedRecords(
                capture.CommitBoundary.Frames)
            .OrderBy(static accepted => accepted.Frame.ArrivalIndex)
            .Select((accepted, index) => new ZLinkRelocationQueuedJob(
                checked((ulong)index + 1),
                ZLinkCanonicalActorAcceptedJournal.Encode(
                    accepted,
                    capture.SourceActor)))
            .ToArray();
    }

    private async ValueTask<ZLinkAuthoritySnapshot[]>
        ReadActorAuthoritiesAsync(
            IReadOnlyList<string> actorIds,
            CancellationToken cancellationToken)
    {
        var authorities = new ZLinkAuthoritySnapshot[actorIds.Count];
        for (var first = 0;
             first < actorIds.Count;
             first += MaxConcurrentParticipantIo)
        {
            var count = Math.Min(
                MaxConcurrentParticipantIo,
                actorIds.Count - first);
            await Task.WhenAll(
                    Enumerable.Range(first, count)
                        .Select(async index =>
                        {
                            var key =
                                ZLinkActorAuthorityPayloadCodec.AuthorityKey(
                                    actorIds[index]);
                            authorities[index] = await ReadOwnedAsync(
                                    key,
                                    cancellationToken)
                                .ConfigureAwait(false);
                        }))
                .ConfigureAwait(false);
        }
        return authorities;
    }

    private async ValueTask<ZLinkAggregateRelocationParticipant[]>
        AppendHeldIngressAsync(
        string meshName,
        IReadOnlyList<ZLinkAggregateRelocationParticipant> participants,
        IReadOnlyList<ZLinkAcceptedWorkRecord> held,
        CancellationToken cancellationToken)
    {
        if (held.Count == 0)
            return participants.ToArray();
        var spot = participants.Single(static participant =>
            participant.Envelope.ObjectKind
                is ZLinkPlacementObjectKind.UserSpot
                or ZLinkPlacementObjectKind.InstanceSpot);
        var heldJobs = await ResolveAcceptedJobsAsync(
                meshName,
                held,
                cancellationToken)
            .ConfigureAwait(false);
        var accepted = spot.Envelope.AcceptedJobs
            .Concat(heldJobs)
            .OrderBy(static record => record.AcceptedSequence)
            .ToArray();
        for (var index = 1; index < accepted.Length; index++)
            if (accepted[index - 1].AcceptedSequence
                >= accepted[index].AcceptedSequence)
                throw new ZLinkRelocationDataLostException(
                    "SPOT relocation accepted journal contains duplicate or out-of-order sequence values.");
        return participants.Select(participant =>
                participant == spot
                    ? participant with
                    {
                        Envelope = participant.Envelope with
                        {
                            AcceptedJobs = accepted
                        }
                    }
                    : participant)
            .ToArray();
    }

    private ValueTask<IReadOnlyList<ZLinkRelocationQueuedJob>>
        ResolveAcceptedJobsAsync(
            string meshName,
            IReadOnlyList<ZLinkAcceptedWorkRecord> records,
            CancellationToken cancellationToken)
    {
        _ = meshName;
        cancellationToken.ThrowIfCancellationRequested();
        if (records.Count == 0)
            return ValueTask.FromResult<IReadOnlyList<ZLinkRelocationQueuedJob>>(
                []);
        return ValueTask.FromResult(ResolveFrozenAcceptedJobs(records));
    }

    internal static IReadOnlyList<ZLinkRelocationQueuedJob>
        ResolveFrozenAcceptedJobs(
            IReadOnlyList<ZLinkAcceptedWorkRecord> records)
    {
        return records.Select(record =>
        {
            var journal = ZLinkSpotAcceptedJournal.Decode(record.Payload.Span);
            if (journal.RequestSource is not { } source)
                throw new ZLinkRelocationDataLostException(
                    "Accepted request source fence was not frozen at ingress.");
            return new ZLinkRelocationQueuedJob(
                record.AcceptedSequence,
                record.Payload)
            {
                RequestSource = new ZLinkCanonicalRequestSourceFence(
                    source.OwnerId,
                    source.LeaseGeneration,
                    source.NodeRid.ToHex(),
                    source.NodeGeneration)
            };
        }).OrderBy(static job => job.AcceptedSequence).ToArray();
    }

    private async ValueTask<ZLinkMeshNodeDescriptor> ReadSourceDescriptorAsync(
        string meshName,
        RoutingId sourceNodeRid,
        ulong sourceNodeGeneration,
        ZLinkLocationOwnerToken sourceOwner,
        CancellationToken cancellationToken)
    {
        var descriptors = await authorityStore.ListAllMeshNodesAsync(
                meshName,
                cancellationToken)
            .ConfigureAwait(false);
        return descriptors.SingleOrDefault(descriptor =>
                   descriptor.Rid == sourceNodeRid
                   && descriptor.LifecycleGeneration == sourceNodeGeneration
                   && descriptor.OwnerId == sourceOwner.OwnerId
                   && descriptor.LeaseGeneration == sourceOwner.LeaseGeneration)
               ?? throw new ZLinkRelocationDataLostException(
                   "Relocation source descriptor fence changed before canonical capture.");
    }

    private static bool SameAcceptedWork(
        IReadOnlyList<ZLinkAcceptedWorkRecord> expected,
        IReadOnlyList<ZLinkAcceptedWorkRecord> actual)
    {
        if (expected.Count != actual.Count)
            return false;
        for (var index = 0; index < expected.Count; index++)
            if (expected[index].AcceptedSequence
                    != actual[index].AcceptedSequence
                || !expected[index].Payload.Span.SequenceEqual(
                    actual[index].Payload.Span))
                return false;
        return true;
    }

    private async ValueTask<ZLinkAuthoritySnapshot> ReadOwnedAsync(
        ZLinkAuthorityKey key,
        CancellationToken cancellationToken)
    {
        var read = await authorityStore.ReadAuthorityAsync(key, cancellationToken)
            .ConfigureAwait(false);
        return read is ZLinkAuthorityReadResult.Found found
            ? found.Snapshot
            : throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.NotFound,
                $"Relocation authority '{key.Value}' is not Ready.");
    }

    private sealed class SourceActorCapture(
        ZLinkActorRuntimeState state,
        ZLinkBackendActorRef sourceActor)
    {
        internal ZLinkActorRuntimeState State { get; } = state;
        internal ZLinkBackendActorRef SourceActor { get; } = sourceActor;
        internal bool CaptureStarted { get; set; }
        internal ZLinkActorHandoffCommitBoundary CommitBoundary { get; set; }
    }
}
