using System.Collections.Concurrent;
using System.Diagnostics;
using Microsoft.Extensions.DependencyInjection;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime.Backend.DotNet.Mappings;
using Zlink.Framework.Runtime.Identifiers;

namespace Zlink.Framework.Runtime.Host;

internal sealed partial class ZLinkFrameworkRuntime
{
    internal async ValueTask<ZLinkRelocationReplyAckState> RelayRelocationReplyAsync(
        RoutingId sourceNodeRid,
        ZLinkServiceWireCodec.ReplyRelayRecord relay,
        ZLinkServiceWireCodec.RequestSourceFence expectedSource,
        IReadOnlyList<byte[]> payload,
        CancellationToken cancellationToken
    )
    {
        var node = GetSpotNodeRuntime(relay.Coordinator.NodeRid);
        var messages = ZLinkRawRouterServicePort.CreateMessages(
            payload,
            static part => Message.From(part)
        );
        ZLinkServiceWireCodec.ReplyRelayAckRecord ack;
        try
        {
            ack = await node.RelayRelocationReplyAsync(
                    sourceNodeRid,
                    relay,
                    expectedSource,
                    messages,
                    node.Registration.DefaultRequestTimeout ?? Registration.DefaultRequestTimeout,
                    cancellationToken
                )
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
        CancellationToken cancellationToken
    )
    {
        var targetRid = RoutingId.FromHex(request.TargetNodeRid);
        var node = GetSpotNodeRuntime(targetRid);
        var spotParticipant = envelope.Participants.Single(static participant =>
            participant.ObjectKind
                is ZLinkPlacementObjectKind.UserSpot
                    or ZLinkPlacementObjectKind.InstanceSpot
        );
        if (spotParticipant.ObjectGeneration == 0 || request.SpotId.Length == 0)
            throw new InvalidDataException(
                "The inbound SPOT relocation target does not match its envelope."
            );

        var targetOwnerGeneration = spotParticipant.AuthorityOwnerGeneration;
        async ValueTask<PreparedReservedSpot> PrepareTargetSpotAsync()
        {
            if (request.InstanceSpot)
            {
                return await node
                    .Catalog.PrepareInstanceReservedAsync(
                        request.StableType,
                        request.SpotId,
                        spotParticipant.ObjectGeneration,
                        targetOwnerGeneration,
                        cancellationToken,
                        restoreLogicalTimers: true,
                        logicalTimers: spotParticipant.LogicalTimers,
                        relocationTarget: true
                    )
                    .ConfigureAwait(false);
            }

            if (
                !node.Registration.SpotRelocations.TryGetValue(
                    request.StableType,
                    out var spotRegistration
                )
            )
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.TypeMismatch,
                    $"User SPOT type '{request.StableType}' is not registered on target node."
                );
            return await node
                .Catalog.PrepareReservedAsync(
                    spotRegistration.InstanceType,
                    request.SpotId,
                    spotParticipant.ObjectGeneration,
                    targetOwnerGeneration,
                    ZLinkMessage.Empty,
                    cancellationToken,
                    invokeCreate: false,
                    logicalTimers: spotParticipant.LogicalTimers
                )
                .ConfigureAwait(false);
        }

        var preparedSpot = await PrepareTargetSpotAsync().ConfigureAwait(false);

        var actorStates = new List<ZLinkActorRuntimeState>();
        var actorTargetAuthorityOwnerGenerations = new Dictionary<ZLinkActorId, ulong>();
        var boundActorIds = new ConcurrentDictionary<ZLinkActorId, byte>();
        var stagedActorStates = new ConcurrentDictionary<ZLinkActorId, ZLinkActorRuntimeState>();
        ZLinkSpotRelocationSeal? targetAdmissionSeal = null;
        try
        {
            await preparedSpot
                .Activation.RestoreSpotRelocationStateAsync(
                    spotParticipant.ApplicationState,
                    cancellationToken
                )
                .ConfigureAwait(false);

            var restoredActors = await RestoreInboundAggregateActorParticipantsAsync(
                    request,
                    envelope,
                    node,
                    preparedSpot,
                    boundActorIds,
                    stagedActorStates,
                    cancellationToken
                )
                .ConfigureAwait(false);
            foreach (var restored in restoredActors)
            {
                actorTargetAuthorityOwnerGenerations.Add(
                    restored.State.RuntimeActorId,
                    restored.TargetAuthorityOwnerGeneration
                );
                actorStates.Add(restored.State);
            }

            targetAdmissionSeal = await preparedSpot
                .Activation.TrySealRelocationAsync()
                .ConfigureAwait(false);
            if (targetAdmissionSeal is null)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    $"Target SPOT '{request.SpotId}' could not seal staging admission.",
                    ZLinkRetryAdvice.RetryAfterBackoff
                );
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
                    request.SourceOwnerLeaseGeneration
                ),
                request.CoordinatorExpectedAuthorityStoreVersion,
                request.RelocationReference,
                request.RelocationChecksum,
                Stopwatch.GetElapsedTime(0) + ZLinkSpotRetireTargetRuntime.StageRetention,
                ZLinkSpotRetireTargetRuntime.ComputeStageRequestDigest(request),
                targetAdmissionSeal
                    ?? throw new InvalidOperationException(
                        "Target staging admission seal was not created."
                    ),
                actorTargetAuthorityOwnerGenerations,
                spotParticipant.AuthorityOwnerGeneration,
                targetOwnerGeneration,
                request.MeshName,
                request.TargetNodeLifecycleGeneration,
                request.TargetOwnerLeaseGeneration,
                request.TargetAttemptGeneration
            );
        }
        catch (Exception primaryFailure)
        {
            var failures = new ZLinkFailureCollector(primaryFailure);
            if (targetAdmissionSeal is not null)
                await failures
                    .CaptureAsync(async () =>
                        _ = await preparedSpot
                            .Activation.AbortRelocationAsync(targetAdmissionSeal)
                            .ConfigureAwait(false)
                    )
                    .ConfigureAwait(false);
            foreach (var actorId in boundActorIds.Keys)
            {
                failures.Capture(() =>
                {
                    if (stagedActorStates.TryGetValue(actorId, out var actorState))
                        actorState.Handoff.AbortImport(envelope.AggregateId.ToString("N"));
                });
                await failures
                    .CaptureAsync(() =>
                        _actorSessionManager.RollbackTransferredActorAsync(
                            actorId.Value,
                            CancellationToken.None
                        )
                    )
                    .ConfigureAwait(false);
            }
            await failures
                .CaptureAsync(() => node.Catalog.DiscardReservedAsync(preparedSpot))
                .ConfigureAwait(false);
            failures.ThrowIfAny();
            throw;
        }
    }

    /// <summary>
    /// Restores every Actor participant of an inbound SPOT aggregate onto the
    /// prepared target activation: validates each participant's staged
    /// authority, rebinds the Actor, stages its relocation session route and
    /// canonical maintenance import, and prepares its transferred join.
    /// Successfully bound Actors are recorded in
    /// <paramref name="boundActorIds"/>/<paramref name="stagedActorStates"/>
    /// as they land so the staging owner's failure path can roll them back.
    /// Returns the restored Actors ordered by Actor id.
    /// </summary>
    private async ValueTask<
        IReadOnlyList<(ZLinkActorRuntimeState State, ulong TargetAuthorityOwnerGeneration)>
    > RestoreInboundAggregateActorParticipantsAsync(
        ZLinkCanonicalSpotStageContext request,
        ZLinkRelocationEnvelope envelope,
        ZLinkSpotNodeRuntime node,
        PreparedReservedSpot preparedSpot,
        ConcurrentDictionary<ZLinkActorId, byte> boundActorIds,
        ConcurrentDictionary<ZLinkActorId, ZLinkActorRuntimeState> stagedActorStates,
        CancellationToken cancellationToken
    )
    {
        //  AuthorityKey(actorId) is the injective wire encoding of the Actor
        //  id, so keying the request descriptors by it once replaces the
        //  former per-participant scan that recomputed every candidate's
        //  AuthorityKey (O(participants x descriptors) key encodings).
        //  Duplicate Actor ids still fail at this materialization, exactly
        //  as the previous ActorId-keyed dictionary did.
        var descriptors = request.Actors.ToDictionary(static actor =>
            ZLinkActorAuthorityPayloadCodec.AuthorityKey(actor.ActorId)
        );
        var actorParticipants = envelope
            .Participants.Where(static participant =>
                participant.ObjectKind == ZLinkPlacementObjectKind.Actor
            )
            .ToArray();
        var restoredActors =
            new ConcurrentBag<(
                ZLinkActorRuntimeState State,
                ulong TargetAuthorityOwnerGeneration
            )>();
        using var restoreConcurrency = new SemaphoreSlim(64);
        await Task.WhenAll(
                actorParticipants.Select(async participant =>
                {
                    await restoreConcurrency.WaitAsync(cancellationToken).ConfigureAwait(false);
                    try
                    {
                        if (!descriptors.TryGetValue(participant.AuthorityKey, out var descriptor))
                            throw new InvalidOperationException(
                                $"Actor participant '{participant.AuthorityKey.Value}' has no staged descriptor."
                            );
                        var hasActor = ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
                            descriptor.AuthorityPayload,
                            out var authority
                        );
                        var hasRelocation = ZLinkActorRelocationAuthorityPayloadCodec.TryDecode(
                            descriptor.AuthorityPayload,
                            out var relocationAuthority
                        );
                        var hasRegistration = node.Registration.ActorRelocations.TryGetValue(
                            hasActor ? authority.StableType : string.Empty,
                            out var relocation
                        );
                        hasRegistration &= hasActor;
                        var steadySource =
                            !hasRelocation
                            && hasActor
                            && authority.State == ZLinkActorAuthorityState.Ready;
                        if (
                            !hasActor
                            || !steadySource
                                && (
                                    !hasRelocation
                                    || relocationAuthority.RelocationId != envelope.AggregateId
                                    || relocationAuthority.Phase
                                        is not (
                                            ZLinkActorRelocationAuthorityPhase.Activated
                                            or ZLinkActorRelocationAuthorityPhase.Steady
                                        )
                                )
                            || !hasRegistration
                        )
                        {
                            throw new InvalidDataException(
                                $"Actor participant '{participant.AuthorityKey.Value}' cannot be restored on the target."
                            );
                        }

                        var actorTargetAuthorityOwnerGeneration =
                            participant.AuthorityOwnerGeneration;
                        var actorState = GetOrCreateActorState(authority.ActorId);
                        await _actorSessionManager
                            .PrepareForTransferredActivationAsync(actorState, cancellationToken)
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
                                cancellationToken
                            )
                            .ConfigureAwait(false);
                        var actorKey = ZLinkActorId.FromBoundary(
                            authority.ActorId,
                            nameof(authority.ActorId)
                        );
                        boundActorIds.TryAdd(actorKey, 0);
                        stagedActorStates.TryAdd(actorKey, actorState);
                        var boundRoute = ZLinkSessionRelocationWire.RestoreBinding(
                            participant,
                            request.MeshName,
                            request.SourceNodeLifecycleGeneration,
                            request.SourceOwnerLeaseGeneration
                        );
                        var wireContext = boundRoute.IsBound
                            ? ZLinkSessionRelocationContext.Create(
                                envelope.AggregateId,
                                request.SourceOwnerId,
                                request.SourceOwnerLeaseGeneration,
                                RoutingId.FromHex(request.SourceNodeRid),
                                request.SourceNodeLifecycleGeneration,
                                request.CoordinatorExpectedAuthorityStoreVersion
                            )
                            : default;
                        actorState.StageRelocationSessionRoute(
                            envelope.AggregateId.ToString("N"),
                            boundRoute,
                            wireContext
                        );
                        var acceptedFrames = ZLinkStandaloneActorRelocationRuntime
                            .DecodeAcceptedRecords(participant.AcceptedJobs)
                            .Select(static accepted => accepted.Frame)
                            .ToArray();
                        actorState.Handoff.BeginCanonicalMaintenanceImport(
                            envelope.AggregateId.ToString("N"),
                            acceptedFrames
                        );
                        await preparedSpot
                            .Activation.PrepareTransferredActorJoinAndReplayAsync(
                                creation.Actor,
                                actorState,
                                cancellationToken
                            )
                            .ConfigureAwait(false);
                        restoredActors.Add((actorState, actorTargetAuthorityOwnerGeneration));
                    }
                    finally
                    {
                        restoreConcurrency.Release();
                    }
                })
            )
            .ConfigureAwait(false);
        return restoredActors
            .OrderBy(static actor => actor.State.ActorId, StringComparer.Ordinal)
            .ToArray();
    }

    // The caller owns the stage publish gate through CAS settlement and publication.
    internal async ValueTask PublishInboundSpotAggregateCoreAsync(
        TargetStage stage,
        CancellationToken cancellationToken
    )
    {
        var spotParticipant = stage.SpotParticipant;
        if (Volatile.Read(ref stage.Published) == 0)
        {
            // The target commit is confirmed before this point. Each member
            // Actor's bound Session projection follows that commit before the
            // ordered backlog runs (relocation 04 §4.6, §7), so a backlog
            // handler pushes with the committed target binding.
            var handoffId = stage.Envelope.AggregateId.ToString("N");
            foreach (var actorState in stage.ActorStates)
            {
                var actorRef =
                    actorState.NativeActorRef
                    ?? throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.NotFound,
                        $"Actor '{actorState.ActorId}' has no staged target reference."
                    );
                var actorType =
                    actorState.ActorType
                    ?? throw new InvalidOperationException(
                        $"Actor '{actorState.ActorId}' has no registered type during relocation."
                    );
                var authority = stage.GetPublishedActorAuthority(actorState.RuntimeActorId);
                await RequireActorRelocationLocationLifecycle(LocationLifecycle, actorState.ActorId)
                    .ActorOwnership.AdoptCommittedActorAuthorityAsync(
                        actorState.RuntimeActorId,
                        actorType,
                        actorRef.ToNative(stage.TargetMeshName),
                        authority,
                        _ => DeactivateActorOnOwnershipLossAsync(actorState)
                    )
                    .ConfigureAwait(false);
                actorState.MarkRelocationSessionAuthorityCommitted(
                    handoffId,
                    actorRef,
                    authority.AuthorityOwnerGeneration,
                    ZLinkMeshName.FromBoundary(stage.TargetMeshName, nameof(stage.TargetMeshName)),
                    stage.TargetNodeLifecycleGeneration,
                    stage.TargetOwnerLeaseGeneration
                );
            }
            await PrepareInboundSpotAggregateCoreAsync(stage, spotParticipant, cancellationToken)
                .ConfigureAwait(false);

            if (Volatile.Read(ref stage.RelocationReadyDelivered) == 0)
            {
                await stage
                    .Spot.Activation.InvokeTargetRelocationReadyCompletedAsync(
                        stage.TargetAdmissionSeal,
                        cancellationToken
                    )
                    .ConfigureAwait(false);
                Volatile.Write(ref stage.RelocationReadyDelivered, 1);
            }

            await Services
                .GetRequiredService<ZLinkSpotRetireTargetRuntime>()
                .InitializeRelocatedTargetAsync(stage, cancellationToken)
                .ConfigureAwait(false);

            await PublishCatalogBeforeNormalizationAsync(
                    stage,
                    () =>
                    {
                        foreach (var actorState in stage.ActorStates)
                            _actorSessionManager.PublishReservedActor(actorState.ActorId);
                        stage.Node.Catalog.PublishRelocatedReserved(stage.Spot);
                    },
                    normalizeAuthority: null
                )
                .ConfigureAwait(false);
            Volatile.Write(ref stage.Published, 1);
        }

        // Published means queue publication is complete: restore, replay,
        // catalog, and the ready callback. Admission opens from the
        // publish path and session route convergence remains asynchronous.
    }

    internal static async ValueTask OpenTargetAdmissionOnceAsync(
        TargetStage stage,
        Func<ValueTask<bool>> openAdmission
    )
    {
        ArgumentNullException.ThrowIfNull(stage);
        ArgumentNullException.ThrowIfNull(openAdmission);
        if (
            Volatile.Read(ref stage.AuthorityPublished) == 0
            || Volatile.Read(ref stage.Published) == 0
        )
            throw new InvalidOperationException(
                "Target admission cannot open before authority and queue publication."
            );
        if (Volatile.Read(ref stage.AdmissionOpened) != 0)
            return;
        if (!await openAdmission().ConfigureAwait(false))
            throw new InvalidOperationException(
                $"Target SPOT '{stage.Spot.Activation.SpotId}' lost its staging admission seal."
            );
        Volatile.Write(ref stage.AdmissionOpened, 1);
    }

    internal void ScheduleRelocationSessionRouteConvergence(TargetStage stage)
    {
        if (
            Volatile.Read(ref stage.AuthorityPublished) == 0
            || Volatile.Read(ref stage.Published) == 0
            || Volatile.Read(ref stage.SessionRoutesConverged) != 0
        )
            return;
        if (stage.ActorStates.Count == 0)
        {
            Volatile.Write(ref stage.SessionRoutesConverged, 1);
            return;
        }
        if (!stage.TryBeginSessionRouteConvergence())
            return;
        if (
            !TryRunDetached(
                $"spot-relocation-session-route:{stage.Envelope.AggregateId:N}",
                cancellationToken => ConvergeRelocationSessionRoutesAsync(stage, cancellationToken)
            )
        )
            stage.EndSessionRouteConvergence();
    }

    internal async ValueTask ConvergeRelocationSessionRoutesAsync(
        TargetStage stage,
        CancellationToken cancellationToken
    )
    {
        try
        {
            var handoffId = stage.Envelope.AggregateId.ToString("N");
            foreach (var actorState in stage.ActorStates)
            {
                await ConvergeRelocationSessionRouteAsync(actorState, handoffId, cancellationToken)
                    .ConfigureAwait(false);
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
        CancellationToken cancellationToken
    )
    {
        bool commit;
        try
        {
            commit = await CommitCompletedSessionRouteAsync(
                    actorState,
                    handoffId,
                    cancellationToken
                )
                .ConfigureAwait(false);
        }
        catch (ZLinkFrameworkException exception)
            when (exception.Kind == ZLinkFrameworkErrorKind.InvalidOperation)
        {
            // The session owner fenced this commit against a replaced binding
            // identity: a late ACK must never commit, so the route is
            // terminal for this handoff.
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"relocation_session_route_fenced actor={actorState.ActorId} handoff={handoffId}"
            );
            return;
        }
        if (!commit)
            return;
    }

    internal static async ValueTask PublishCatalogBeforeNormalizationAsync(
        TargetStage stage,
        Action publishCatalog,
        Func<ValueTask>? normalizeAuthority
    )
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

    private async ValueTask PrepareInboundSpotAggregateCoreAsync(
        TargetStage stage,
        ZLinkRelocationParticipantEnvelope spotParticipant,
        CancellationToken cancellationToken
    )
    {
        await stage
            .Node.Catalog.ValidateRelocatedReservedAsync(
                stage.Spot,
                stage.StableType,
                spotParticipant.ObjectGeneration,
                stage.TargetAuthorityOwnerGeneration,
                cancellationToken
            )
            .ConfigureAwait(false);
        AdmitInboundAggregateActorPrefixes(stage, cancellationToken);
        var replayJobs = spotParticipant
            .AcceptedJobs.Concat(stage.SnapshotHeldRecords())
            .OrderBy(static job => job.AcceptedSequence)
            .ToArray();
        if (stage.AdmittedSpotReplayCount > replayJobs.Length)
            throw new ZLinkRelocationDataLostException(
                $"Target SPOT '{stage.Spot.Activation.SpotId}' replay cursor exceeds its accepted journal."
            );
        foreach (var job in replayJobs.Skip(stage.AdmittedSpotReplayCount))
        {
            cancellationToken.ThrowIfCancellationRequested();
            var delivery = stage.Spot.Activation.AdmitAcceptedReplayJob(
                job,
                stage.SourceMeshName,
                (reply, ct) => CompleteAcceptedSpotRequestAsync(stage, job, reply, ct)
            );
            if (delivery is not null)
                stage.SpotCapturedRequestDeliveries.Add(delivery);
            stage.AdmittedSpotReplayCount++;
        }
    }

    private void AdmitInboundAggregateActorPrefixes(
        TargetStage stage,
        CancellationToken cancellationToken
    )
    {
        var handoffId = stage.Envelope.AggregateId.ToString("N");
        foreach (var actorState in stage.ActorStates)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var participant = stage.Envelope.Participants.Single(candidate =>
                candidate.ObjectKind == ZLinkPlacementObjectKind.Actor
                && candidate.AuthorityKey
                    == ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorState.ActorId)
            );
            var actorRef =
                actorState.NativeActorRef
                ?? throw new ZLinkRelocationDataLostException(
                    $"Actor '{actorState.ActorId}' target reference is missing."
                );
            if (!actorState.Handoff.IsAuthorityCommitted(handoffId))
                actorState.Handoff.MarkAuthorityCommitted(
                    handoffId,
                    participant.ObjectGeneration,
                    actorRef.Generation
                );
            _ = actorState.Handoff.PrepareCanonicalMaintenanceReplay(handoffId);
            var requests = participant
                .AcceptedJobs.Where(static job => job.CanonicalRequest is { ReplyRouteId: not 0 })
                .ToDictionary(static job => checked((long)job.AcceptedSequence));
            AdmitTransferredActorPrefix(
                actorState,
                handoffId,
                stage.TargetMeshName,
                frame =>
                    stage.Spot.Activation.DispatchOwnedActorFrameAsync(
                        actorRef,
                        frame,
                        requests.TryGetValue(frame.ArrivalIndex, out var job)
                            ? (replayed, reply, ct) =>
                                CompleteAcceptedActorReplayAsync(
                                    stage,
                                    participant,
                                    job,
                                    replayed,
                                    reply,
                                    ct
                                )
                            : null,
                        ShutdownToken
                    ),
                frame => requests.ContainsKey(frame.ArrivalIndex),
                acceptedThrough: checked(
                    (long)
                        participant
                            .AcceptedJobs.Select(static job => job.AcceptedSequence)
                            .DefaultIfEmpty()
                            .Max()
                )
            );
        }
    }

    internal async ValueTask OpenInboundSpotAggregateAdmissionAsync(
        TargetStage stage,
        Func<ValueTask> recordDispatchOpening
    )
    {
        var handoffId = stage.Envelope.AggregateId.ToString("N");
        await OpenTargetAdmissionOnceAsync(
                stage,
                () =>
                    stage.Spot.Activation.OpenRelocationTargetAdmissionAsync(
                        stage.TargetAdmissionSeal,
                        static () => { },
                        async () =>
                        {
                            // The FIFO prefix was reserved during publication.
                            // Release it with the Spot only after the phase CAS.
                            await recordDispatchOpening().ConfigureAwait(false);
                            foreach (var actorState in stage.ActorStates)
                                actorState.Handoff.Complete(handoffId);
                        }
                    )
            )
            .ConfigureAwait(false);
        stage.CapturedRequestDelivery ??= Task.WhenAll(
            stage.SpotCapturedRequestDeliveries.Concat(
                stage.ActorStates.Select(actor =>
                    actor.Handoff.CapturedRequestDelivery(handoffId) ?? Task.CompletedTask
                )
            )
        );
        stage.SpotCapturedRequestDeliveries.Clear();
    }

    private async ValueTask CompleteAcceptedActorReplayAsync(
        TargetStage stage,
        ZLinkRelocationParticipantEnvelope participant,
        ZLinkRelocationQueuedJob job,
        ZLinkSpotActorFrame frame,
        ZLinkActorReply? reply,
        CancellationToken cancellationToken
    )
    {
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"canonical_actor_replay_completion_begin actor={frame.Actor.ActorId} accepted_sequence={job.AcceptedSequence} reply={reply is not null} has_reply_route={job.CanonicalRequest is { ReplyRouteId: not 0 }}"
        );
        ZLinkCanonicalTerminalCompletion? completion = null;
        byte[]? replyFrame = null;
        var request = job.CanonicalRequest;
        if (request is { ReplyRouteId: not 0 })
        {
            if (reply is null)
                throw new ZLinkRelocationDataLostException(
                    "Actor accepted request replay produced no terminal reply."
                );
            replyFrame = reply.ToFrame(frame.Header);
            completion = ZLinkRelocationEnvelopeCodec.CreateCanonicalTerminalCompletion(
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
                    EncodeDurableActorReply(request.ReplyRouteId, replyFrame)
                )
            );
        }
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"canonical_actor_replay_handled actor={frame.Actor.ActorId} accepted_sequence={job.AcceptedSequence} has_completion={completion is not null}"
        );
        if (completion is not null)
        {
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"canonical_actor_reply_relay_begin actor={frame.Actor.ActorId} accepted_sequence={job.AcceptedSequence} reply_route={request!.ReplyRouteId}"
            );
            var acknowledgement = await TryRelayCanonicalReplyAsync(
                    RoutingId.FromHex(completion.SourceNodeRid),
                    await CreateCanonicalReplyRelayAsync(
                            Registration.Locations.ResolveStore()
                                ?? throw new ZLinkConfigurationException(
                                    "Location Store is not registered."
                                ),
                            stage,
                            participant,
                            request!,
                            completion,
                            cancellationToken
                        )
                        .ConfigureAwait(false),
                    completion,
                    [replyFrame!],
                    cancellationToken
                )
                .ConfigureAwait(false);
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"canonical_actor_reply_relay_result actor={frame.Actor.ActorId} accepted_sequence={job.AcceptedSequence} acknowledgement={acknowledgement}"
            );
            if (
                !await CompleteCanonicalReplyDeliveryAsync(
                        completion,
                        acknowledgement,
                        cancellationToken
                    )
                    .ConfigureAwait(false)
            )
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    "Actor request reply is waiting for its exact source acknowledgement.",
                    ZLinkRetryAdvice.RetryAfterBackoff
                );
        }
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"canonical_actor_replay_completion_end actor={frame.Actor.ActorId} accepted_sequence={job.AcceptedSequence}"
        );
    }

    private async ValueTask CompleteAcceptedSpotRequestAsync(
        TargetStage stage,
        ZLinkRelocationQueuedJob job,
        byte[][]? capturedReply,
        CancellationToken cancellationToken
    )
    {
        var request = job.CanonicalRequest!;
        if (capturedReply is null)
            throw new ZLinkRelocationDataLostException(
                "Canonical request replay completed without a terminal reply."
            );
        var participant = stage.SpotParticipant;
        var completion = ZLinkRelocationEnvelopeCodec.CreateCanonicalTerminalCompletion(
            request.OperationHigh,
            request.OperationLow,
            request.Source.OwnerId,
            request.Source.OwnerLeaseGeneration,
            request.Source.NodeRid,
            request.Source.NodeGeneration,
            participant.CanonicalParticipantId,
            job.AcceptedSequence,
            terminalResult: 0,
            errorCode: 0,
            DecodeCanonicalReply(capturedReply)
        );

        var authorityStore =
            Registration.Locations.ResolveStore()
            ?? throw new ZLinkConfigurationException("Location Store is not registered.");
        var acknowledged = await TryRelayCanonicalReplyAsync(
                RoutingId.FromHex(completion.SourceNodeRid),
                await CreateCanonicalReplyRelayAsync(
                        authorityStore,
                        stage,
                        participant,
                        request,
                        completion,
                        cancellationToken
                    )
                    .ConfigureAwait(false),
                completion,
                capturedReply,
                cancellationToken
            )
            .ConfigureAwait(false);
        if (
            !await CompleteCanonicalReplyDeliveryAsync(completion, acknowledged, cancellationToken)
                .ConfigureAwait(false)
        )
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                "The canonical request reply has neither an acknowledgement nor exact source lease expiry proof.",
                ZLinkRetryAdvice.RetryAfterBackoff
            );
    }

    private async ValueTask<bool> CompleteCanonicalReplyDeliveryAsync(
        ZLinkCanonicalTerminalCompletion completion,
        ZLinkRelocationReplyAckState acknowledgement,
        CancellationToken cancellationToken
    )
    {
        if (
            acknowledgement
            is ZLinkRelocationReplyAckState.TerminalReceived
                or ZLinkRelocationReplyAckState.AlreadyTerminal
        )
            return true;

        var store =
            Registration.Locations.ResolveStore()
            ?? throw new ZLinkConfigurationException("Location Store is not registered.");
        var lease = await store
            .ReadOwnerLeaseAsync(completion.SourceOwnerId, cancellationToken)
            .ConfigureAwait(false);
        if (!IsExactSourceLeaseExpired(lease, completion))
            return false;
        return true;
    }

    private async ValueTask<ZLinkRelocationReplyAckState> TryRelayCanonicalReplyAsync(
        RoutingId sourceNodeRid,
        ZLinkServiceWireCodec.ReplyRelayRecord relay,
        ZLinkCanonicalTerminalCompletion completion,
        IReadOnlyList<byte[]> payload,
        CancellationToken cancellationToken
    )
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
                        completion.SourceNodeGeneration
                    ),
                    payload,
                    cancellationToken
                )
                .ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch
        {
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"canonical_reply_relay_failed operation={completion.OperationHigh:x16}{completion.OperationLow:x16}"
            );
            return ZLinkRelocationReplyAckState.NotAcknowledged;
        }
    }

    private async ValueTask<ZLinkServiceWireCodec.ReplyRelayRecord> CreateCanonicalReplyRelayAsync(
        IZLinkLocationRepository authorityStore,
        TargetStage stage,
        ZLinkRelocationParticipantEnvelope participant,
        ZLinkCanonicalAcceptedRequest request,
        ZLinkCanonicalTerminalCompletion completion,
        CancellationToken cancellationToken
    )
    {
        var authority = await authorityStore
            .ReadAuthorityAsync(participant.AuthorityKey, cancellationToken)
            .ConfigureAwait(false);
        if (authority is not ZLinkAuthorityReadResult.Found found)
            throw new ZLinkRelocationDataLostException(
                "The canonical reply relay authority is missing."
            );
        var targetOwner =
            LocationLifecycle?.OwnerToken
            ?? throw new ZLinkConfigurationException("Location runtime is not registered.");
        if (
            !ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                found.Snapshot.Payload.Span,
                out var canonical
            )
            || !IsExactCanonicalReplyRelayTarget(
                canonical,
                stage.Envelope.CanonicalRelocationHigh,
                stage.Envelope.CanonicalRelocationLow,
                stage.TargetAttemptGeneration,
                stage.Node.Node.RoutingId,
                stage.TargetNodeLifecycleGeneration,
                targetOwner
            )
        )
            throw new ZLinkRelocationDataLostException(
                "The canonical reply relay target attempt does not match durable authority."
            );
        return new ZLinkServiceWireCodec.ReplyRelayRecord(
            new MeshOperationId(completion.OperationHigh, completion.OperationLow),
            request.ReplyRouteId,
            new ZLinkServiceWireCodec.RelocationWireId(
                stage.Envelope.CanonicalRelocationHigh,
                stage.Envelope.CanonicalRelocationLow
            ),
            canonical.TargetAttemptGeneration,
            new ZLinkServiceWireCodec.RelocationCoordinatorFence(
                targetOwner.OwnerId,
                checked((ulong)targetOwner.LeaseGeneration),
                stage.Node.Node.RoutingId,
                stage.TargetNodeLifecycleGeneration,
                found.Snapshot.StoreVersion
            ),
            completion.ParticipantId,
            completion.AcceptedSequence,
            completion.TerminalResult,
            (ServiceWireConstants.FrameworkErrorCode)completion.ErrorCode
        );
    }

    internal static bool IsExactCanonicalReplyRelayTarget(
        ZLinkCanonicalRelocationAuthorityProjection canonical,
        ulong relocationHigh,
        ulong relocationLow,
        ulong targetAttemptGeneration,
        RoutingId targetNodeRid,
        ulong targetNodeGeneration,
        ZLinkLocationOwnerToken targetOwner
    ) =>
        targetAttemptGeneration != 0
        && canonical.TargetAttemptGeneration == targetAttemptGeneration
        && canonical.RelocationHigh == relocationHigh
        && canonical.RelocationLow == relocationLow
        && canonical.State.TargetNodeRid == targetNodeRid.ToHex()
        && canonical.State.TargetNodeGeneration == targetNodeGeneration
        && canonical.TargetOwnerId == targetOwner.OwnerId
        && targetOwner.LeaseGeneration > 0
        && canonical.TargetOwnerLeaseGeneration == checked((ulong)targetOwner.LeaseGeneration);

    internal static bool IsExactSourceLeaseExpired(
        ZLinkOwnerLeaseReadResult lease,
        ZLinkCanonicalTerminalCompletion completion
    ) =>
        lease switch
        {
            ZLinkOwnerLeaseReadResult.Missing => true,
            ZLinkOwnerLeaseReadResult.Found found => completion.SourceOwnerLeaseGeneration
                > long.MaxValue
                || !StringComparer.Ordinal.Equals(found.Token.OwnerId, completion.SourceOwnerId)
                || found.Token.LeaseGeneration
                    != checked((long)completion.SourceOwnerLeaseGeneration)
                || found.LeaseExpiresAt <= found.StoreNow,
            _ => false,
        };

    private static ZLinkCanonicalApplicationPayload DecodeCanonicalReply(
        IReadOnlyList<byte[]> replyParts
    )
    {
        var parts = ZLinkRawRouterServicePort.CreateMessages(
            replyParts,
            static part => Message.From(part)
        );
        try
        {
            var header = ZLinkEnvelopeCodec.DecodeHeader(parts);
            if (parts.Length != 2)
                throw new ZLinkRelocationDataLostException(
                    "Canonical terminal reply must contain header and body parts."
                );
            return new ZLinkCanonicalApplicationPayload(
                header.MessageName,
                header.ContentType,
                parts[1].ToArray()
            );
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }
    }

    internal async ValueTask AbortInboundSpotAggregateAsync(TargetStage stage)
    {
        if (Volatile.Read(ref stage.Published) != 0)
            return;
        foreach (var actorState in stage.ActorStates)
        {
            actorState.Handoff.AbortImport(stage.Envelope.AggregateId.ToString("N"));
            await _actorSessionManager
                .RollbackTransferredActorAsync(actorState.ActorId, CancellationToken.None)
                .ConfigureAwait(false);
        }
        await stage.Node.Catalog.DiscardReservedAsync(stage.Spot).ConfigureAwait(false);
    }

    internal ValueTask RelayCommittedSpotRecordsAsync(
        ZLinkSpotRetireReservation reservation,
        ZLinkAggregateRelocationPublished relocation,
        IReadOnlyList<ZLinkAcceptedWorkRecord> held,
        CancellationToken cancellationToken
    )
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
