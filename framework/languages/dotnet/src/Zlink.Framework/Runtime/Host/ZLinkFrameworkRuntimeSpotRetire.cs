using System.Collections.Concurrent;
using Systems.Zlink.Framework.Runtime.Protocol;

namespace Zlink.Framework.Runtime.Host;

internal sealed partial class ZLinkFrameworkRuntime
{
    internal bool TryAcquireInboundSpotRelocation(
        long payloadBytes,
        bool restore,
        bool allowOversizedPayload,
        out IDisposable permit)
    {
        if (_drainAdmission.IsDraining)
        {
            permit = null!;
            return false;
        }
        if (_relocationPermits.TryAcquire(
                ZLinkRelocationPermitRequest.Inbound(
                    payloadBytes,
                    restore,
                    allowOversizedPayload),
                out var acquired))
        {
            permit = acquired;
            return true;
        }
        permit = null!;
        return false;
    }

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
        IDisposable inboundPermit,
        ZLinkPreparedAggregateRelocation? preparedAggregate,
        ulong reservedTargetAuthorityOwnerGeneration,
        CancellationToken cancellationToken)
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
                    preparedAggregate,
                    spotParticipant,
                    reservedTargetAuthorityOwnerGeneration,
                    cancellationToken)
                .ConfigureAwait(false);
        PreparedReservedSpot preparedSpot;
        if (request.InstanceSpot)
        {
            preparedSpot = await node.Catalog.PrepareInstanceReservedAsync(
                    request.StableType,
                    request.SpotId,
                    spotParticipant.ObjectGeneration,
                    targetOwnerGeneration,
                    cancellationToken,
                    restoreLogicalTimers: true)
                .ConfigureAwait(false);
        }
        else
        {
            if (!node.Registration.SpotRelocations.TryGetValue(
                    request.StableType,
                    out var spotRegistration))
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.TypeMismatch,
                    $"User SPOT type '{request.StableType}' is not registered on target node.");
            preparedSpot = await node.Catalog.PrepareReservedAsync(
                    spotRegistration.InstanceType,
                    request.SpotId,
                    spotParticipant.ObjectGeneration,
                    targetOwnerGeneration,
                    ZLinkMessage.Empty,
                    cancellationToken,
                    invokeCreate: false)
                .ConfigureAwait(false);
        }

        var actorStates = new List<ZLinkActorRuntimeState>();
        var actorTargetAuthorityOwnerGenerations =
            new Dictionary<string, ulong>(StringComparer.Ordinal);
        var boundActorIds =
            new ConcurrentDictionary<string, byte>(
                StringComparer.Ordinal);
        var stagedActorStates =
            new ConcurrentDictionary<string, ZLinkActorRuntimeState>(
                StringComparer.Ordinal);
        ZLinkSpotRelocationSeal? targetAdmissionSeal = null;
        try
        {
            await preparedSpot.Activation.RestoreSpotRelocationStateAsync(
                    spotParticipant.ApplicationState,
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
                            preparedAggregate is null
                            && reservedTargetAuthorityOwnerGeneration == 0
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
                                    preparedAggregate,
                                    participant,
                                    reservedTargetAuthorityOwnerGeneration: 0,
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
                        boundActorIds.TryAdd(authority.ActorId, 0);
                        var actorState = GetOrCreateActorState(authority.ActorId);
                        stagedActorStates.TryAdd(
                            authority.ActorId,
                            actorState);
                        actorState.StageRelocationSessionRoute(
                            envelope.AggregateId.ToString("N"),
                            hasRelocation
                                ? relocationAuthority.BoundSessionRoute
                                : default);
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
                    restored.State.ActorId,
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
                request.RelocationReference,
                request.RelocationChecksum,
                DateTimeOffset.UtcNow
                + ZLinkSpotRetireTargetRuntime.StageRetention,
                ZLinkSpotRetireTargetRuntime.ComputeStageRequestDigest(
                    request),
                targetAdmissionSeal
                ?? throw new InvalidOperationException(
                    "Target staging admission seal was not created."),
                inboundPermit,
                preparedAggregate,
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
                            actorId,
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

    private async ValueTask<ulong>
        ResolveInboundTargetAuthorityOwnerGenerationAsync(
            ZLinkPreparedAggregateRelocation? preparedAggregate,
            ZLinkRelocationParticipantEnvelope participant,
            ulong reservedTargetAuthorityOwnerGeneration,
            CancellationToken cancellationToken)
    {
        if (preparedAggregate is null
            && reservedTargetAuthorityOwnerGeneration == 0)
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
            : preparedAggregate is not null
            ? preparedAggregate.TargetAuthorityOwnerGeneration(
                participant.AuthorityKey)
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
                var sessionCommits =
                    new List<(ZLinkActorRuntimeState State,
                        ZLinkSessionRouteCommitRequest Request,
                        RoutingId SessionOwnerNode)>();
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
                            actorState.ActorId),
                        stage.TargetMeshName,
                        stage.TargetNodeLifecycleGeneration,
                        stage.TargetOwnerLeaseGeneration);
                    var sessionCommit = await CommitCompletedSessionRouteAsync(
                            actorState,
                            handoffId,
                            cancellationToken)
                        .ConfigureAwait(false);
                    if (sessionCommit is not null)
                        sessionCommits.Add((
                            actorState,
                            sessionCommit.Value.Request,
                            sessionCommit.Value.SessionOwnerNode));
                }

                if (Volatile.Read(
                        ref stage.RelocationReadyCompletionDelivered) == 0)
                {
                    await stage.Spot.Activation
                        .InvokeTargetRelocationReadyCompletedAsync(
                            stage.TargetAdmissionSeal,
                            cancellationToken)
                        .ConfigureAwait(false);
                    Volatile.Write(
                        ref stage.RelocationReadyCompletionDelivered,
                        1);
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
                foreach (var (actorState, sessionCommit, sessionOwnerNode)
                         in sessionCommits)
                {
                    await UnsealCompletedSessionRouteAsync(
                            sessionCommit,
                            sessionOwnerNode,
                            cancellationToken)
                        .ConfigureAwait(false);
                    actorState.CompleteRelocationSessionRoute(handoffId);
                }
                Volatile.Write(ref stage.Published, 1);
            }
            else if (normalizeAuthority is not null)
            {
                await normalizeAuthority(cancellationToken)
                    .ConfigureAwait(false);
            }

            // Command 34 publishes authority and the local catalog. Direct
            // execution stays sealed until command 35 proves that the source
            // finished its pre-cutover Message Follow drain.
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
        if (Volatile.Read(ref stage.Published) == 0)
            throw new InvalidOperationException(
                "Target admission cannot open before the staged journal is published.");
        if (Volatile.Read(ref stage.AdmissionOpened) != 0)
            return;
        if (!openAdmission())
            throw new InvalidOperationException(
                $"Target SPOT '{stage.Spot.Activation.SpotId}' lost its staging admission seal.");
        Volatile.Write(ref stage.AdmissionOpened, 1);
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
        if (!stage.Envelope.CanonicalLogicalStream.IsEmpty)
            await RestoreCanonicalReplayProgressAsync(
                    stage,
                    replayJobs,
                    cancellationToken)
                .ConfigureAwait(false);
        var replayed = Volatile.Read(ref stage.ReplayedJobCount);
        if (replayed > replayJobs.Length)
            throw new ZLinkRelocationDataLostException(
                $"Target SPOT '{stage.Spot.Activation.SpotId}' replay cursor exceeds its accepted journal.");
        if (replayed == replayJobs.Length)
            return;
        var authorityStore = Registration.Locations.ResolveStore()
                             ?? throw new ZLinkConfigurationException(
                                 "Location Store is not registered.");
        var relocationStore = Registration.Locations.ResolveRelocationStore()
                              ?? throw new ZLinkConfigurationException(
                                  "Relocation Store is not registered.");
        var replayCoordinator = new ZLinkAggregateRelocationCoordinator(
            authorityStore,
            relocationStore);
        await stage.Spot.Activation.ReplayAcceptedJobsAsync(
                replayJobs,
                stage.SourceMeshName,
                stage.TargetAdmissionSeal,
                Volatile.Read(ref stage.ReplayedJobCount),
                (job, journal, capturedReply, ct) =>
                    CompleteAcceptedReplayAsync(
                        stage,
                        job,
                        journal,
                        capturedReply,
                        replayCoordinator,
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
        var targetDescriptor = new ZLinkMeshNodeDescriptorKey(
            stage.TargetMeshName,
            stage.Node.Node.RoutingId);
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
            actorState.Handoff.AcknowledgeCanonicalReplayThrough(
                participant.ReplayCursor);

            foreach (var completion in participant.TerminalCompletions.Where(
                         static completion => completion.DeliveryState == 0))
            {
                var job = participant.AcceptedJobs.Single(candidate =>
                    candidate.AcceptedSequence
                    == completion.AcceptedSequence);
                var request = job.CanonicalRequest
                              ?? throw new ZLinkRelocationDataLostException(
                                  "Actor pending reply has no accepted request.");
                var (_, replyFrame) = DecodeDurableActorReply(completion);
                var acknowledgement = await TryRelayCanonicalReplyAsync(
                        RoutingId.FromHex(completion.SourceNodeRid),
                        await CreateCanonicalReplyRelayAsync(
                                authorityStore,
                                stage,
                                participant,
                                request,
                                completion,
                                cancellationToken)
                            .ConfigureAwait(false),
                        completion,
                        [replyFrame],
                        cancellationToken)
                    .ConfigureAwait(false);
                if (!await CompleteCanonicalReplyDeliveryAsync(
                        stage,
                        completion,
                        acknowledgement,
                        coordinator,
                        targetDescriptor,
                        targetOwner,
                        cancellationToken)
                        .ConfigureAwait(false))
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.Unavailable,
                        "Actor pending reply is waiting for its exact source acknowledgement.",
                        ZLinkRetryAdvice.RetryAfterBackoff);
            }

            var barrier = actorState.ReserveHandoffRestoreBarrier();
            var turn = await barrier.ClaimAsync().ConfigureAwait(false);
            var queued = new List<Task>();
            Task<bool> durableCompletionTail = Task.FromResult(true);
            try
            {
                foreach (var job in participant.AcceptedJobs
                             .Where(job => job.AcceptedSequence
                                 > participant.ReplayCursor)
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
                        var previousDurableCompletion =
                            durableCompletionTail;
                        var currentDurableCompletion =
                            new TaskCompletionSource<bool>(
                                TaskCreationOptions
                                    .RunContinuationsAsynchronously);
                        durableCompletionTail =
                            currentDurableCompletion.Task;
                        queued.Add(pipeline.QueueCanonicalReplayAsync(
                            batch,
                            async (frame, reply, ct) =>
                            {
                                try
                                {
                                    if (!await previousDurableCompletion
                                            .ConfigureAwait(false))
                                        throw new ZLinkFrameworkException(
                                            ZLinkFrameworkErrorKind.Unavailable,
                                            "Actor replay is waiting for an earlier durable completion.",
                                            ZLinkRetryAdvice.RetryAfterBackoff);
                                    await CompleteAcceptedActorReplayAsync(
                                            stage,
                                            participant,
                                            actorState,
                                            job,
                                            frame,
                                            reply,
                                            coordinator,
                                            targetDescriptor,
                                            targetOwner,
                                            ct)
                                        .ConfigureAwait(false);
                                    currentDurableCompletion
                                        .TrySetResult(true);
                                }
                                catch
                                {
                                    currentDurableCompletion
                                        .TrySetResult(false);
                                    throw;
                                }
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
            // Keep target capture closed until command 35 confirms source
            // cleanup; completion then orders followed frames before direct
            // new-owner ingress and opens admission once.
        }
    }

    internal async ValueTask CompleteInboundSpotAggregateAsync(
        TargetStage stage,
        CancellationToken cancellationToken)
    {
        await CompleteInboundSpotAggregateReplayAsync(
                stage,
                cancellationToken)
            .ConfigureAwait(false);
        await OpenInboundSpotAggregateAdmissionAsync(stage)
            .ConfigureAwait(false);
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
            var queuedThrough = checked((long)Math.Max(
                participant.ReplayCursor,
                participant.AcceptedBoundary));
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
                        var queuedThrough = checked((long)Math.Max(
                            participant.ReplayCursor,
                            participant.AcceptedBoundary));
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
        ZLinkAggregateRelocationCoordinator coordinator,
        ZLinkMeshNodeDescriptorKey targetDescriptor,
        ZLinkLocationOwnerToken targetOwner,
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
                    0,
                    new ZLinkCanonicalApplicationPayload(
                        frame.Header.Name,
                        DurableActorReplyContentType,
                        EncodeDurableActorReply(
                            request.ReplyRouteId,
                            replyFrame)));
        }
        _ = await coordinator.AdvanceCanonicalReplayAsync(
                stage.Envelope,
                participant.CanonicalParticipantId,
                job.AcceptedSequence,
                completion,
                targetDescriptor,
                stage.TargetNodeLifecycleGeneration,
                targetOwner,
            cancellationToken)
            .ConfigureAwait(false);
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"canonical_actor_replay_advanced actor={actorState.ActorId} "
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
                    stage,
                    completion,
                    acknowledgement,
                    coordinator,
                    targetDescriptor,
                    targetOwner,
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

    private async ValueTask RestoreCanonicalReplayProgressAsync(
        TargetStage stage,
        IReadOnlyList<ZLinkRelocationQueuedJob> replayJobs,
        CancellationToken cancellationToken)
    {
        var authorityStore = Registration.Locations.ResolveStore()
                             ?? throw new ZLinkConfigurationException(
                                 "Location Store is not registered.");
        var relocationStore = Registration.Locations.ResolveRelocationStore()
                              ?? throw new ZLinkConfigurationException(
                                  "Relocation Store is not registered.");
        var targetOwner = LocationLifecycle?.OwnerToken
                          ?? throw new ZLinkConfigurationException(
                              "Location runtime is not registered.");
        var targetDescriptor = new ZLinkMeshNodeDescriptorKey(
            stage.TargetMeshName,
            stage.Node.Node.RoutingId);
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            authorityStore,
            relocationStore);
        var current = await coordinator.ReadCanonicalRootAsync(
                stage.Envelope,
                targetOwner,
                cancellationToken)
            .ConfigureAwait(false);
        var participant = current.Participants.Single(static candidate =>
            candidate.ObjectKind is ZLinkPlacementObjectKind.UserSpot
                or ZLinkPlacementObjectKind.InstanceSpot);

        foreach (var completion in participant.TerminalCompletions
                     .Where(static candidate => candidate.DeliveryState == 0))
        {
            var job = replayJobs.SingleOrDefault(candidate =>
                candidate.AcceptedSequence == completion.AcceptedSequence
                && candidate.CanonicalRequest is { } request
                && request.OperationHigh == completion.OperationHigh
                && request.OperationLow == completion.OperationLow)
                      ?? throw new ZLinkRelocationDataLostException(
                          "Canonical pending reply has no matching accepted request.");
            var request = job.CanonicalRequest!;
            var replyParts = EncodeCanonicalReply(
                stage.SourceMeshName,
                request,
                completion);
            var acknowledged = await TryRelayCanonicalReplyAsync(
                    RoutingId.FromHex(completion.SourceNodeRid),
                    await CreateCanonicalReplyRelayAsync(
                            authorityStore,
                            stage,
                            participant,
                            request,
                            completion,
                            cancellationToken)
                        .ConfigureAwait(false),
                    completion,
                    replyParts,
                    cancellationToken)
                .ConfigureAwait(false);
            if (!await CompleteCanonicalReplyDeliveryAsync(
                    stage,
                    completion,
                    acknowledged,
                    coordinator,
                    targetDescriptor,
                    targetOwner,
                    cancellationToken)
                .ConfigureAwait(false))
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    "The canonical pending reply has neither an acknowledgement nor exact source lease expiry proof.",
                    ZLinkRetryAdvice.RetryAfterBackoff);
        }

        Interlocked.Exchange(
            ref stage.ReplayedJobCount,
            replayJobs.Count(job => job.AcceptedSequence <= participant.ReplayCursor));
    }

    private async ValueTask CompleteAcceptedReplayAsync(
        TargetStage stage,
        ZLinkRelocationQueuedJob job,
        ZLinkSpotAcceptedJournalRecord journal,
        byte[][]? capturedReply,
        ZLinkAggregateRelocationCoordinator coordinator,
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
                    deliveryState: 0,
                    DecodeCanonicalReply(capturedReply));
        }

        var authorityStore = Registration.Locations.ResolveStore()
                             ?? throw new ZLinkConfigurationException(
                                 "Location Store is not registered.");
        var targetOwner = LocationLifecycle?.OwnerToken
                          ?? throw new ZLinkConfigurationException(
                              "Location runtime is not registered.");
        var targetDescriptor = new ZLinkMeshNodeDescriptorKey(
            stage.TargetMeshName,
            stage.Node.Node.RoutingId);
        _ = await coordinator.AdvanceCanonicalReplayAsync(
                stage.Envelope,
                stage.Envelope.Participants.Single(static participant =>
                        participant.ObjectKind
                        is ZLinkPlacementObjectKind.UserSpot
                        or ZLinkPlacementObjectKind.InstanceSpot)
                    .CanonicalParticipantId,
                job.AcceptedSequence,
                completion,
                targetDescriptor,
                stage.TargetNodeLifecycleGeneration,
                targetOwner,
                cancellationToken)
            .ConfigureAwait(false);

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
                    stage,
                    completion,
                    acknowledged,
                    coordinator,
                    targetDescriptor,
                    targetOwner,
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
        TargetStage stage,
        ZLinkCanonicalTerminalCompletion completion,
        ZLinkRelocationReplyAckState acknowledgement,
        ZLinkAggregateRelocationCoordinator coordinator,
        ZLinkMeshNodeDescriptorKey targetDescriptor,
        ZLinkLocationOwnerToken targetOwner,
        CancellationToken cancellationToken)
    {
        if (acknowledgement is
            ZLinkRelocationReplyAckState.TerminalReceived
            or ZLinkRelocationReplyAckState.AlreadyTerminal)
        {
            var acknowledged = await coordinator.AcknowledgeCanonicalReplyAsync(
                    stage.Envelope,
                    completion,
                    acknowledgement
                    == ZLinkRelocationReplyAckState.AlreadyTerminal
                        ? (byte)2
                        : (byte)1,
                    targetDescriptor,
                    stage.TargetNodeLifecycleGeneration,
                    targetOwner,
                    cancellationToken)
                .ConfigureAwait(false);
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"canonical_reply_acknowledged operation={completion.OperationHigh:x16}{completion.OperationLow:x16} "
                + $"terminal={acknowledged.Participants.Sum(static participant => participant.TerminalCompletions.Count)} "
                + $"pending={acknowledged.Participants.Sum(static participant => participant.PendingRelayCount)}");
            return true;
        }

        var store = Registration.Locations.ResolveStore()
                    ?? throw new ZLinkConfigurationException(
                        "Location Store is not registered.");
        var lease = await store.ReadOwnerLeaseAsync(
                completion.SourceOwnerId,
                cancellationToken)
            .ConfigureAwait(false);
        if (!IsExactSourceLeaseExpired(lease, completion))
            return false;
        _ = await coordinator.ExpireCanonicalReplySourceLeaseAsync(
                stage.Envelope,
                completion,
                targetDescriptor,
                stage.TargetNodeLifecycleGeneration,
                targetOwner,
                cancellationToken)
            .ConfigureAwait(false);
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
