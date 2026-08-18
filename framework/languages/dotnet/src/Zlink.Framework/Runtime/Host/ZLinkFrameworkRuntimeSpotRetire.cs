using System.Collections.Concurrent;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime.Identifiers;

namespace Zlink.Framework.Runtime.Host;

internal sealed partial class ZLinkFrameworkRuntime
{
    internal async ValueTask<ZLinkRelocationReplyAckState> RelayRelocationReplyAsync(
        RoutingId sourceNodeRid,
        ZLinkServiceWireCodec.ReplyRelayRecord relay,
        ZLinkServiceWireCodec.RequestSourceFence expectedSource,
        IReadOnlyList<byte[]> payload,
        CancellationToken cancellationToken)
    {
        var node = GetSpotNodeRuntime(relay.Coordinator.NodeRid);
        var messages = payload.Select(static part => Message.From(part)).ToArray();
        ZLinkServiceWireCodec.ReplyRelayAckRecord ack;
        try
        {
            ack = await node.RelayRelocationReplyAsync(
                    sourceNodeRid,
                    relay,
                    expectedSource,
                    messages,
                    node.Registration.DefaultRequestTimeout
                    ?? Registration.DefaultRequestTimeout,
                    cancellationToken)
                .ConfigureAwait(false);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(messages);
        }
        if (ack.RequestSource != expectedSource)
            return ZLinkRelocationReplyAckState.NotAcknowledged;
        return ack.Status == 2
            ? ZLinkRelocationReplyAckState.AlreadyTerminal
            : ZLinkRelocationReplyAckState.TerminalReceived;
    }

    internal async ValueTask<TargetStage> StageInboundSpotAggregateAsync(
        ZLinkCanonicalSpotStageContext request,
        ZLinkRelocationEnvelope envelope,
        ulong reservedTargetAuthorityOwnerGeneration,
        CancellationToken cancellationToken,
        bool stagedBeforeCutover = false,
        ReadOnlyMemory<byte> basePayload = default,
        bool hasBase = false)
    {
        var targetRid = RoutingId.FromHex(request.TargetNodeRid);
        var node = GetSpotNodeRuntime(targetRid);
        var spotParticipant = envelope.Participants.Single(
            static participant => participant.ObjectKind
                is ZLinkPlacementObjectKind.UserSpot
                or ZLinkPlacementObjectKind.InstanceSpot);
        if (spotParticipant.ObjectGeneration == 0
            || request.SpotId.Length == 0)
            throw new InvalidDataException(
                "The inbound SPOT relocation target does not match its envelope.");

        var targetOwnerGeneration =
            await ResolveInboundTargetAuthorityOwnerGenerationAsync(
                    spotParticipant,
                    stagedBeforeCutover
                        ? checked(spotParticipant.AuthorityOwnerGeneration + 1)
                        : reservedTargetAuthorityOwnerGeneration,
                    cancellationToken)
                .ConfigureAwait(false);
        async ValueTask<PreparedReservedSpot> PrepareTargetSpotAsync()
        {
            if (request.InstanceSpot)
            {
                return await node.Catalog.PrepareInstanceReservedAsync(
                        request.StableType,
                        request.SpotId,
                        spotParticipant.ObjectGeneration,
                        targetOwnerGeneration,
                        cancellationToken,
                        restoreLogicalTimers: true)
                    .ConfigureAwait(false);
            }

            if (!node.Registration.SpotRelocations.TryGetValue(
                    request.StableType,
                    out var spotRegistration))
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.TypeMismatch,
                    $"User SPOT type '{request.StableType}' is not registered on target node.");
            return await node.Catalog.PrepareReservedAsync(
                    spotRegistration.InstanceType,
                    request.SpotId,
                    spotParticipant.ObjectGeneration,
                    targetOwnerGeneration,
                    ZLinkMessage.Empty,
                    cancellationToken,
                    invokeCreate: false)
                .ConfigureAwait(false);
        }

        var preparedSpot = await PrepareTargetSpotAsync().ConfigureAwait(false);

        var actorStates = new List<ZLinkActorRuntimeState>();
        var actorTargetAuthorityOwnerGenerations =
            new Dictionary<ZLinkActorId, ulong>();
        var boundActorIds =
            new ConcurrentDictionary<ZLinkActorId, byte>();
        var stagedActorStates =
            new ConcurrentDictionary<ZLinkActorId, ZLinkActorRuntimeState>();
        ZLinkSpotRelocationSeal? targetAdmissionSeal = null;
        try
        {
            preparedSpot = await RestorePreparedSpotStateAsync(
                    preparedSpot,
                    _ => PrepareTargetSpotAsync(),
                    node.Catalog.DiscardReservedAsync,
                    fresh => preparedSpot = fresh,
                    spotParticipant.ApplicationState,
                    basePayload,
                    hasBase,
                    cancellationToken)
                .ConfigureAwait(false);

            var descriptors = request.Actors.ToDictionary(
                static actor => actor.ActorId,
                StringComparer.Ordinal);
            var actorParticipants = envelope.Participants.Where(
                    static participant =>
                        participant.ObjectKind
                        == ZLinkPlacementObjectKind.Actor)
                .ToArray();
            var restoredActors =
                new ConcurrentBag<(ZLinkActorRuntimeState State,
                    ulong TargetAuthorityOwnerGeneration)>();
            using var restoreConcurrency = new SemaphoreSlim(64);
            await Task.WhenAll(actorParticipants.Select(
                async participant =>
                {
                    await restoreConcurrency.WaitAsync(cancellationToken)
                        .ConfigureAwait(false);
                    try
                    {
                        var descriptor = descriptors.Values.Single(candidate =>
                            ZLinkActorAuthorityPayloadCodec.AuthorityKey(
                                candidate.ActorId)
                            == participant.AuthorityKey);
                        var hasActor =
                            ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
                                descriptor.AuthorityPayload,
                                out var authority);
                        var hasRelocation =
                            ZLinkActorRelocationAuthorityPayloadCodec.TryDecode(
                                descriptor.AuthorityPayload,
                                out var relocationAuthority);
                        var hasRegistration =
                            node.Registration.ActorRelocations.TryGetValue(
                                hasActor ? authority.StableType : string.Empty,
                                out var relocation);
                        hasRegistration &= hasActor;
                        var publishedSteadyRecovery =
                            reservedTargetAuthorityOwnerGeneration == 0
                            && !hasRelocation
                            && hasActor
                            && authority.State == ZLinkActorAuthorityState.Ready;
                        if (!hasActor
                            || !publishedSteadyRecovery
                            && (!hasRelocation
                                || relocationAuthority.RelocationId
                                != envelope.AggregateId
                                || relocationAuthority.Phase is not (
                                    ZLinkActorRelocationAuthorityPhase.Activated
                                    or ZLinkActorRelocationAuthorityPhase.Steady))
                            || !hasRegistration)
                        {
                            throw new InvalidDataException(
                                $"Actor participant '{participant.AuthorityKey.Value}' cannot be restored on the target.");
                        }

                        var actorTargetAuthorityOwnerGeneration =
                            await ResolveInboundTargetAuthorityOwnerGenerationAsync(
                                    participant,
                                    stagedBeforeCutover
                                        ? checked(participant
                                            .AuthorityOwnerGeneration + 1)
                                        : 0,
                                    cancellationToken)
                                .ConfigureAwait(false);
                        var creation = await _actorSessionManager
                            .RelocateAndBindActorAsync(
                                authority.ActorId,
                                authority.StableType,
                                relocation!,
                                participant.ApplicationState,
                                participant.ObjectGeneration,
                                actorTargetAuthorityOwnerGeneration,
                                ZLinkActorClaimMode.StagedRelocation,
                                publishActorRef: false,
                                cancellationToken)
                            .ConfigureAwait(false);
                        var actorKey = ZLinkActorId.FromBoundary(
                            authority.ActorId,
                            nameof(authority.ActorId));
                        boundActorIds.TryAdd(actorKey, 0);
                        var actorState = GetOrCreateActorState(authority.ActorId);
                        stagedActorStates.TryAdd(
                            actorKey,
                            actorState);
                        var boundRoute = hasRelocation
                            ? relocationAuthority.BoundSessionRoute
                            : default;
                        var wireContext = default(
                            ZLinkSessionRelocationContext);
                        if (boundRoute.IsBound)
                        {
                            if (ZLinkCanonicalRelocationAuthorityStateCodec
                                         .TryRead(
                                             descriptor.AuthorityPayload,
                                             out var canonical))
                            {
                                wireContext = new ZLinkSessionRelocationContext(
                                    new ZLinkServiceWireCodec.RelocationWireId(
                                        canonical.RelocationHigh,
                                        canonical.RelocationLow),
                                    new ZLinkServiceWireCodec
                                        .RelocationCoordinatorFence(
                                            canonical.State.CoordinatorOwnerId,
                                            canonical.State
                                                .CoordinatorLeaseGeneration,
                                            RoutingId.FromHex(
                                                canonical.State
                                                    .CoordinatorNodeRid),
                                            canonical.State
                                                .CoordinatorNodeGeneration,
                                            canonical.State
                                                .CoordinatorExpectedAuthorityStoreVersion));
                            }
                        }
                        actorState.StageRelocationSessionRoute(
                            envelope.AggregateId.ToString("N"),
                            boundRoute,
                            wireContext);
                        var acceptedFrames =
                            ZLinkStandaloneActorRelocationRuntime
                                .DecodeAcceptedRecords(
                                    participant.AcceptedJobs)
                                .Select(static accepted => accepted.Frame)
                                .ToArray();
                        actorState.Handoff.BeginCanonicalMaintenanceImport(
                            envelope.AggregateId.ToString("N"),
                            acceptedFrames);
                        await preparedSpot.Activation
                            .PrepareTransferredActorJoinAndReplayAsync(
                                creation.Actor,
                                actorState,
                                cancellationToken)
                            .ConfigureAwait(false);
                        restoredActors.Add((
                            actorState,
                            actorTargetAuthorityOwnerGeneration));
                    }
                    finally
                    {
                        restoreConcurrency.Release();
                    }
                })).ConfigureAwait(false);
            foreach (var restored in restoredActors.OrderBy(
                         static actor => actor.State.ActorId,
                         StringComparer.Ordinal))
            {
                actorTargetAuthorityOwnerGenerations.Add(
                    restored.State.RuntimeActorId,
                    restored.TargetAuthorityOwnerGeneration);
                actorStates.Add(restored.State);
            }

            if (!preparedSpot.Activation.TrySealRelocation(
                    out targetAdmissionSeal))
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    $"Target SPOT '{request.SpotId}' could not seal staging admission.",
                    ZLinkRetryAdvice.RetryAfterBackoff);
            preparedSpot.Activation.RestoreLogicalTimers(
                spotParticipant.LogicalTimers);
            return new TargetStage(
                node,
                preparedSpot,
                envelope,
                actorStates,
                request.StableType,
                request.MeshName,
                RoutingId.FromHex(request.SourceNodeRid),
                request.SourceNodeLifecycleGeneration,
                new ZLinkLocationOwnerToken(
                    request.SourceOwnerId,
                    request.SourceOwnerLeaseGeneration),
                request.CoordinatorExpectedAuthorityStoreVersion,
                request.RelocationReference,
                request.RelocationChecksum,
                DateTimeOffset.UtcNow
                + ZLinkSpotRetireTargetRuntime.StageRetention,
                ZLinkSpotRetireTargetRuntime.ComputeStageRequestDigest(
                    request),
                targetAdmissionSeal
                ?? throw new InvalidOperationException(
                    "Target staging admission seal was not created."),
                actorTargetAuthorityOwnerGenerations,
                spotParticipant.AuthorityOwnerGeneration,
                targetOwnerGeneration,
                request.MeshName,
                request.TargetNodeLifecycleGeneration,
                request.TargetOwnerLeaseGeneration,
                request.TargetAttemptGeneration);
        }
        catch
        {
            if (targetAdmissionSeal is not null)
                _ = preparedSpot.Activation.AbortRelocation(
                    targetAdmissionSeal);
            foreach (var actorId in boundActorIds.Keys)
            {
                try
                {
                    if (stagedActorStates.TryGetValue(
                            actorId,
                            out var actorState))
                    {
                        actorState.Handoff.AbortImport(
                            envelope.AggregateId.ToString("N"));
                    }
                    await _actorSessionManager.RollbackTransferredActorAsync(
                            actorId.Value,
                            CancellationToken.None)
                        .ConfigureAwait(false);
                }
                catch
                {
                }
            }
            await node.Catalog.DiscardReservedAsync(preparedSpot)
                .ConfigureAwait(false);
            throw;
        }
    }

    /// <summary>
    /// Restores an unpublished relocation target. Spec 15 requires a single
    /// base/delta retry on a wholly fresh Spot activation: disposing the
    /// prepared activation also disposes its native reservation, then the
    /// catalog constructs and binds a new activation for the same identity.
    /// Existing (published) activations are never replaced here.
    /// </summary>
    internal static async ValueTask<PreparedReservedSpot>
        RestorePreparedSpotStateAsync(
            PreparedReservedSpot preparedSpot,
            Func<CancellationToken, ValueTask<PreparedReservedSpot>>
                prepareFreshAsync,
            Func<PreparedReservedSpot, ValueTask> discardAsync,
            Action<PreparedReservedSpot> freshPrepared,
            ReadOnlyMemory<byte> state,
            ReadOnlyMemory<byte> basePayload,
            bool hasBase,
            CancellationToken cancellationToken)
    {
        try
        {
            await preparedSpot.Activation.RestoreSpotRelocationStateAsync(
                    state,
                    basePayload,
                    hasBase,
                    cancellationToken)
                .ConfigureAwait(false);
            return preparedSpot;
        }
        catch when (hasBase
            && !preparedSpot.Existing
            && !cancellationToken.IsCancellationRequested)
        {
            await discardAsync(preparedSpot).ConfigureAwait(false);
            preparedSpot = await prepareFreshAsync(cancellationToken)
                .ConfigureAwait(false);
            // Publish the replacement to the staging owner before its
            // restore begins, so its normal failure cleanup owns this
            // attempt if the second restore fails.
            freshPrepared(preparedSpot);
            await preparedSpot.Activation.RestoreSpotRelocationStateAsync(
                    state,
                    basePayload,
                    hasBase,
                    cancellationToken)
                .ConfigureAwait(false);
            return preparedSpot;
        }
    }

    private async ValueTask<ulong>
        ResolveInboundTargetAuthorityOwnerGenerationAsync(
            ZLinkRelocationParticipantEnvelope participant,
            ulong reservedTargetAuthorityOwnerGeneration,
            CancellationToken cancellationToken)
    {
        if (reservedTargetAuthorityOwnerGeneration == 0)
        {
            var published =
                await ReadPublishedTargetAuthorityOwnerGenerationAsync(
                        participant.AuthorityKey,
                        cancellationToken)
                    .ConfigureAwait(false);
            if (published is 0 or > long.MaxValue
                || participant.AuthorityOwnerGeneration != published)
                throw new ZLinkRelocationDataLostException(
                    $"Published target authority generation for '{participant.AuthorityKey.Value}' changed during recovery.");
            return published;
        }

        var targetGeneration = reservedTargetAuthorityOwnerGeneration > 0
            ? reservedTargetAuthorityOwnerGeneration
            : await ReadPublishedTargetAuthorityOwnerGenerationAsync(
                    participant.AuthorityKey,
                    cancellationToken)
                .ConfigureAwait(false);
        if (participant.AuthorityOwnerGeneration is 0 or > long.MaxValue
            || targetGeneration is 0 or > long.MaxValue
            || targetGeneration <= participant.AuthorityOwnerGeneration)
            throw new ZLinkRelocationDataLostException(
                $"Target authority generation for '{participant.AuthorityKey.Value}' is not newer than its source generation.");
        return targetGeneration;
    }

    private async ValueTask<ulong>
        ReadPublishedTargetAuthorityOwnerGenerationAsync(
            ZLinkAuthorityKey key,
            CancellationToken cancellationToken)
    {
        var store = Registration.Locations.ResolveStore()
                    ?? throw new ZLinkConfigurationException(
                        "Inbound relocation recovery requires a Location Store.");
        return await store.ReadAuthorityAsync(key, cancellationToken)
            .ConfigureAwait(false) switch
        {
            ZLinkAuthorityReadResult.Found found =>
                found.Snapshot.AuthorityOwnerGeneration,
            _ => throw new ZLinkRelocationDataLostException(
                $"Published target authority '{key.Value}' is missing.")
        };
    }

    internal async ValueTask PublishInboundSpotAggregateAsync(
        TargetStage stage,
        Func<CancellationToken, ValueTask>? normalizeAuthority,
        CancellationToken cancellationToken)
    {
        await stage.PublishGate.WaitAsync(cancellationToken)
            .ConfigureAwait(false);
        var spotParticipant = stage.Envelope.Participants.Single(
            static participant => participant.ObjectKind
                is ZLinkPlacementObjectKind.UserSpot
                or ZLinkPlacementObjectKind.InstanceSpot);
        try
        {
            if (Volatile.Read(ref stage.Published) == 0)
            {
                await PrepareInboundSpotAggregateCoreAsync(
                        stage,
                        spotParticipant,
                        cancellationToken)
                    .ConfigureAwait(false);
                var handoffId = stage.Envelope.AggregateId.ToString("N");
                foreach (var actorState in stage.ActorStates)
                {
                    var actorRef = actorState.NativeActorRef
                                   ?? throw new ZLinkFrameworkException(
                                       ZLinkFrameworkErrorKind.NotFound,
                                       $"Actor '{actorState.ActorId}' has no staged target reference.");
                    actorState.MarkRelocationSessionAuthorityCommitted(
                        handoffId,
                        actorRef,
                        stage.TargetActorAuthorityOwnerGeneration(
                            actorState.RuntimeActorId),
                        ZLinkMeshName.FromBoundary(
                            stage.TargetMeshName,
                            nameof(stage.TargetMeshName)),
                        stage.TargetNodeLifecycleGeneration,
                        stage.TargetOwnerLeaseGeneration);
                }

                if (Volatile.Read(ref stage.RelocationReadyDelivered) == 0)
                {
                    await stage.Spot.Activation
                        .InvokeTargetRelocationReadyCompletedAsync(
                            stage.TargetAdmissionSeal,
                            cancellationToken)
                        .ConfigureAwait(false);
                    Volatile.Write(ref stage.RelocationReadyDelivered, 1);
                }

                await PublishCatalogBeforeNormalizationAsync(
                        stage,
                        () =>
                        {
                            foreach (var actorState in stage.ActorStates)
                                _actorSessionManager.PublishReservedActor(
                                    actorState.ActorId);
                            stage.Node.Catalog.PublishRelocatedReserved(
                                stage.Spot);
                        },
                        normalizeAuthority is null
                            ? null
                            : () => normalizeAuthority(cancellationToken))
                    .ConfigureAwait(false);
                Volatile.Write(ref stage.Published, 1);
            }
            else if (normalizeAuthority is not null)
            {
                await normalizeAuthority(cancellationToken)
                    .ConfigureAwait(false);
            }

            // Published means queue publication is complete: restore, replay,
            // catalog, and the ready callback. Admission opens from the
            // publish path and session route convergence remains asynchronous.
        }
        finally
        {
            stage.PublishGate.Release();
        }
    }

    internal static void OpenTargetAdmissionOnce(
        TargetStage stage,
        Func<bool> openAdmission)
    {
        ArgumentNullException.ThrowIfNull(stage);
        ArgumentNullException.ThrowIfNull(openAdmission);
        if (Volatile.Read(ref stage.AuthorityPublished) == 0
            || Volatile.Read(ref stage.Published) == 0)
            throw new InvalidOperationException(
                "Target admission cannot open before authority and queue publication.");
        if (Volatile.Read(ref stage.AdmissionOpened) != 0)
            return;
        if (!openAdmission())
            throw new InvalidOperationException(
                $"Target SPOT '{stage.Spot.Activation.SpotId}' lost its staging admission seal.");
        Volatile.Write(ref stage.AdmissionOpened, 1);
    }

    private const int SessionRouteConvergenceAttempts = 5;

    private static readonly TimeSpan SessionRouteConvergenceRetryDelay =
        TimeSpan.FromMilliseconds(50);

    internal void ScheduleRelocationSessionRouteConvergence(TargetStage stage)
    {
        if (Volatile.Read(ref stage.AuthorityPublished) == 0
            || Volatile.Read(ref stage.Published) == 0
            || Volatile.Read(ref stage.SessionRoutesConverged) != 0)
            return;
        if (stage.ActorStates.Count == 0)
        {
            Volatile.Write(ref stage.SessionRoutesConverged, 1);
            return;
        }
        if (!stage.TryBeginSessionRouteConvergence())
            return;
        if (!TryRunDetached(
                $"spot-relocation-session-route:{stage.Envelope.AggregateId:N}",
                cancellationToken => ConvergeRelocationSessionRoutesAsync(
                    stage,
                    cancellationToken)))
            stage.EndSessionRouteConvergence();
    }

    internal async ValueTask ConvergeRelocationSessionRoutesAsync(
        TargetStage stage,
        CancellationToken cancellationToken)
    {
        try
        {
            var handoffId = stage.Envelope.AggregateId.ToString("N");
            foreach (var actorState in stage.ActorStates)
            {
                for (var attempt = 1; ; attempt++)
                {
                    try
                    {
                        await ConvergeRelocationSessionRouteAsync(
                                actorState,
                                handoffId,
                                cancellationToken)
                            .ConfigureAwait(false);
                        break;
                    }
                    catch (Exception exception)
                        when (exception is not OperationCanceledException
                              && attempt < SessionRouteConvergenceAttempts)
                    {
                        ZLinkFrameworkDebugLog.SpotDiscovery(
                            $"relocation_session_route_retry actor={actorState.ActorId} "
                            + $"handoff={handoffId} attempt={attempt}");
                        await Task.Delay(
                                SessionRouteConvergenceRetryDelay * attempt,
                                cancellationToken)
                            .ConfigureAwait(false);
                    }
                }
            }
            Volatile.Write(ref stage.SessionRoutesConverged, 1);
        }
        finally
        {
            stage.EndSessionRouteConvergence();
        }
    }

    private async ValueTask ConvergeRelocationSessionRouteAsync(
        ZLinkActorRuntimeState actorState,
        string handoffId,
        CancellationToken cancellationToken)
    {
        bool commit;
        try
        {
            commit = await CommitCompletedSessionRouteAsync(
                    actorState,
                    handoffId,
                    cancellationToken)
                .ConfigureAwait(false);
        }
        catch (ZLinkFrameworkException exception)
            when (exception.Kind == ZLinkFrameworkErrorKind.InvalidOperation)
        {
            // The session owner fenced this commit against a replaced binding
            // identity: a late ACK must never commit, so the route is
            // terminal for this handoff.
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"relocation_session_route_fenced actor={actorState.ActorId} "
                + $"handoff={handoffId}");
            return;
        }
        if (!commit)
            return;
    }

    internal static async ValueTask PublishCatalogBeforeNormalizationAsync(
        TargetStage stage,
        Action publishCatalog,
        Func<ValueTask>? normalizeAuthority)
    {
        ArgumentNullException.ThrowIfNull(stage);
        ArgumentNullException.ThrowIfNull(publishCatalog);
        if (Volatile.Read(ref stage.LocalCatalogPublished) == 0)
        {
            publishCatalog();
            Volatile.Write(ref stage.LocalCatalogPublished, 1);
        }
        if (normalizeAuthority is not null)
            await normalizeAuthority().ConfigureAwait(false);
    }

    internal async ValueTask PrepareInboundSpotAggregateAsync(
        TargetStage stage,
        CancellationToken cancellationToken)
    {
        await stage.PublishGate.WaitAsync(cancellationToken)
            .ConfigureAwait(false);
        try
        {
            if (Volatile.Read(ref stage.Published) != 0)
                return;
            var spotParticipant = stage.Envelope.Participants.Single(
                static participant => participant.ObjectKind
                    is ZLinkPlacementObjectKind.UserSpot
                    or ZLinkPlacementObjectKind.InstanceSpot);
            await PrepareInboundSpotAggregateCoreAsync(
                    stage,
                    spotParticipant,
                    cancellationToken)
                .ConfigureAwait(false);
        }
        finally
        {
            stage.PublishGate.Release();
        }
    }

    private async ValueTask PrepareInboundSpotAggregateCoreAsync(
        TargetStage stage,
        ZLinkRelocationParticipantEnvelope spotParticipant,
        CancellationToken cancellationToken)
    {
        foreach (var actorState in stage.ActorStates)
            await stage.Spot.Activation
                .CompleteTransferredActorJoinSealedAsync(
                    actorState,
                    stage.TargetAdmissionSeal,
                    cancellationToken)
                .ConfigureAwait(false);
        await stage.Node.Catalog.ValidateRelocatedReservedAsync(
                stage.Spot,
                stage.StableType,
                spotParticipant.ObjectGeneration,
                stage.TargetAuthorityOwnerGeneration,
                cancellationToken)
            .ConfigureAwait(false);
        await ReplayInboundAggregateActorsAsync(stage, cancellationToken)
            .ConfigureAwait(false);
        var replayJobs = spotParticipant.AcceptedJobs
            .Concat(stage.HeldRecords)
            .OrderBy(static job => job.AcceptedSequence)
            .ToArray();
        var replayed = Volatile.Read(ref stage.ReplayedJobCount);
        if (replayed > replayJobs.Length)
            throw new ZLinkRelocationDataLostException(
                $"Target SPOT '{stage.Spot.Activation.SpotId}' replay cursor exceeds its accepted journal.");
        if (replayed == replayJobs.Length)
            return;
        await stage.Spot.Activation.ReplayAcceptedJobsAsync(
                replayJobs,
                stage.SourceMeshName,
                stage.TargetAdmissionSeal,
                Volatile.Read(ref stage.ReplayedJobCount),
                (job, _, capturedReply, ct) =>
                    CompleteAcceptedReplayAsync(
                        stage,
                        job,
                        capturedReply,
                        ct),
                cancellationToken)
            .ConfigureAwait(false);
        if (Volatile.Read(ref stage.ReplayedJobCount) != replayJobs.Length)
            throw new ZLinkRelocationDataLostException(
                $"Target SPOT '{stage.Spot.Activation.SpotId}' did not complete every accepted replay.");
    }

    private async ValueTask ReplayInboundAggregateActorsAsync(
        TargetStage stage,
        CancellationToken cancellationToken)
    {
        if (stage.ActorStates.Count == 0)
            return;
        var handoffId = stage.Envelope.AggregateId.ToString("N");
        var authorityStore = Registration.Locations.ResolveStore()
                             ?? throw new ZLinkConfigurationException(
                                 "Location Store is not registered.");
        var relocationStore = Registration.Locations.ResolveRelocationStore()
                              ?? throw new ZLinkConfigurationException(
                                  "Relocation Store is not registered.");
        var targetOwner = LocationLifecycle?.OwnerToken
                          ?? throw new ZLinkConfigurationException(
                              "Location runtime is not registered.");
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            authorityStore,
            relocationStore);
        var current = stage.Envelope.CanonicalLogicalStream.IsEmpty
            ? stage.Envelope
            : await coordinator.ReadCanonicalRootAsync(
                    stage.Envelope,
                    targetOwner,
                    cancellationToken)
                .ConfigureAwait(false);
        var pipeline = new ZLinkActorInboundPipeline(
            this,
            new ZLinkEntrySpotActorInboundEndpoint(this));
        foreach (var actorState in stage.ActorStates.OrderBy(
                     static state => state.ActorId,
                     StringComparer.Ordinal))
        {
            var participant = current.Participants.Single(candidate =>
                candidate.ObjectKind == ZLinkPlacementObjectKind.Actor
                && candidate.AuthorityKey
                == ZLinkActorAuthorityPayloadCodec.AuthorityKey(
                    actorState.ActorId));
            var actorRef = actorState.NativeActorRef
                           ?? throw new ZLinkRelocationDataLostException(
                               $"Actor '{actorState.ActorId}' target reference is missing.");
            if (!actorState.Handoff.IsAuthorityCommitted(handoffId))
                actorState.Handoff.MarkAuthorityCommitted(
                    handoffId,
                    participant.ObjectGeneration,
                    actorRef.Generation);
            _ = actorState.Handoff.PrepareCanonicalMaintenanceReplay(
                handoffId);


            var barrier = actorState.ReserveHandoffRestoreBarrier();
            var turn = await barrier.ClaimAsync().ConfigureAwait(false);
            var queued = new List<Task>();
            try
            {
                foreach (var job in participant.AcceptedJobs
                             .OrderBy(static job => job.AcceptedSequence))
                {
                    var accepted = ZLinkStandaloneActorRelocationRuntime
                        .DecodeAcceptedRecords([job])
                        .Single();
                    var sourceActor = accepted.FrozenTargetActor
                                      ?? throw new ZLinkRelocationDataLostException(
                                          "Actor accepted journal lost its frozen source reference.");
                    var batch = ZLinkActorHandoffFrames.RestoreCanonical(
                        actorRef,
                        sourceActor,
                        [accepted]);
                    ZLinkSpotRelocationActorQueueReservation? queueReservation =
                        null;
                    try
                    {
                        queueReservation = stage.Spot.Activation
                            .ReserveRelocationActorReplay(
                                stage.TargetAdmissionSeal,
                                actorState.ActorId);
                        var replayAdmission =
                            new ZLinkSpotRelocationReplayAdmission(
                                stage.Spot.Activation,
                                stage.TargetAdmissionSeal,
                                handoffId,
                                queueReservation);
                        queued.Add(pipeline.QueueCanonicalReplayAsync(
                            batch,
                            async (frame, reply, ct) =>
                            {
                                await CompleteAcceptedActorReplayAsync(
                                        stage,
                                        participant,
                                        actorState,
                                        job,
                                        frame,
                                        reply,
                                        ct)
                                    .ConfigureAwait(false);
                            },
                            replayAdmission,
                            CancellationToken.None));
                    }
                    catch
                    {
                        queueReservation?.Discard();
                        batch.Dispose();
                        throw;
                    }
                }

            }
            finally
            {
                turn.Dispose();
            }

            await Task.WhenAll(queued).ConfigureAwait(false);
            // Authority publication can make new-owner ingress arrive before
            // the previous owner has delivered its Message Follow backlog.
            // Capture stays closed only until the publish path finishes the
            // queue merge; the trailing-reserve-then-open primitive then
            // orders followed frames before direct new-owner ingress and
            // opens admission once without a separate completion exchange.
        }
    }

    internal async ValueTask CompleteInboundSpotAggregateReplayAsync(
        TargetStage stage,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(stage);
        if (Volatile.Read(ref stage.Published) == 0)
            throw new InvalidOperationException(
                "Target SPOT admission cannot complete before publication.");

        var handoffId = stage.Envelope.AggregateId.ToString("N");
        var pipeline = new ZLinkActorInboundPipeline(
            this,
            new ZLinkEntrySpotActorInboundEndpoint(this));
        foreach (var actorState in stage.ActorStates.OrderBy(
                     static state => state.ActorId,
                     StringComparer.Ordinal))
        {
            if (actorState.Handoff
                    .IsCanonicalMaintenanceReplayComplete(handoffId))
                continue;
            if (actorState.Handoff
                    .GetCanonicalMaintenanceDrain(handoffId)
                is { } existingDrain)
            {
                await existingDrain.ConfigureAwait(false);
                continue;
            }
            var participant = stage.Envelope.Participants.Single(candidate =>
                candidate.ObjectKind == ZLinkPlacementObjectKind.Actor
                && candidate.AuthorityKey
                == ZLinkActorAuthorityPayloadCodec.AuthorityKey(
                    actorState.ActorId));
            var queuedThrough = checked((long)participant.AcceptedJobs
                .Select(static job => job.AcceptedSequence)
                                .DefaultIfEmpty(0UL)
                .Max());
            var actorRef = actorState.NativeActorRef
                           ?? throw new ZLinkRelocationDataLostException(
                               $"Actor '{actorState.ActorId}' target reference is missing.");
            var barrier = actorState.ReserveHandoffRestoreBarrier();
            var turn = await barrier.ClaimAsync().ConfigureAwait(false);
            IReadOnlyList<Task> queued;
            try
            {
                queued = actorState.Handoff
                    .ReserveCanonicalMaintenanceTrailing(
                        handoffId,
                        queuedThrough,
                        frame =>
                        {
                            var batch = ZLinkActorHandoffFrames.Restore(
                                actorRef,
                                [frame]);
                            ZLinkSpotRelocationActorQueueReservation?
                                queueReservation = null;
                            try
                            {
                                queueReservation = stage.Spot.Activation
                                    .ReserveRelocationActorReplay(
                                        stage.TargetAdmissionSeal,
                                        actorState.ActorId);
                                var replayAdmission =
                                    new ZLinkSpotRelocationReplayAdmission(
                                        stage.Spot.Activation,
                                        stage.TargetAdmissionSeal,
                                        handoffId,
                                        queueReservation);
                                return pipeline.DispatchReplayAsync(
                                        batch,
                                        arrivalIndex => actorState.Handoff
                                            .AcknowledgeReplayedFrame(arrivalIndex),
                                        replayAdmission,
                                        CancellationToken.None)
                                    .AsTask();
                            }
                            catch
                            {
                                queueReservation?.Discard();
                                batch.Dispose();
                                throw;
                            }
                        });
            }
            finally
            {
                // Direct new-owner ingress can append only after every
                // previous-owner frame owns its target queue position.
                turn.Dispose();
            }

            await Task.WhenAll(queued).ConfigureAwait(false);
        }
    }

    internal async ValueTask OpenInboundSpotAggregateAdmissionAsync(
        TargetStage stage)
    {
        ArgumentNullException.ThrowIfNull(stage);
        if (stage.AdmissionDrainTask is { } existing)
        {
            await existing.ConfigureAwait(false);
            return;
        }
        var handoffId = stage.Envelope.AggregateId.ToString("N");
        var pipeline = new ZLinkActorInboundPipeline(
            this,
            new ZLinkEntrySpotActorInboundEndpoint(this));
        var actorDrains = new List<Task>();

        OpenTargetAdmissionOnce(
            stage,
            () => stage.Spot.Activation.OpenRelocationTargetAdmission(
                stage.TargetAdmissionSeal,
                () =>
                {
                    foreach (var actorState in stage.ActorStates.OrderBy(
                                 static state => state.ActorId,
                                 StringComparer.Ordinal))
                    {
                        if (actorState.Handoff
                                .IsCanonicalMaintenanceReplayComplete(handoffId))
                            continue;
                        if (actorState.Handoff
                                .GetCanonicalMaintenanceDrain(handoffId)
                            is { } existingDrain)
                        {
                            actorDrains.Add(existingDrain);
                            continue;
                        }
                        var participant = stage.Envelope.Participants.Single(candidate =>
                            candidate.ObjectKind == ZLinkPlacementObjectKind.Actor
                            && candidate.AuthorityKey
                            == ZLinkActorAuthorityPayloadCodec.AuthorityKey(
                                actorState.ActorId));
                        var queuedThrough = checked((long)participant.AcceptedJobs
                            .Select(static job => job.AcceptedSequence)
                                            .DefaultIfEmpty(0UL)
                            .Max());
                        var actorRef = actorState.NativeActorRef
                                       ?? throw new ZLinkRelocationDataLostException(
                                           $"Actor '{actorState.ActorId}' target reference is missing.");
                        var queued = actorState.Handoff
                            .ReserveCanonicalMaintenanceTrailingAndOpenAdmission(
                                handoffId,
                                queuedThrough,
                                frame =>
                                {
                                    var batch = ZLinkActorHandoffFrames.Restore(
                                        actorRef,
                                        [frame]);
                                    ZLinkSpotRelocationActorQueueReservation?
                                        queueReservation = null;
                                    try
                                    {
                                        queueReservation = stage.Spot.Activation
                                            .ReserveRelocationActorReplay(
                                                stage.TargetAdmissionSeal,
                                                actorState.ActorId);
                                        var replayAdmission =
                                            new ZLinkSpotRelocationReplayAdmission(
                                                stage.Spot.Activation,
                                                stage.TargetAdmissionSeal,
                                                handoffId,
                                                queueReservation);
                                        return pipeline.DispatchReplayAsync(
                                                batch,
                                                arrivalIndex => actorState.Handoff
                                                    .AcknowledgeReplayedFrame(arrivalIndex),
                                                replayAdmission,
                                                CancellationToken.None)
                                            .AsTask();
                                    }
                                    catch
                                    {
                                        queueReservation?.Discard();
                                        batch.Dispose();
                                        throw;
                                    }
                                });
                        var drainStart = new TaskCompletionSource(
                            TaskCreationOptions.RunContinuationsAsynchronously);
                        var drain = DrainInboundActorReplayAsync(
                            actorState,
                            handoffId,
                            queued,
                            drainStart.Task);
                        actorState.Handoff.RegisterCanonicalMaintenanceDrain(
                            handoffId,
                            drain);
                        actorDrains.Add(drain);
                        drainStart.TrySetResult();
                    }
                }));

        // Admission is already visible. Persist the drain task on the stage
        // before awaiting it so retries observe the same success or failure
        // instead of treating AdmissionOpened as completion.
        stage.AdmissionDrainTask = Task.WhenAll(actorDrains);
        await stage.AdmissionDrainTask.ConfigureAwait(false);
    }

    private static async Task DrainInboundActorReplayAsync(
        ZLinkActorRuntimeState actorState,
        string handoffId,
        IReadOnlyCollection<Task> queued,
        Task drainStart)
    {
        await drainStart.ConfigureAwait(false);
        await Task.WhenAll(queued).ConfigureAwait(false);
        if (!actorState.Handoff
                .TryCompleteCanonicalMaintenanceReplay(handoffId))
            throw new ZLinkRelocationDataLostException(
                $"Actor '{actorState.ActorId}' target replay did not drain its cutover backlog.");
    }

    private async ValueTask CompleteAcceptedActorReplayAsync(
        TargetStage stage,
        ZLinkRelocationParticipantEnvelope participant,
        ZLinkActorRuntimeState actorState,
        ZLinkRelocationQueuedJob job,
        ZLinkSpotActorFrame frame,
        ZLinkActorReply? reply,
        CancellationToken cancellationToken)
    {
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"canonical_actor_replay_completion_begin actor={actorState.ActorId} "
            + $"accepted_sequence={job.AcceptedSequence} reply={reply is not null} "
            + $"has_reply_route={job.CanonicalRequest is { ReplyRouteId: not 0 }}");
        ZLinkCanonicalTerminalCompletion? completion = null;
        byte[]? replyFrame = null;
        var request = job.CanonicalRequest;
        if (request is { ReplyRouteId: not 0 })
        {
            if (reply is null)
                throw new ZLinkRelocationDataLostException(
                    "Actor accepted request replay produced no terminal reply.");
            replyFrame = reply.ToFrame(frame.Header);
            completion = ZLinkRelocationEnvelopeCodec
                .CreateCanonicalTerminalCompletion(
                    request.OperationHigh,
                    request.OperationLow,
                    request.Source.OwnerId,
                    request.Source.OwnerLeaseGeneration,
                    request.Source.NodeRid,
                    request.Source.NodeGeneration,
                    participant.CanonicalParticipantId,
                    job.AcceptedSequence,
                    0,
                    0,
                    new ZLinkCanonicalApplicationPayload(
                        frame.Header.Name,
                        DurableActorReplyContentType,
                        EncodeDurableActorReply(
                            request.ReplyRouteId,
                            replyFrame)));
        }
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"canonical_actor_replay_handled actor={actorState.ActorId} "
            + $"accepted_sequence={job.AcceptedSequence} "
            + $"has_completion={completion is not null}");
        if (completion is not null)
        {
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"canonical_actor_reply_relay_begin actor={actorState.ActorId} "
                + $"accepted_sequence={job.AcceptedSequence} "
                + $"reply_route={request!.ReplyRouteId}");
            var acknowledgement = await TryRelayCanonicalReplyAsync(
                    RoutingId.FromHex(completion.SourceNodeRid),
                    await CreateCanonicalReplyRelayAsync(
                            Registration.Locations.ResolveStore()
                            ?? throw new ZLinkConfigurationException(
                                "Location Store is not registered."),
                            stage,
                            participant,
                            request!,
                            completion,
                            cancellationToken)
                        .ConfigureAwait(false),
                    completion,
                    [replyFrame!],
                    cancellationToken)
                .ConfigureAwait(false);
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"canonical_actor_reply_relay_result actor={actorState.ActorId} "
                + $"accepted_sequence={job.AcceptedSequence} "
                + $"acknowledgement={acknowledgement}");
            if (!await CompleteCanonicalReplyDeliveryAsync(
                    completion,
                    acknowledgement,
                    cancellationToken)
                    .ConfigureAwait(false))
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    "Actor request reply is waiting for its exact source acknowledgement.",
                    ZLinkRetryAdvice.RetryAfterBackoff);
        }
        actorState.Handoff.AcknowledgeCanonicalReplayThrough(
            job.AcceptedSequence);
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"canonical_actor_replay_completion_end actor={actorState.ActorId} "
            + $"accepted_sequence={job.AcceptedSequence}");
    }


    private async ValueTask CompleteAcceptedReplayAsync(
        TargetStage stage,
        ZLinkRelocationQueuedJob job,
        byte[][]? capturedReply,
        CancellationToken cancellationToken)
    {
        if (stage.Envelope.CanonicalLogicalStream.IsEmpty)
        {
            if (capturedReply is not null)
                throw new ZLinkRelocationDataLostException(
                    "A legacy relocation reply has no exact request-source fence for service-wire relay.");
            Interlocked.Increment(ref stage.ReplayedJobCount);
            return;
        }

        var request = job.CanonicalRequest
                      ?? throw new ZLinkRelocationDataLostException(
                          "Canonical replay record has no request projection.");
        ZLinkCanonicalTerminalCompletion? completion = null;
        if (request.ReplyRouteId != 0)
        {
            if (capturedReply is null)
                throw new ZLinkRelocationDataLostException(
                    "Canonical request replay completed without a terminal reply.");
            completion = ZLinkRelocationEnvelopeCodec
                .CreateCanonicalTerminalCompletion(
                    request.OperationHigh,
                    request.OperationLow,
                    request.Source.OwnerId,
                    request.Source.OwnerLeaseGeneration,
                    request.Source.NodeRid,
                    request.Source.NodeGeneration,
                    stage.Envelope.Participants.Single(static participant =>
                            participant.ObjectKind
                            is ZLinkPlacementObjectKind.UserSpot
                            or ZLinkPlacementObjectKind.InstanceSpot)
                        .CanonicalParticipantId,
                    job.AcceptedSequence,
                    terminalResult: 0,
                    errorCode: 0,
                    DecodeCanonicalReply(capturedReply));
        }

        var authorityStore = Registration.Locations.ResolveStore()
                             ?? throw new ZLinkConfigurationException(
                                 "Location Store is not registered.");
        if (completion is not null)
        {
            var sourceNodeRid = RoutingId.FromHex(completion.SourceNodeRid);
            var participant = stage.Envelope.Participants.Single(
                static candidate => candidate.ObjectKind
                    is ZLinkPlacementObjectKind.UserSpot
                    or ZLinkPlacementObjectKind.InstanceSpot);
            var acknowledged = await TryRelayCanonicalReplyAsync(
                    sourceNodeRid,
                    await CreateCanonicalReplyRelayAsync(
                            authorityStore,
                            stage,
                            participant,
                            request,
                            completion,
                            cancellationToken)
                        .ConfigureAwait(false),
                    completion,
                    capturedReply!,
                    cancellationToken)
                .ConfigureAwait(false);
            if (!await CompleteCanonicalReplyDeliveryAsync(
                    completion,
                    acknowledged,
                    cancellationToken)
                .ConfigureAwait(false))
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    "The canonical request reply has neither an acknowledgement nor exact source lease expiry proof.",
                    ZLinkRetryAdvice.RetryAfterBackoff);
        }

        Interlocked.Increment(ref stage.ReplayedJobCount);
    }

    private async ValueTask<bool> CompleteCanonicalReplyDeliveryAsync(
        ZLinkCanonicalTerminalCompletion completion,
        ZLinkRelocationReplyAckState acknowledgement,
        CancellationToken cancellationToken)
    {
        if (acknowledgement is
            ZLinkRelocationReplyAckState.TerminalReceived
            or ZLinkRelocationReplyAckState.AlreadyTerminal)
            return true;

        var store = Registration.Locations.ResolveStore()
                    ?? throw new ZLinkConfigurationException(
                        "Location Store is not registered.");
        var lease = await store.ReadOwnerLeaseAsync(
                completion.SourceOwnerId,
                cancellationToken)
            .ConfigureAwait(false);
        if (!IsExactSourceLeaseExpired(lease, completion))
            return false;
        return true;
    }

    private async ValueTask<ZLinkRelocationReplyAckState>
        TryRelayCanonicalReplyAsync(
        RoutingId sourceNodeRid,
        ZLinkServiceWireCodec.ReplyRelayRecord relay,
        ZLinkCanonicalTerminalCompletion completion,
        IReadOnlyList<byte[]> payload,
        CancellationToken cancellationToken)
    {
        try
        {
            return await RelayRelocationReplyAsync(
                    sourceNodeRid,
                    relay,
                    new ZLinkServiceWireCodec.RequestSourceFence(
                        completion.SourceOwnerId,
                        completion.SourceOwnerLeaseGeneration,
                        RoutingId.FromHex(completion.SourceNodeRid),
                        completion.SourceNodeGeneration),
                    payload,
                    cancellationToken)
                .ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch
        {
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"canonical_reply_relay_failed operation={completion.OperationHigh:x16}{completion.OperationLow:x16}");
            return ZLinkRelocationReplyAckState.NotAcknowledged;
        }
    }

    private async ValueTask<ZLinkServiceWireCodec.ReplyRelayRecord>
        CreateCanonicalReplyRelayAsync(
            IZLinkLocationRepository authorityStore,
            TargetStage stage,
            ZLinkRelocationParticipantEnvelope participant,
            ZLinkCanonicalAcceptedRequest request,
            ZLinkCanonicalTerminalCompletion completion,
            CancellationToken cancellationToken)
    {
        var authority = await authorityStore.ReadAuthorityAsync(
                participant.AuthorityKey,
                cancellationToken)
            .ConfigureAwait(false);
        if (authority is not ZLinkAuthorityReadResult.Found found)
            throw new ZLinkRelocationDataLostException(
                "The canonical reply relay authority is missing.");
        var targetOwner = LocationLifecycle?.OwnerToken
                          ?? throw new ZLinkConfigurationException(
                              "Location runtime is not registered.");
        if (!ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                found.Snapshot.Payload.Span,
                out var canonical)
            || !IsExactCanonicalReplyRelayTarget(
                canonical,
                stage.Envelope.CanonicalRelocationHigh,
                stage.Envelope.CanonicalRelocationLow,
                stage.TargetAttemptGeneration,
                stage.Node.Node.RoutingId,
                stage.TargetNodeLifecycleGeneration,
                targetOwner))
            throw new ZLinkRelocationDataLostException(
                "The canonical reply relay target attempt does not match durable authority.");
        return new ZLinkServiceWireCodec.ReplyRelayRecord(
            new MeshOperationId(
                completion.OperationHigh,
                completion.OperationLow),
            request.ReplyRouteId,
            new ZLinkServiceWireCodec.RelocationWireId(
                stage.Envelope.CanonicalRelocationHigh,
                stage.Envelope.CanonicalRelocationLow),
            canonical.TargetAttemptGeneration,
            new ZLinkServiceWireCodec.RelocationCoordinatorFence(
                targetOwner.OwnerId,
                checked((ulong)targetOwner.LeaseGeneration),
                stage.Node.Node.RoutingId,
                stage.TargetNodeLifecycleGeneration,
                found.Snapshot.StoreVersion),
            completion.ParticipantId,
            completion.AcceptedSequence,
            completion.TerminalResult,
            (ServiceWireConstants.FrameworkErrorCode)completion.ErrorCode);
    }

    internal static bool IsExactCanonicalReplyRelayTarget(
        ZLinkCanonicalRelocationAuthorityProjection canonical,
        ulong relocationHigh,
        ulong relocationLow,
        ulong targetAttemptGeneration,
        RoutingId targetNodeRid,
        ulong targetNodeGeneration,
        ZLinkLocationOwnerToken targetOwner) =>
        targetAttemptGeneration != 0
        && canonical.TargetAttemptGeneration == targetAttemptGeneration
        && canonical.RelocationHigh == relocationHigh
        && canonical.RelocationLow == relocationLow
        && canonical.State.TargetNodeRid == targetNodeRid.ToHex()
        && canonical.State.TargetNodeGeneration == targetNodeGeneration
        && canonical.TargetOwnerId == targetOwner.OwnerId
        && targetOwner.LeaseGeneration > 0
        && canonical.TargetOwnerLeaseGeneration
           == checked((ulong)targetOwner.LeaseGeneration);

    internal static bool IsExactSourceLeaseExpired(
        ZLinkOwnerLeaseReadResult lease,
        ZLinkCanonicalTerminalCompletion completion) =>
        lease switch
        {
            ZLinkOwnerLeaseReadResult.Missing => true,
            ZLinkOwnerLeaseReadResult.Found found =>
                completion.SourceOwnerLeaseGeneration > long.MaxValue
                || !StringComparer.Ordinal.Equals(
                    found.Token.OwnerId,
                    completion.SourceOwnerId)
                || found.Token.LeaseGeneration
                   != checked((long)completion.SourceOwnerLeaseGeneration)
                || found.LeaseExpiresAt <= found.StoreNow,
            _ => false
        };

    private static ZLinkCanonicalApplicationPayload DecodeCanonicalReply(
        IReadOnlyList<byte[]> replyParts)
    {
        var parts = replyParts.Select(static part => Message.From(part)).ToArray();
        try
        {
            var header = ZLinkEnvelopeCodec.DecodeHeader(parts);
            if (parts.Length != 2)
                throw new ZLinkRelocationDataLostException(
                    "Canonical terminal reply must contain header and body parts.");
            return new ZLinkCanonicalApplicationPayload(
                header.MessageName,
                header.ContentType,
                parts[1].ToArray());
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }
    }

    private static byte[][] EncodeCanonicalReply(
        string channelName,
        ZLinkCanonicalAcceptedRequest request,
        ZLinkCanonicalTerminalCompletion completion)
    {
        var payload = completion.Payload
                      ?? throw new ZLinkRelocationDataLostException(
                          "Canonical terminal reply has no application payload.");
        using var header = ZLinkEnvelopeCodec.EncodeHeader(
            new ZLinkEnvelopeHeader(
                ZLinkMessageKind.Response,
                channelName,
                payload.PacketName,
                payload.ContentType,
                request.ReplyRouteId.ToString(
                    System.Globalization.CultureInfo.InvariantCulture),
                null,
                null,
                null,
                null));
        return [header.ToArray(), payload.Payload.ToArray()];
    }

    internal async ValueTask AbortInboundSpotAggregateAsync(TargetStage stage)
    {
        if (Volatile.Read(ref stage.Published) != 0)
            return;
        foreach (var actorState in stage.ActorStates)
        {
            actorState.Handoff.AbortImport(
                stage.Envelope.AggregateId.ToString("N"));
            await _actorSessionManager.RollbackTransferredActorAsync(
                    actorState.ActorId,
                    CancellationToken.None)
                .ConfigureAwait(false);
        }
        await stage.Node.Catalog.DiscardReservedAsync(stage.Spot)
            .ConfigureAwait(false);
    }

    internal ValueTask RelayCommittedSpotRecordsAsync(
        ZLinkSpotRetireReservation reservation,
        ZLinkAggregateRelocationPublished relocation,
        IReadOnlyList<ZLinkAcceptedWorkRecord> held,
        CancellationToken cancellationToken)
    {
        // The accepted queue, including ingress held at the cutoff, is already
        // part of the one immutable root. Command 31 transfers that journal;
        // no second network relay is allowed after authority publication.
        cancellationToken.ThrowIfCancellationRequested();
        _ = reservation;
        _ = relocation;
        _ = held;
        return ValueTask.CompletedTask;
    }
}
