using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime.Backend.DotNet.Mappings;
using Zlink.Framework.Runtime.Identifiers;

namespace Zlink.Framework.Runtime.Host;

internal sealed class ZLinkActorRemoteJoiner(
    ZLinkFrameworkRuntime runtime,
    ZLinkFrameworkRegistration registration,
    IServiceProvider services,
    ZLinkSpotRuntimeManager spots,
    ZLinkActorSessionManager actorSessionManager)
{
    internal ValueTask<ZLinkActorJoinResult> JoinEntrySpotAsync(
        ZLinkMeshNodeDescriptor target,
        IZLinkActor actor,
        ZLinkBackendActorRef actorRef,
        ZLinkMessage request,
        ZLinkActorJoinOperationId? operationId,
        CancellationToken cancellationToken,
        DateTimeOffset? absoluteDeadline = null)
    {
        if (string.IsNullOrEmpty(target.EntrySpotId)
            || target.LifecycleGeneration == 0
            || target.LeaseGeneration <= 0)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.NotFound,
                "The selected Entry Spot descriptor is incomplete.");
        var snapshot = new ZLinkSpotHandleSnapshot(
            target.MeshName,
            target.Rid,
            target.EntrySpotId,
            target.LifecycleGeneration,
            ZLinkSpotKind.Entry,
            target.LifecycleGeneration,
            target.LifecycleGeneration,
            checked((ulong)target.LeaseGeneration));
        var handle = new ZLinkResolvedSpotHandle(
            snapshot,
            target.DescriptorRevision,
            _ => ValueTask.FromResult<
                (ZLinkSpotHandleSnapshot Snapshot, ulong Version)?>(null));
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"entry_target_resolved node={target.Rid} spot={target.EntrySpotId} "
            + $"spot_gen={snapshot.Generation} node_gen={snapshot.NodeGeneration} "
            + $"authority_gen={snapshot.AuthorityOwnerGeneration} "
            + $"lease_gen={snapshot.OwnerLeaseGeneration} "
            + $"descriptor_revision={target.DescriptorRevision}");
        return SubmitRoutedJoinActorAsync(
            actor,
            actorRef,
            actorSessionManager.GetOrCreateState(actor.Context.ActorId),
            handle,
            request,
            operationId,
            cancellationToken,
            absoluteDeadline);
    }

    public ValueTask<ZLinkActorJoinResult> JoinAsync(
        ZLinkFrameworkComponentState state,
        string spotId,
        IZLinkActor actor,
        ZLinkBackendActorRef actorRef,
        IZLinkBackendSpotNode node,
        ZLinkMessage request,
        CancellationToken cancellationToken,
        DateTimeOffset? absoluteDeadline = null)
    {
        return JoinAsync(
            state,
            spotId,
            actor,
            actorRef,
            node,
            request,
            operationId: null,
            cancellationToken,
            absoluteDeadline);
    }

    public async ValueTask<ZLinkActorJoinResult> JoinAsync(
        ZLinkFrameworkComponentState state,
        string spotId,
        IZLinkActor actor,
        ZLinkBackendActorRef actorRef,
        IZLinkBackendSpotNode node,
        ZLinkMessage request,
        ZLinkActorJoinOperationId? operationId,
        CancellationToken cancellationToken,
        DateTimeOffset? absoluteDeadline = null)
    {
        using var flow = ZLinkFlowContext.EnterCurrentOrCreate(
            ZLinkFlowOrigin.Application,
            runtime.Flow.CaptureEnabled);
        var activation = spots.GetActivationBySpotId(state, spotId);
        if (activation is not null)
        {
            if (!activation.TryResolveActorJoinDescriptor(out var descriptor) || descriptor is null)
                throw new InvalidOperationException(
                    $"SPOT '{activation.SpotId}' does not declare an actor join callback.");

            return await SubmitNativeJoinActorAsync(
                    actor,
                    actorRef,
                    node,
                    activation.NodeRid,
                    activation.SpotId,
                    activation.ChannelName,
                    request,
                    cancellationToken)
                .ConfigureAwait(false);
        }

        var remoteAddress = await ResolveRemoteActorJoinTargetAsync(
                spotId,
                cancellationToken)
            .ConfigureAwait(false);
        return await SubmitRoutedJoinActorAsync(
                actor,
                actorRef,
                actorSessionManager.GetOrCreateState(actor.Context.ActorId),
                remoteAddress,
                request,
                operationId,
                cancellationToken,
                absoluteDeadline)
            .ConfigureAwait(false);
    }

    private async ValueTask<ZLinkActorJoinResult> SubmitRoutedJoinActorAsync(
        IZLinkActor actor,
        ZLinkBackendActorRef actorRef,
        ZLinkActorRuntimeState actorState,
        ZLinkResolvedSpotHandle target,
        ZLinkMessage request,
        ZLinkActorJoinOperationId? operationId,
        CancellationToken cancellationToken,
        DateTimeOffset? absoluteDeadline = null)
    {
        if (string.IsNullOrWhiteSpace(actorState.ActorType))
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.NotFound,
                $"Actor '{actor.Context.ActorId}' does not have an actor type for remote SPOT join.");

        var actorType = actorState.ActorType;
        var handoffId = Guid.NewGuid().ToString("N");
        ZLinkActorRelocationRegistry.TryResolve(
            registration,
            actorType,
            actorRef.NodeRid,
            out var relocation);
        if (relocation is null)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Rejected,
                $"Actor type '{actorType}' relocation policy is not registered on the source node.");

        var effectiveDeadline = absoluteDeadline
                                ?? DateTimeOffset.UtcNow
                                    + registration.DefaultRequestTimeout;
        var timeout = RemainingTimeout(effectiveDeadline);
        return await ExecuteWithDeadlineAsync(
                timeoutToken => SubmitRoutedJoinActorCoreAsync(
                    actor,
                    actorRef,
                    actorState,
                    target,
                    request,
                    actorType,
                    handoffId,
                    relocation,
                    operationId,
                    effectiveDeadline,
                    timeoutToken,
                    cancellationToken),
                timeout,
                cancellationToken)
            .ConfigureAwait(false);
    }

    internal static async ValueTask<TResult> ExecuteWithDeadlineAsync<TResult>(
        Func<CancellationToken, ValueTask<TResult>> operation,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        using var timeoutSource = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timeoutSource.CancelAfter(timeout);
        try
        {
            return await operation(timeoutSource.Token).ConfigureAwait(false);
        }
        catch (OperationCanceledException timeoutError) when (
            !cancellationToken.IsCancellationRequested
            && timeoutSource.IsCancellationRequested)
        {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.DeadlineExceeded,
                $"Actor relocation timed out after {timeout}.",
                innerException: timeoutError);
        }
    }

    private static TimeSpan RemainingTimeout(DateTimeOffset absoluteDeadline)
    {
        var remaining = absoluteDeadline - DateTimeOffset.UtcNow;
        if (remaining <= TimeSpan.Zero)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.DeadlineExceeded,
                "Actor Join deadline elapsed before remote admission.",
                ZLinkRetryAdvice.RetryAfterBackoff);
        return remaining;
    }

    private async ValueTask<ZLinkActorJoinResult> SubmitRoutedJoinActorCoreAsync(
        IZLinkActor actor,
        ZLinkBackendActorRef actorRef,
        ZLinkActorRuntimeState actorState,
        ZLinkResolvedSpotHandle target,
        ZLinkMessage request,
        string actorType,
        string handoffId,
        ZLinkObjectRelocationRegistration relocation,
        ZLinkActorJoinOperationId? operationId,
        DateTimeOffset absoluteDeadline,
        CancellationToken cancellationToken,
        CancellationToken admissionCancellationToken)
    {
        var authorityStore = registration.Locations.ResolveStore()
                             ?? throw new InvalidOperationException(
                                 "Actor handoff requires a location store.");
        var authorityRead = await authorityStore.ReadAuthorityAsync(
                ZLinkActorAuthorityPayloadCodec.AuthorityKey(actor.Context.ActorId),
                cancellationToken)
            .ConfigureAwait(false);
        if (authorityRead is not ZLinkAuthorityReadResult.Found authority
            || authority.Snapshot.ObjectGeneration != actorRef.Generation
            || !ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
                authority.Snapshot.Payload.Span,
                out var sourceAuthority)
            || sourceAuthority.NodeRid != actorRef.NodeRid
            || sourceAuthority.NodeGeneration == 0)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                $"Actor '{actor.Context.ActorId}' authority changed before handoff.");
        var actorAuthorityOwnerGeneration =
            authority.Snapshot.AuthorityOwnerGeneration;
        var captureRequired = relocation.PolicyKind == 2;
        var predictedPayloadBytes =
            ZLinkRemoteActorJoinPackets.MeasurePredictedRelocationPayloadBytes(
                request,
                registration.Codecs,
                captureRequired);
        var targetAccepted = false;
            ZLinkActorJoinResult.Accepted? committedResult = null;
            TargetAdmissionReservationRoute? targetReservationRoute = null;
            var sourceActivation = actorState.LiveActivation;
            var sourceLeft = false;
            var sourceCaptureStarted = false;
            //  Spec 25 §5: the interruption histogram covers an Actor unit from
            //  its admission seal to the target's admission-open ACK. The source
            //  seals when capture starts and the target acknowledges by
            //  accepting, so those two callbacks bound the window.
            var interruption = Diagnostics.ZLinkRelocationInterruptionOperation.Disabled;
            var relocationMetric = ZLinkRuntimeMetrics.CreateRelocation(
                actor.Context.MeshName,
                ZLinkRelocationMetricObjectKind.Actor,
                relocation.PolicyKind != 2
                    ? ZLinkRelocationMetricPolicy.Recreate
                    : ZLinkRelocationMetricPolicy.Snapshot);
            try
            {
                var result = await SubmitRoutedJoinActorTransactionAsync(
                        actor,
                        actorRef,
                        actorState,
                        target,
                        request,
                        actorType,
                        handoffId,
                        relocation,
                        authorityStore,
                        authority.Snapshot,
                        sourceAuthority,
                        actorAuthorityOwnerGeneration,
                        predictedPayloadBytes,
                        operationId,
                        absoluteDeadline,
                        reservation => targetReservationRoute = reservation,
                        (acceptedRef, acceptedReply) =>
                        {
                            targetAccepted = true;
                            committedResult = new ZLinkActorJoinResult.Accepted(
                                acceptedRef.ToNative(sourceAuthority.MeshName),
                                acceptedReply);
                            interruption.Complete();
                        },
                        () =>
                        {
                            sourceCaptureStarted = true;
                            interruption = runtime.RelocationInterruption.Start(
                                Diagnostics.ZLinkRelocationUnitKind.Actor);
                        },
                        () => sourceLeft = true,
                        relocationMetric,
                        cancellationToken,
                        admissionCancellationToken)
                    .ConfigureAwait(false);
                if (result is not ZLinkActorJoinResult.Accepted)
                {
                    if (targetReservationRoute is { } reservation)
                        await AbortTargetReservationBestEffortAsync(reservation)
                            .ConfigureAwait(false);
                    await RollbackSourceHandoffAsync(
                            actor,
                            actorState,
                            sourceActivation,
                            sourceLeft,
                            sourceCaptureStarted)
                        .ConfigureAwait(false);
                    relocationMetric.Complete(ZLinkRelocationMetricOutcome.Aborted);
                }
                else
                    relocationMetric.Complete(ZLinkRelocationMetricOutcome.Completed);

                return result;
            }
            catch (Exception transactionFailure)
            {
                if (!targetAccepted)
                {
                    try
                    {
                        if (targetReservationRoute is { } reservation)
                            await AbortTargetReservationBestEffortAsync(reservation)
                                .ConfigureAwait(false);
                        await AbortBoundSessionRouteSealBestEffortAsync(
                                actor.Context.ActorId,
                                actorState,
                                targetReservationRoute?.HandoffId ?? handoffId)
                            .ConfigureAwait(false);
                        await RollbackSourceHandoffAsync(
                                actor,
                                actorState,
                                sourceActivation,
                                sourceLeft,
                                sourceCaptureStarted)
                            .ConfigureAwait(false);
                    }
                    catch (Exception rollbackFailure)
                    {
                        relocationMetric.Complete(ZLinkRelocationMetricOutcome.Failed);
                        throw new AggregateException(transactionFailure, rollbackFailure);
                    }
                }
                else if (committedResult is { } committed)
                {
                    // Target admission is the commit boundary. A caller
                    // cancellation after this point must not be reported as a
                    // source failure because authority has already moved to
                    // the target. The durable target-side reconciliation owns
                    // any remaining completion work.
                    relocationMetric.Complete(ZLinkRelocationMetricOutcome.Failed);
                    runtime.LogActorHandoff(
                        $"post_commit_reconciliation_deferred actor={actor.Context.ActorId}");
                    return committed;
                }
                relocationMetric.Complete(
                    IsRuntimeShutdown(transactionFailure)
                        ? ZLinkRelocationMetricOutcome.Shutdown
                        : ZLinkRelocationMetricOutcome.Failed);
                throw;
        }
    }

    private async ValueTask RollbackSourceHandoffAsync(
        IZLinkActor actor,
        ZLinkActorRuntimeState actorState,
        ZLinkSpotActivation? sourceActivation,
        bool sourceLeft,
        bool sourceCaptureStarted)
    {
        List<Exception>? failures = null;
        if (sourceLeft && sourceActivation is not null)
        {
            try
            {
                await sourceActivation.RestoreActorAfterFailedHandoffAsync(
                        actor,
                        CancellationToken.None)
                    .ConfigureAwait(false);
            }
            catch (Exception exception)
            {
                (failures ??= []).Add(exception);
            }
        }

        if (sourceCaptureStarted)
        {
            try
            {
                await ReplayAbortedSourceHandoffAsync(actorState).ConfigureAwait(false);
            }
            catch (Exception exception)
            {
                (failures ??= []).Add(exception);
            }
        }

        if (failures is { Count: > 0 })
            throw new AggregateException("Actor handoff source rollback failed.", failures);
    }

    private async ValueTask<ZLinkActorJoinResult> SubmitRoutedJoinActorTransactionAsync(
        IZLinkActor actor,
        ZLinkBackendActorRef actorRef,
        ZLinkActorRuntimeState actorState,
        ZLinkResolvedSpotHandle target,
        ZLinkMessage request,
        string actorType,
        string handoffId,
        ZLinkObjectRelocationRegistration relocation,
        IZLinkLocationRepository authorityStore,
        ZLinkAuthoritySnapshot sourceAuthoritySnapshot,
        ZLinkActorAuthorityPayload sourceAuthority,
        ulong actorAuthorityOwnerGeneration,
        long predictedPayloadBytes,
        ZLinkActorJoinOperationId? operationId,
        DateTimeOffset absoluteDeadline,
        Action<TargetAdmissionReservationRoute> setTargetReservation,
        Action<ZLinkBackendActorRef, ZLinkMessage> setTargetAccepted,
        Action markSourceCaptureStarted,
        Action markSourceLeft,
        ZLinkRuntimeMetrics.ZLinkRelocationMetricOperation relocationMetric,
        CancellationToken cancellationToken,
        CancellationToken admissionCancellationToken)
    {
        var sourceSpotId = ResolveSourceSpotId(sourceAuthority);
        var sourceActivation = actorState.LiveActivation;

        var admissionDeadline = absoluteDeadline;
        var sourceNode = runtime.GetSpotNodeRuntime(actorRef.NodeRid);
        var canonicalRequest = CreateCanonicalActorJoinRequest(
            actorRef,
            sourceAuthority,
            sourceAuthoritySnapshot,
            target.Snapshot,
            request);
        var canonicalTransport = sourceNode.Node as IZLinkBackendCanonicalActorJoin;
        if (canonicalTransport is not null
            && HasCanonicalActorJoinAuthorityFence(canonicalRequest)
            && sourceNode.Node is IZLinkBackendAuthorityObserver observer)
        {
            // Match the existing router-channel path: it records the exact
            // Location-resolved Spot fence before any service-wire send.
            observer.ObserveSpotAuthority(
                canonicalRequest.TargetNodeRid,
                canonicalRequest.TargetSpotId,
                canonicalRequest.TargetSpotGeneration,
                canonicalRequest.TargetNodeGeneration,
                canonicalRequest.TargetAuthorityOwnerGeneration,
                canonicalRequest.TargetOwnerLeaseGeneration);
        }

        var canonicalAdmission = canonicalTransport is not null
            && canonicalTransport.CanRequestCanonicalActorJoin(canonicalRequest)
            ? await TryRequestCanonicalAdmissionAsync(
                    canonicalTransport,
                    canonicalRequest,
                    predictedPayloadBytes,
                    RemainingTimeout(absoluteDeadline),
                    admissionCancellationToken)
                .ConfigureAwait(false)
            : null;
        handoffId = canonicalAdmission?.HandoffId ?? handoffId;
        var admission = canonicalAdmission is { } selectedCanonical
            ? (Snapshot: target.Snapshot, Reply: selectedCanonical.Reply)
            : await ZLinkSpotHandleRequestExecution.ExecuteAsync(
                target,
                async snapshot =>
                {
                    var requestTimeout = RemainingTimeout(absoluteDeadline);
                    var started = System.Diagnostics.Stopwatch.GetTimestamp();
                    var admissionHeader = ZLinkClientCallCodec.CreateEnvelope(
                        ZLinkMessageKind.Request,
                        snapshot.RouterChannelId,
                        ZLinkRemoteActorJoinPackets.AdmissionPacketName,
                        requestTimeout);
                    var admissionParts = ZLinkRemoteActorJoinPackets.EncodeAdmissionRequest(
                        admissionHeader,
                        actor.Context.ActorId,
                        actorType,
                        handoffId,
                        admissionDeadline,
                        sourceSpotId,
                        actorRef.NodeRid,
                        request,
                        registration.Codecs,
                        actorRef.Generation,
                        actorAuthorityOwnerGeneration,
                        predictedPayloadBytes,
                        (ulong)snapshot.Generation,
                        snapshot.AuthorityOwnerGeneration);
                    IReadOnlyList<ReadOnlyMemory<byte>> wire;
                    try
                    {
                        wire = admissionParts.Select(
                            static part => (ReadOnlyMemory<byte>)part.ToArray()).ToArray();
                    }
                    finally
                    {
                        ZLinkMessageParts.DisposeAll(admissionParts);
                    }
                    var replyParts = await ZLinkDurableRequest.RequestAsync(
                            wire,
                            started,
                            requestTimeout,
                            (frames, remaining, token) =>
                            {
                                ZLinkFrameworkDebugLog.SpotDiscovery(
                                    $"admit_request_sent actor={actor.Context.ActorId} "
                                    + $"target_node={snapshot.NodeRid} spot={snapshot.SpotId} "
                                    + $"spot_gen={snapshot.Generation} "
                                    + $"node_gen={snapshot.NodeGeneration} "
                                    + $"authority_gen={snapshot.AuthorityOwnerGeneration} "
                                    + $"lease_gen={snapshot.OwnerLeaseGeneration}");
                                return runtime.RequestToSpotViaRouterChannelAsync(
                                    snapshot.RouterChannelId,
                                    snapshot.NodeRid,
                                    snapshot.SpotId,
                                    (ulong)snapshot.Generation,
                                    snapshot.NodeGeneration,
                                    snapshot.AuthorityOwnerGeneration,
                                    snapshot.OwnerLeaseGeneration,
                                    frames.Select(Message.From).ToArray(),
                                    remaining,
                                    token);
                            },
                            admissionCancellationToken)
                        .ConfigureAwait(false);
                    ZLinkFrameworkDebugLog.SpotDiscovery(
                        $"admit_reply_received actor={actor.Context.ActorId}");
                    var reply = ZLinkRemoteActorJoinPackets.DecodeAdmissionReplyAndDispose(
                        replyParts,
                        actor.Context.ActorId,
                        snapshot.SpotId);
                    return (Snapshot: snapshot, Reply: reply);
                },
                admissionCancellationToken)
            .ConfigureAwait(false);
        var targetNodeRid = admission.Snapshot.NodeRid;
        var targetSpotId = admission.Snapshot.SpotId;
        var routerChannelId = admission.Snapshot.RouterChannelId;
        var admissionReply = admission.Reply;
        var admissionReplyMessage = ZLinkRemoteActorJoinPackets.DecodeAdmissionReplyPayload(
            admissionReply,
            registration.Codecs);
        if (!admissionReply.Accepted)
        {
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"source_rejected site={1} token_empty={{string.IsNullOrEmpty(admissionReply.ReservationToken)}}");
            Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                "actor_join_rejected site=remote_admission");
            return new ZLinkActorJoinResult.Rejected(admissionReplyMessage);
        }
        if (string.IsNullOrEmpty(admissionReply.ReservationToken)
            || admissionReply.ReservedPayloadBytes != predictedPayloadBytes
            || admissionReply.TargetNodeRid is null
            || !admissionReply.TargetNodeRid.AsSpan().SequenceEqual(
                targetNodeRid.ToBytes())
            || admissionReply.TargetNodeGeneration == 0
            || admissionReply.TargetSpotGeneration
               != (ulong)admission.Snapshot.Generation
            || actorAuthorityOwnerGeneration
               is 0 or > long.MaxValue
            || admissionReply.TargetAuthorityOwnerGeneration
               is 0 or > long.MaxValue
            // The canonical path self-composes TargetAuthorityOwnerGeneration
            // as actorAuthorityOwnerGeneration + 1 (see
            // TryRequestCanonicalAdmissionAsync); checking it against the same
            // value here is tautological for that path. The legacy
            // router-channel admission reply carries the target's own
            // independently computed value and spec 51 §9 requires exact
            // equality, not merely "advanced" ordering.
            || (canonicalAdmission is null
                && admissionReply.TargetAuthorityOwnerGeneration
                   != checked(actorAuthorityOwnerGeneration + 1))
            || admissionReply.TargetSpotAuthorityOwnerGeneration
               != admission.Snapshot.AuthorityOwnerGeneration)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ProtocolError,
                $"Actor '{actor.Context.ActorId}' target returned an invalid relocation reservation.");
        var targetReservation = new ZLinkActorRelocationReservation(
            admissionReply.ReservationToken,
            admissionReply.ReservedPayloadBytes,
            targetNodeRid,
            admissionReply.TargetNodeGeneration,
            admissionReply.TargetSpotGeneration,
            admissionReply.TargetAuthorityOwnerGeneration,
            admissionReply.TargetSpotAuthorityOwnerGeneration,
            admissionReply.ReceiveChunkLimitBytes);
        setTargetReservation(new TargetAdmissionReservationRoute(
            actor.Context.ActorId,
            handoffId,
            targetReservation.Token,
            targetNodeRid,
            targetSpotId,
            targetReservation.TargetSpotGeneration,
            targetReservation.TargetNodeGeneration,
            targetReservation.TargetSpotAuthorityOwnerGeneration,
            admission.Snapshot.OwnerLeaseGeneration,
            routerChannelId));
        var authorityRecheck = await authorityStore.ReadAuthorityAsync(
                ZLinkActorAuthorityPayloadCodec.AuthorityKey(actor.Context.ActorId),
                cancellationToken)
            .ConfigureAwait(false);
        if (authorityRecheck is not ZLinkAuthorityReadResult.Found currentAuthority
            || currentAuthority.Snapshot.ObjectGeneration != actorRef.Generation
            || currentAuthority.Snapshot.AuthorityOwnerGeneration
               != actorAuthorityOwnerGeneration)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                $"Actor '{actor.Context.ActorId}' authority changed during relocation preflight.");
        var hasBoundSession = actorState.TryGetBoundSession(out var boundSession);
        //  The seal branch is conditional, so its trace being absent means
        //  "skipped" just as often as "never reached". Record the condition.
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"preflight_done actor={actor.Context.ActorId} has_bound_session={hasBoundSession}");
        if (hasBoundSession && boundSession.SessionNodeRid is null)
            boundSession = boundSession with { SessionNodeRid = actorRef.NodeRid };
        //  The relocation coordinator fence (owner/lease/node/store-version)
        //  is required on every ZLJR request, bound session or not: the
        //  target's ZLJR decoder rejects an empty/zero fence as malformed
        //  recovery regardless of whether a session happens to be bound.
        //  Only session seal/binding stays conditional on hasBoundSession.
        var sessionRelocationContext = ZLinkSessionRelocationContext.Create(
            Guid.ParseExact(handoffId, "N"),
            currentAuthority.Snapshot.OwnerId,
            checked((ulong)currentAuthority.Snapshot.OwnerLeaseGeneration),
            sourceAuthority.NodeRid,
            sourceAuthority.NodeGeneration,
            currentAuthority.Snapshot.StoreVersion);
        if (hasBoundSession)
        {
            actorState.RememberSourceSessionRelocation(
                handoffId,
                sessionRelocationContext);
            boundSession = boundSession with
            {
                BindingGeneration = boundSession.BindingGeneration == 0
                    ? 1
                    : boundSession.BindingGeneration,
                ObjectGeneration = actorRef.Generation,
                AuthorityOwnerGeneration = actorAuthorityOwnerGeneration,
                SessionOwnerNodeGeneration =
                    boundSession.SessionOwnerNodeGeneration == 0
                        ? ResolveSessionOwnerNodeGeneration(
                            boundSession.SessionNodeRid ?? actorRef.NodeRid)
                        : boundSession.SessionOwnerNodeGeneration
            };
            await SealBoundSessionRouteAsync(
                    actor.Context.ActorId,
                    actorRef.NodeRid,
                    boundSession,
                    sessionRelocationContext,
                    cancellationToken)
                .ConfigureAwait(false);
            actorState.BindSession(
                boundSession.SessionNodeRid,
                boundSession.SessionRid,
                boundSession.BindingToken,
                boundSession.BindingGeneration,
                boundSession.ObjectGeneration,
                boundSession.AuthorityOwnerGeneration,
                boundSession.MeshName,
                boundSession.TargetNodeGeneration,
                boundSession.OwnerLeaseGeneration,
                boundSession.SessionOwnerNodeGeneration,
                boundSession.AcceptedHighWater);
        }

        ZLinkCapturedActorRelocationState relocationState;
        relocationMetric.Start();
        _ = await actorState.BeginHandoffCaptureAsync(cancellationToken)
            .ConfigureAwait(false);
        markSourceCaptureStarted();
        relocationState = await CaptureRelocationStateAsync(
                relocation,
                actor,
                cancellationToken)
            .ConfigureAwait(false);
        if (relocationState.Payload.LongLength
            > ZLinkRemoteActorJoinPackets.SnapshotApplicationStateReservationBytes)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Rejected,
                $"Actor '{actor.Context.ActorId}' relocation adapter returned more than 64 MiB.");

        // A locally bound session records no SessionNodeRid (null means "this
        // node"); the join commit crosses nodes, so the target must receiver
        // the concrete session node rid — the actor's current owner node — or
        // its pushes can never route back to the session.
        //  Spec 30 §11: the SafeToShutdown obligation starts at seal, not
        //  at cutover — waiting until CommitMessageFollow would let a
        //  shutdown query race the seal-to-cutover window. Attaching the
        //  token in the same locked call as the seal itself closes the
        //  window where a status read could observe the seal without the
        //  obligation counted, or an abort could race the attach.
        actorState.Handoff.SealCapture(runtime.BeginPendingRelocationUnit());
        var committedFrames = actorState.Handoff.SnapshotFrames();
        var relocationStore = registration.Locations.ResolveRelocationStore()
                              ?? throw new ZLinkConfigurationException(
                                  "Cross-node Actor relocation requires a Relocation Store.");
        var relocationId = Guid.ParseExact(handoffId, "N");
        var authorityKey = ZLinkActorAuthorityPayloadCodec.AuthorityKey(
            actor.Context.ActorId);
        // The immutable root cannot contain its own reference, checksum, or
        // digest. Recovery persists this exact sentinel; startup verifies the
        // real root against the published authority instead.
        var pendingReference = new ZLinkRelocationManifestReference(
            "pending",
            0,
            relocationId,
            1,
            new byte[32]);
        var requestTemplate = ZLinkRemoteActorJoinPackets.CreateJoinRequest(
            actor.Context.ActorId,
            actorType,
            handoffId,
            sourceSpotId,
            actorRef.NodeRid,
            actorRef.Generation,
            actorAuthorityOwnerGeneration,
            boundSession.SessionNodeRid,
            boundSession.SessionRid,
            relocationState.ContentType,
            pendingReference,
             request,
             registration.Codecs,
	             hasBoundSession ? boundSession : null,
	             targetReservation,
                 sessionRelocationContext,
                 actorNodeGeneration:
                     currentAuthority.Snapshot.Allocation
                         .DescriptorLifecycleGeneration,
	                 expectedOwnerLeaseGeneration: checked(
	                     (ulong)currentAuthority.Snapshot.OwnerLeaseGeneration),
                     targetAttemptGeneration:
                         ZLinkStandaloneActorRelocationRuntime
                             .InitialTargetAttemptGeneration);
        var recovery = new ZLinkActorRelocationRecoveryRecord(
            requestTemplate,
            targetSpotId,
            targetNodeRid.ToBytes().ToArray(),
            targetReservation.TargetNodeGeneration,
            targetReservation.TargetSpotGeneration,
            targetReservation.TargetAuthorityOwnerGeneration,
            operationId?.High ?? 0,
            operationId?.Low ?? 0,
            operationId is null
                ? null
                : admissionReply.RecoveryReplyContentType
                  ?? admissionReply.ReplyContentType,
            operationId is null ? [] : admissionReply.Reply);
        var targetDescriptor = (await authorityStore.ListAllMeshNodesAsync(
                sourceAuthority.MeshName,
                cancellationToken)
            .ConfigureAwait(false)).SingleOrDefault(
            descriptor => descriptor.Rid == targetNodeRid);
        if (targetDescriptor is null
            || targetDescriptor.LifecycleGeneration
               != targetReservation.TargetNodeGeneration
            || targetDescriptor.LeaseGeneration <= 0
            || string.IsNullOrWhiteSpace(targetDescriptor.OwnerId))
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.DataLost,
                $"Actor '{actor.Context.ActorId}' target owner fence changed before root preparation.",
                retryAdvice: ZLinkRetryAdvice.DoNotRetry);
        var targetActor = new ZLinkBackendActorRef(
            targetNodeRid,
            actor.Context.ActorId,
            actorRef.Generation);
        var acceptedRecords = committedFrames.Select(frame =>
                new ZLinkActorAcceptedRecord(
                    frame,
                    frame.RequestSource
                    ?? throw new ZLinkRelocationDataLostException(
                        $"Actor '{actor.Context.ActorId}' accepted journal lost its source fence."),
                    targetActor))
            .ToArray();
        var precommit = new ZLinkStandaloneActorRelocationPrecommitCoordinator(
            authorityStore);
        var precommitSnapshot = await precommit.BeginPreparingAsync(
                currentAuthority.Snapshot,
                sourceAuthority,
                relocationId,
                registration.ApplicationVersion,
                cancellationToken)
            .ConfigureAwait(false);
        var relocationEnvelope =
            ZLinkStandaloneActorRelocationRuntime.CreateImmutableRoot(
                precommitSnapshot,
                sourceAuthority,
                new ZLinkStandaloneActorRelocationDestination(
                    targetSpotId,
                    targetReservation.TargetSpotGeneration,
                    admission.Snapshot.SpotKind,
                    targetNodeRid,
                    targetReservation.TargetNodeGeneration,
                    sourceAuthority.MeshName,
                    new ZLinkLocationOwnerToken(
                        targetDescriptor.OwnerId,
                        targetDescriptor.LeaseGeneration)),
                relocationId,
                relocationState.Payload,
                acceptedRecords,
                hasBoundSession
                    ? ZLinkRemoteActorJoinPackets.DecodeBoundSessionRoute(
                        requestTemplate)
                    : default,
                ZLinkActorRemoteJoinRecoveryCodec.Encode(recovery),
                relocationState.ContentType
                == ZLinkRemoteActorJoinPackets.SnapshotRelocationContentType
                    ? ZLinkObjectMaintenancePolicyKind.Snapshot
                    : ZLinkObjectMaintenancePolicyKind.Recreate);
        var initialEnvelope = ZLinkCanonicalActorRelocationWriter.CreateInitial(
            relocationEnvelope,
            registration.ApplicationVersion);
        //  Direct transfer (spec 28 §4.2): the encoded envelope stays in
        //  source memory and is streamed as relocationState chunks — the
        //  Relocation Store holds no handoff payload.
        //  Spec 28 direct transfer: clamp to the target's advertised
        //  receive-chunk-limit in addition to this node's own configured
        //  chunk limit.
        var effectiveChunkLimit = ZLinkRemoteActorJoinPackets
            .EffectiveDirectTransferChunkLimit(
                registration.Locations.Options.RelocationPayloadChunkLimit,
                targetReservation.ReceiveChunkLimitBytes);
        var transferPayload = ZLinkRelocationTransferPayload.Create(
            initialEnvelope,
            effectiveChunkLimit);
        var prepared = new ZLinkPreparedRelocation(
            new ZLinkRelocationStored(
                string.Empty,
                transferPayload.ChecksumCrc32c,
                default,
                default),
            initialEnvelope)
        {
            LogicalLength = transferPayload.TotalLength,
            LogicalChecksumCrc32c = transferPayload.ChecksumCrc32c,
            ChunkCount = transferPayload.ChunkCount
        };
        precommitSnapshot = await precommit.CaptureAsync(
                precommitSnapshot,
                initialEnvelope,
                cancellationToken)
            .ConfigureAwait(false);
        if (sourceNode.Node is not IZLinkBackendCanonicalRelocation canonical)
            throw new ZLinkConfigurationException(
                "The source MeshNode does not support canonical relocation commands.");
        //  The Coordinator fence (owner/lease/node/StoreVersion) is a single
        //  pre-precommit value shared by the durable ZLJR recovery record
        //  (sessionRelocationContext above) and this command-40 Prepare —
        //  never the post-BeginPreparing/post-Capture precommitSnapshot,
        //  whose StoreVersion has already moved past what ZLJR carries. The
        //  cpp reference source builds exactly one `coordinator` from the
        //  pre-precommit authority snapshot and reuses it for both wire
        //  messages (mesh_node_runtime.cpp's relocate_application_actor).
        //  ObjectGeneration/AuthorityOwnerGeneration/OwnerId/LeaseGeneration
        //  are unaffected by this swap: BeginPreparingAsync/CaptureAsync
        //  preserve them (ZLinkAuthorityGenerationTransition.Preserve) and
        //  only rotate Payload/StoreVersion.
        var prepare = ZLinkStandaloneActorRelocationRuntime.CreatePrepare(
            currentAuthority.Snapshot,
            sourceAuthority,
            targetDescriptor,
            initialEnvelope,
            transferPayload,
            registration.ApplicationVersion);
        _ = await canonical.PrepareCanonicalRelocationAsync(
                targetNodeRid,
                prepare,
                transferPayload,
                RemainingTimeout(absoluteDeadline),
                cancellationToken)
            .ConfigureAwait(false);
        var commitBoundary = actorState.Handoff.FreezeCaptureCommitBoundary();
        var cutoverRecords = commitBoundary.Frames.Select(frame =>
                new ZLinkActorAcceptedRecord(
                    frame,
                    frame.RequestSource
                    ?? throw new ZLinkRelocationDataLostException(
                        $"Actor '{actor.Context.ActorId}' accepted journal lost its source fence."),
                    targetActor))
            .ToArray();
        //  Spec 28 §4.4: the pre-boundary relay batch is encoded once so the
        //  cutover boundary CRC covers exactly the relayed bytes.
        var boundaryRecords = cutoverRecords
            .Skip(acceptedRecords.Length)
            .Select(accepted => (ReadOnlyMemory<byte>)
                ZLinkCanonicalActorAcceptedJournal.Encode(accepted, actorRef))
            .ToArray();
        foreach (var encodedRecord in boundaryRecords)
            await canonical.SendCanonicalRelocationDataAsync(
                    targetNodeRid,
                    new ZLinkServiceWireCodec.RelocationDataRecord(
                        prepare.RelocationId,
                        prepare.TargetAttemptGeneration,
                        prepare.Coordinator,
                        1,
                        prepare.Object,
                        new ZLinkServiceWireCodec.FrozenRecord(encodedRecord)),
                    cancellationToken)
                .ConfigureAwait(false);
        await canonical.SendCanonicalRelocationCutoverAsync(
                targetNodeRid,
                new ZLinkServiceWireCodec.RelocationCutoverRecord(
                    prepare.RelocationId,
                    prepare.TargetAttemptGeneration,
                    prepare.Coordinator,
                    1,
                    prepare.Object,
                    checked((ulong)boundaryRecords.Length),
                    ZLinkRelocationBoundaryBatch.ComputeChecksum(
                        boundaryRecords)),
                cancellationToken)
            .ConfigureAwait(false);
        var published = await ZLinkStandaloneActorRelocationRuntime
            .WaitForCommittedTargetAuthorityAsync(
                authorityStore,
                authorityKey,
                sourceAuthoritySnapshot,
                prepared.Relocation,
                relocationId,
                targetDescriptor,
                prepare.TargetAttemptGeneration,
                cancellationToken)
            .ConfigureAwait(false);
        var resultActorRef = targetActor;
        setTargetAccepted(resultActorRef, admissionReplyMessage);
        if (resultActorRef.Generation != actorRef.Generation)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                $"Actor '{actor.Context.ActorId}' target changed ObjectGeneration during handoff.");
        var trailingFrames = actorState.Handoff.CutoverCaptureToMessageFollow(
            cutoverRecords.Length,
            actorRef,
            resultActorRef,
            routerChannelId,
            sourceAuthority.NodeGeneration,
            admission.Snapshot.NodeGeneration,
            actorAuthorityOwnerGeneration,
            published.AuthorityOwnerGeneration,
            checked((ulong)runtime.LocationLifecycle!.OwnerToken.LeaseGeneration),
            admission.Snapshot.OwnerLeaseGeneration);
        markSourceLeft();
        var trailingDeliveries = runtime.RelayStandaloneActorRelocationTrailing(
            actorState,
            actorRef,
            trailingFrames);
        actorState.Handoff.CommitMessageFollow(
            registration.Locations.Options.MessageFollowDuration,
            registration.Locations.Options.RelocationCutoverWaitTimeout);
        if (trailingDeliveries.Count != 0
            && (await Task.WhenAll(trailingDeliveries).ConfigureAwait(false))
            .Any(static delivered => !delivered))
            throw new ZLinkRelocationDataLostException(
                $"Actor '{actor.Context.ActorId}' could not deliver its pre-cutover Message Follow backlog.");
        if (!runtime.TryRunDetached(
                "actor-source-handoff-cleanup",
                async shutdownToken =>
                {
                    using var sourceCleanupCancellation =
                        CancellationTokenSource.CreateLinkedTokenSource(shutdownToken);
                    var sourceCleanupRemaining = absoluteDeadline - DateTimeOffset.UtcNow;
                    if (sourceCleanupRemaining <= TimeSpan.Zero)
                        sourceCleanupCancellation.Cancel();
                    else
                        sourceCleanupCancellation.CancelAfter(sourceCleanupRemaining);
                    await ReconcileCommittedSourceHandoffAsync(
                            actor,
                            sourceActivation,
                            actorState,
                            actorRef,
                            resultActorRef,
                            sourceAuthoritySnapshot,
                            sourceCleanupCancellation.Token)
                        .ConfigureAwait(false);
                    runtime.LogActorHandoff(
                        $"source_handoff_completed actor={actor.Context.ActorId}");
                }))
            runtime.LogActorHandoff(
                $"source_handoff_schedule_rejected actor={actor.Context.ActorId}");
        actorState.ForgetSourceSessionRelocation(handoffId);
        return new ZLinkActorJoinResult.Accepted(
            resultActorRef.ToNative(sourceAuthority.MeshName),
            admissionReplyMessage);
    }

    private ZLinkBackendCanonicalActorJoinRequest CreateCanonicalActorJoinRequest(
        ZLinkBackendActorRef actor,
        ZLinkActorAuthorityPayload sourceAuthority,
        ZLinkAuthoritySnapshot sourceAuthoritySnapshot,
        ZLinkSpotHandleSnapshot target,
        ZLinkMessage request)
    {
        var encodedRequest = request.Encode(registration.Codecs);
        return new ZLinkBackendCanonicalActorJoinRequest(
            actor,
            sourceAuthority.NodeGeneration,
            sourceAuthoritySnapshot.AuthorityOwnerGeneration,
            checked((ulong)sourceAuthoritySnapshot.OwnerLeaseGeneration),
            target.SpotKind == ZLinkSpotKind.Entry,
            target.NodeRid,
            target.SpotId,
            checked((ulong)target.Generation),
            target.NodeGeneration,
            target.AuthorityOwnerGeneration,
            target.OwnerLeaseGeneration,
            "ZLinkFrameworkActorJoinRequest",
            encodedRequest.ContentType,
            encodedRequest.Payload.ToArray());
    }

    // internal (not private) so unit tests can exercise the ZLJR
    // outer-vs-inner ReplyContentType split directly, matching
    // DecodeCanonicalApplicationReply/ResolveSourceSpotId below.
    internal static async ValueTask<CanonicalAdmission?>
        TryRequestCanonicalAdmissionAsync(
            IZLinkBackendCanonicalActorJoin transport,
            ZLinkBackendCanonicalActorJoinRequest request,
            long predictedPayloadBytes,
            TimeSpan timeout,
            CancellationToken cancellationToken)
    {
        using var completion =
            new ZLinkNativeReplyCompletion<ZLinkBackendActorJoinResult>(
                cancellationToken);
        if (!transport.RequestCanonicalActorJoin(
                request,
                completion.Complete,
                timeout,
                out var correlation))
            return null;

        var (join, replyParts) = await completion.Task.ConfigureAwait(false);
        if (join.Result != RequestResult.Ok)
        {
            ZLinkMessageParts.DisposeAll(replyParts);
            throw ZLinkRequestFailureMapper.CreateCompletionException(
                join.Result,
                join.FailureErrno,
                $"Canonical Actor join for '{request.Actor.ActorId}' to SPOT "
                + $"'{request.TargetSpotId}'");
        }

        try
        {
            if (join.JoinResultCode is not 0 and not 1
                || join.Actor != request.Actor with { NodeRid = request.TargetNodeRid }
                || string.IsNullOrEmpty(join.JoinedSpotId)
                || join.JoinedSpotGeneration == 0)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.ProtocolError,
                    "Canonical actorJoin admission reply tail is malformed.");

            if (replyParts.Count > 1)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.ProtocolError,
                    "Canonical actorJoin application reply is malformed.");

            var application = DecodeCanonicalApplicationReply(join, replyParts);
            var handoffId = ZLinkRemoteActorJoinPackets.CreateCanonicalHandoffId(
                request.Actor.NodeRid,
                request.Actor.ActorId,
                request.Actor.Generation,
                request.ActorNodeGeneration,
                correlation);
            var reply = new ZLinkRemoteActorAdmissionReply(
                join.JoinResultCode == 0,
                application.ContentType,
                application.Payload.ToArray(),
                ReservationToken: handoffId,
                ReservedPayloadBytes: predictedPayloadBytes,
                TargetNodeRid: request.TargetNodeRid.ToBytes().ToArray(),
                TargetNodeGeneration: request.TargetNodeGeneration,
                TargetSpotGeneration: join.JoinedSpotGeneration,
                TargetAuthorityOwnerGeneration: checked(
                    request.ActorAuthorityOwnerGeneration + 1),
                TargetSpotAuthorityOwnerGeneration:
                    request.TargetAuthorityOwnerGeneration,
                ReceiveChunkLimitBytes: join.Flags,
                // The ZLJR saved-work row (command 40) fences this as the
                // fixed outer service-wire profile, not the reply's actual
                // typed content type — see ZLinkRemoteActorAdmissionReply.
                RecoveryReplyContentType:
                    ServiceWireConstants.FrameworkMultipartContentType);
            return new CanonicalAdmission(reply, handoffId);
        }
        catch (ZLinkFrameworkException)
        {
            throw;
        }
        catch (Exception error) when (error is not OperationCanceledException)
        {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ProtocolError,
                "Canonical actorJoin admission reply could not be decoded.",
                innerException: error);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(replyParts);
        }
    }

    // service-wire-v1 ActorJoin(28): spec 51 fixes the reply framing as
    // multipart wrap + sole raw part. The managed mesh has already unwrapped
    // the framework-multipart reply, so the sole part here is delivered as
    // the application reply verbatim — it is never reinterpreted as another
    // (nested) envelope, matching every other target.
    internal static ZLinkApplicationPayloadEnvelope DecodeCanonicalApplicationReply(
        ZLinkBackendActorJoinResult join,
        IReadOnlyList<Message> replyParts)
    {
        if (replyParts.Count > 1)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ProtocolError,
                "Canonical actorJoin application reply is malformed.");

        if (replyParts.Count == 0)
            return new ZLinkApplicationPayloadEnvelope(
                typeof(ZLinkMessage).Name,
                join.ReplyContentType,
                ReadOnlyMemory<byte>.Empty);

        return new ZLinkApplicationPayloadEnvelope(
            typeof(ZLinkMessage).Name,
            join.ReplyContentType,
            replyParts[0].AsReadOnlyMemory());
    }

    internal readonly record struct CanonicalAdmission(
        ZLinkRemoteActorAdmissionReply Reply,
        string HandoffId);

    private static bool HasCanonicalActorJoinAuthorityFence(
        ZLinkBackendCanonicalActorJoinRequest request) =>
        request.Actor.Generation != 0
        && request.ActorNodeGeneration != 0
        && request.ActorAuthorityOwnerGeneration != 0
        && request.ActorOwnerLeaseGeneration != 0
        && !request.TargetNodeRid.IsEmpty
        && !string.IsNullOrWhiteSpace(request.TargetSpotId)
        && request.TargetSpotGeneration != 0
        && request.TargetNodeGeneration != 0
        && request.TargetAuthorityOwnerGeneration != 0
        && request.TargetOwnerLeaseGeneration != 0;

    private bool IsRuntimeShutdown(Exception exception) =>
        runtime.ShutdownToken.IsCancellationRequested
        || exception is ZLinkFrameworkException
        {
            Kind: ZLinkFrameworkErrorKind.ShuttingDown
        };

    private async ValueTask<ZLinkCapturedActorRelocationState> CaptureRelocationStateAsync(
        ZLinkObjectRelocationRegistration relocation,
        IZLinkActor actor,
        CancellationToken cancellationToken)
    {
        var payload = await ZLinkActorRelocationRegistry.CaptureAsync(
                services,
                relocation,
                actor,
                cancellationToken)
            .ConfigureAwait(false);
        return new ZLinkCapturedActorRelocationState(
            relocation.PolicyKind == 2
                ? ZLinkRemoteActorJoinPackets.SnapshotRelocationContentType
                : ZLinkRemoteActorJoinPackets.RecreateRelocationContentType,
            payload);
    }

    private async ValueTask ReconcileCommittedSourceHandoffAsync(
        IZLinkActor actor,
        ZLinkSpotActivation? sourceActivation,
        ZLinkActorRuntimeState actorState,
        ZLinkBackendActorRef sourceActorRef,
        ZLinkBackendActorRef targetActorRef,
        ZLinkAuthoritySnapshot sourceAuthoritySnapshot,
        CancellationToken cancellationToken)
    {
        if (ZLinkBoundSessionDispatchScope.TryDefer(
            actorState.ActorId,
            ct => ReconcileCommittedSourceHandoffCoreAsync(
                    actor,
                    sourceActivation,
                    actorState,
                    sourceActorRef,
                    targetActorRef,
                    sourceAuthoritySnapshot,
                    ct)))
            return;

        await ReconcileCommittedSourceHandoffCoreAsync(
                actor,
                sourceActivation,
                actorState,
                sourceActorRef,
                targetActorRef,
                sourceAuthoritySnapshot,
                cancellationToken)
            .ConfigureAwait(false);
    }

    private async ValueTask ReconcileCommittedSourceHandoffCoreAsync(
        IZLinkActor actor,
        ZLinkSpotActivation? sourceActivation,
        ZLinkActorRuntimeState actorState,
        ZLinkBackendActorRef sourceActorRef,
        ZLinkBackendActorRef targetActorRef,
        ZLinkAuthoritySnapshot sourceAuthoritySnapshot,
        CancellationToken cancellationToken)
    {
        var migrationApplied = false;
        await ZLinkReconciliationRunner.RunAsync(
                async token =>
                {
                    if (!migrationApplied)
                    {
                        if (sourceActivation is not null)
                            await ReconcileCommittedSourceLeaveAsync(
                                    actor,
                                    sourceActivation,
                                    token)
                                .ConfigureAwait(false);
                        await ApplyRemoteActorMigrationCoreAsync(
                                actorState,
                                targetActorRef,
                                sourceAuthoritySnapshot,
                                token)
                            .ConfigureAwait(false);
                        migrationApplied = true;
                    }

                    await actorSessionManager.FinalizeMigratedSourceAsync(actorState, sourceActorRef)
                        .ConfigureAwait(false);
                },
                exception => ReportCommittedHandoffFailure(
                    "actor-source-handoff-cleanup",
                    exception),
                cancellationToken,
                static exception => exception is OperationCanceledException)
            .ConfigureAwait(false);
    }

    private async ValueTask ReconcileCommittedSourceLeaveAsync(
        IZLinkActor actor,
        ZLinkSpotActivation sourceActivation,
        CancellationToken cancellationToken)
    {
        await ZLinkReconciliationRunner.RunAsync(
                token => sourceActivation.TryNotifyActorLeftAfterCommittedMembershipAsync(
                    actor,
                    token),
                exception => ReportCommittedHandoffFailure(
                    "actor-source-leave",
                    exception),
                cancellationToken,
                static exception => exception is OperationCanceledException)
            .ConfigureAwait(false);
    }

    private async ValueTask<ZLinkRemoteActorJoinReply> ReconcileTargetJoinCommitAsync(
        string actorId,
        string handoffId,
        RoutingId targetNodeRid,
        string targetSpotId,
        ulong targetSpotGeneration,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        ulong ownerLeaseGeneration,
        string routerChannelId,
        DateTimeOffset absoluteDeadline,
        CancellationToken cancellationToken,
        Func<IReadOnlyList<Message>> createParts)
    {
        return await ZLinkReconciliationRunner.RunAsync(
                async token =>
                {
                    var replyParts = await runtime.RequestToSpotViaRouterChannelAsync(
                            routerChannelId,
                            targetNodeRid,
                            targetSpotId,
                            targetSpotGeneration,
                            targetNodeGeneration,
                            authorityOwnerGeneration,
                            ownerLeaseGeneration,
                            createParts(),
                            RemainingTimeout(absoluteDeadline),
                            token)
                        .ConfigureAwait(false);
                    return ZLinkRemoteActorJoinPackets.DecodeJoinReplyAndDispose(
                        replyParts,
                        actorId,
                        targetSpotId);
                },
                exception => ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"handoff commit retry actor={actorId} id={handoffId}: {exception.Message}"),
                cancellationToken,
                static exception => exception is ZLinkActorHandoffRejectedException)
            .ConfigureAwait(false);
    }

    private async ValueTask ReplayAbortedSourceHandoffAsync(ZLinkActorRuntimeState actorState)
    {
        var frames = actorState.Handoff.AbortCapture();
        if (frames.Count == 0) return;

        if (actorState.LiveActivation is { } activation)
        {
            await activation.ReplayAbortedActorHandoffAsync(
                    actorState,
                    frames,
                    CancellationToken.None)
                .ConfigureAwait(false);
            return;
        }

        var actorRef = actorState.NativeActorRef
                       ?? throw new ZLinkFrameworkException(
                           ZLinkFrameworkErrorKind.NotFound,
                           $"Actor '{actorState.ActorId}' does not have a native Actor ref during handoff rollback.");
        var pipeline = new ZLinkActorInboundPipeline(
            runtime,
            new ZLinkEntrySpotActorInboundEndpoint(runtime));
        await pipeline.DispatchAsync(
                ZLinkActorHandoffFrames.Restore(actorRef, frames),
                CancellationToken.None)
            .ConfigureAwait(false);
    }

    private void ReportCommittedHandoffFailure(string operation, Exception exception)
    {
        ZLinkFrameworkDebugLog.TaskFailure(operation, exception);
        runtime.ErrorSink.ReportUnhandledCallbackException(exception);
    }

    private async ValueTask AbortTargetReservationBestEffortAsync(
        TargetAdmissionReservationRoute reservation)
    {
        try
        {
            var header = ZLinkClientCallCodec.CreateEnvelope(
                ZLinkMessageKind.Request,
                reservation.RouterChannelId,
                ZLinkRemoteActorJoinPackets.AdmissionAbortPacketName,
                registration.DefaultRequestTimeout);
            var parts = ZLinkRemoteActorJoinPackets.EncodeAdmissionAbortRequest(
                header,
                reservation.ActorId,
                reservation.HandoffId,
                reservation.ReservationToken);
            var replyParts = await runtime.RequestToSpotViaRouterChannelAsync(
                    reservation.RouterChannelId,
                    reservation.TargetNodeRid,
                    reservation.TargetSpotId,
                    reservation.TargetSpotGeneration,
                    reservation.TargetNodeGeneration,
                    reservation.TargetSpotAuthorityOwnerGeneration,
                    reservation.TargetOwnerLeaseGeneration,
                    parts,
                    registration.DefaultRequestTimeout,
                    CancellationToken.None)
                .ConfigureAwait(false);
            _ = ZLinkClientCallCodec.DecodeEnvelopeReplyAndDispose<
                ZLinkRemoteActorAdmissionAbortRequest>(
                replyParts,
                "Remote actor admission abort reply was empty.",
                $"Remote actor admission abort failed for '{reservation.ActorId}'.",
                null);
        }
        catch (Exception exception)
        {
            ZLinkFrameworkDebugLog.TaskFailure(
                "actor-target-reservation-abort",
                exception);
        }
    }

    internal static string ResolveSourceSpotId(
        ZLinkActorAuthorityPayload sourceAuthority)
    {
        ArgumentNullException.ThrowIfNull(sourceAuthority);
        return ZLinkSpotId.Require(
            sourceAuthority.CurrentSpotId,
            nameof(sourceAuthority.CurrentSpotId));
    }

    private ulong ResolveSessionOwnerNodeGeneration(RoutingId nodeRid)
    {
        foreach (var spotNode in registration.SpotNodes.Values)
        {
            if (spotNode.RoutingId != nodeRid) continue;
            var meshName = spotNode.SpotMeshChannelName
                           ?? spotNode.SpotNodeName;
            var generation = runtime.GetMeshNodeRuntime(meshName)
                .Node
                .MeshStatus()
                .LifecycleGeneration;
            if (generation > 0) return generation;
        }

        throw new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.InvalidOperation,
            $"Session owner node '{nodeRid}' lifecycle generation is unavailable.");
    }

    private async ValueTask SealBoundSessionRouteAsync(
        string actorId,
        RoutingId actorNodeRid,
        ZLinkActorBoundSession session,
        ZLinkSessionRelocationContext wireContext,
        CancellationToken cancellationToken)
    {
        var sessionOwnerNode = session.SessionNodeRid!.Value;
        var meshName = session.MeshName.Value;
        //  Placed before the local/remote branch: putting it inside one arm
        //  made its absence read as "never reached" when it only meant the
        //  other arm ran.
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"bound_seal_begin actor={actorId} session_node={sessionOwnerNode} "
            + $"local={sessionOwnerNode == runtime.GetMeshNodeRuntime(meshName).Node.RoutingId}");
        _ = await runtime.SealSessionRelocationAsync(
                meshName,
                sessionOwnerNode,
                ZLinkSessionRelocationWire.CreateSeal(
                    actorId,
                    actorNodeRid,
                    session,
                    wireContext),
                cancellationToken)
            .ConfigureAwait(false);
    }

    private async ValueTask AbortBoundSessionRouteSealBestEffortAsync(
        string actorId,
        ZLinkActorRuntimeState actorState,
        string handoffId)
    {
        if (!actorState.TryGetBoundSession(out var session)
            || session.SessionNodeRid is null
            || session.BindingGeneration == 0
            || session.ObjectGeneration == 0
            || session.AuthorityOwnerGeneration == 0
            || session.SessionOwnerNodeGeneration == 0)
            return;
        if (!actorState.TryGetSourceSessionRelocation(
                handoffId,
                out var wireContext))
            return;
        try
        {
            await AbortBoundSessionRouteSealAsync(
                    actorId,
                    session,
                    wireContext,
                    CancellationToken.None)
                .ConfigureAwait(false);
            actorState.ForgetSourceSessionRelocation(handoffId);
        }
        catch
        {
            // The session connection or exact binding may already be gone.
            // Its physical cleanup owns the remaining tombstone.
        }
    }

    private async ValueTask AbortBoundSessionRouteSealAsync(
        string actorId,
        ZLinkActorBoundSession session,
        ZLinkSessionRelocationContext wireContext,
        CancellationToken cancellationToken)
    {
        var sessionOwnerNode = session.SessionNodeRid!.Value;
        var meshName = session.MeshName.Value;
        await runtime.RouteSessionRelocationAsync(
                meshName,
                sessionOwnerNode,
                ZLinkSessionRelocationWire.CreateAbort(
                    actorId,
                    session,
                    wireContext),
                cancellationToken)
            .ConfigureAwait(false);
    }

    private async ValueTask ApplyRemoteActorMigrationCoreAsync(
        ZLinkActorRuntimeState actorState,
        ZLinkBackendActorRef targetActorRef,
        ZLinkAuthoritySnapshot sourceAuthoritySnapshot,
        CancellationToken cancellationToken)
    {
        actorState.BindNativeActorRef(targetActorRef);
        // Source Context identity remains readable through the source leave
        // callback. Fence new operations now, but retain the exact source
        // activation until FinalizeMigratedSourceAsync retires both together.
        actorState.FenceRuntimeGeneration();
        await ReconcileActorLocationAfterMoveAsync(
                actorState,
                sourceAuthoritySnapshot,
                cancellationToken)
            .ConfigureAwait(false);
    }

    private async ValueTask ReconcileActorLocationAfterMoveAsync(
        ZLinkActorRuntimeState actorState,
        ZLinkAuthoritySnapshot sourceAuthoritySnapshot,
        CancellationToken cancellationToken)
    {
        await ZLinkReconciliationRunner.RunAsync(
                token => actorSessionManager.ReleaseActorLocationAfterMoveAsync(
                    actorState,
                    sourceAuthoritySnapshot,
                    token),
                exception => ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"remote actor move cleanup retry for '{actorState.ActorId}': {exception.Message}"),
                cancellationToken)
            .ConfigureAwait(false);
    }

    private async ValueTask<ZLinkResolvedSpotHandle> ResolveRemoteActorJoinTargetAsync(
        string spotId,
        CancellationToken cancellationToken)
    {
        var resolver = services.GetService(typeof(ZLinkLocationAddressResolvers))
            as ZLinkLocationAddressResolvers;
        if (resolver is null) throw new InvalidOperationException($"SPOT '{spotId}' is not active.");

        var handle = await resolver.ResolveSpotHandleAsync(
                spotId,
                cancellationToken)
            .ConfigureAwait(false);
        if (handle is null)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.NotFound,
                $"SPOT '{spotId}' has no live location row.");
        var snapshot = handle.Snapshot;
        if (services.GetService(typeof(IZLinkMeshNodeLocationResolver))
                is IZLinkMeshNodeLocationResolver peerResolver)
        {
            var peers = await peerResolver.ListLiveMeshNodesAsync(
                    snapshot.RouterChannelId, cancellationToken)
                .ConfigureAwait(false);
            if (peers.Any(descriptor =>
                    descriptor.Rid.Equals(snapshot.NodeRid)
                    && descriptor.State == ZLinkFrameworkRuntimeState.Draining))
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Rejected,
                    $"SPOT '{spotId}' is hosted by a draining node.",
                    ZLinkRetryAdvice.DoNotRetry);
        }
        return handle;
    }

    private async ValueTask<ZLinkActorJoinResult> SubmitNativeJoinActorAsync(
        IZLinkActor actor,
        ZLinkBackendActorRef actorRef,
        IZLinkBackendSpotNode node,
        RoutingId targetNodeRid,
        string targetSpotId,
        string channelName,
        ZLinkMessage request,
        CancellationToken cancellationToken)
    {
        var encodedRequest = request.Encode(registration.Codecs);
        var joinHeader = new ZLinkEnvelopeHeader(
            ZLinkMessageKind.Command,
            channelName,
            typeof(ZLinkMessage).Name,
            encodedRequest.ContentType,
            null, null, null, null, null);
        IReadOnlyList<Message> joinParts;
        joinParts = ZLinkMessageParts.Create(
            ZLinkEnvelopeCodec.EncodeHeader(joinHeader),
            Message.From(encodedRequest.Payload.Bytes.Span));

        using var completion = new ZLinkNativeReplyCompletion<ZLinkBackendActorJoinResult>(cancellationToken);

        if (runtime.Flow.Enabled(ZLinkMessageFlowOutcome.Sent))
            runtime.Flow.Trace(new ZLinkMessageFlowEvent(
                ZLinkMessageFlowOutcome.Sent,
                ZLinkDispatchErrorSurface.SpotActor,
                ZLinkDispatchMessageKind.ActorRequest,
                "JoinSpot",
                channelName,
                SourceRid: targetNodeRid.ToString(),
                SpotId: targetSpotId.ToString(),
                ActorId: actor.Context.ActorId));

        bool submitted;
        try
        {
            submitted = node.JoinActor(
                actorRef,
                targetNodeRid,
                targetSpotId,
                joinParts,
                completion.Complete,
                registration.DefaultRequestTimeout);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(joinParts);
        }

        if (!submitted)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.NotFound,
                $"Actor join submit failed for '{actor.Context.ActorId}' to SPOT '{targetSpotId}'.");

        var (joinResult, replyParts) = await completion.Task.ConfigureAwait(false);

        if (runtime.Flow.Enabled(ZLinkMessageFlowOutcome.ReplyReceived))
            runtime.Flow.Trace(new ZLinkMessageFlowEvent(
                ZLinkMessageFlowOutcome.ReplyReceived,
                ZLinkDispatchErrorSurface.SpotActor,
                ZLinkDispatchMessageKind.Response,
                "JoinSpot",
                channelName,
                SourceRid: targetNodeRid.ToString(),
                SpotId: targetSpotId.ToString(),
                ActorId: actor.Context.ActorId));
        var reply = DecodeNativeJoinReply(
            joinResult.Result,
            joinResult.FailureErrno,
            replyParts,
            actor.Context.ActorId,
            targetSpotId);
        var accepted = joinResult.JoinResultCode == 0;
        var actorState = actorSessionManager.GetOrCreateState(actor.Context.ActorId);
        if (accepted)
        {
            actorState.BindNativeActorRef(joinResult.Actor);
            if (joinResult.Actor.NodeRid != actorRef.NodeRid) actorState.InvalidateContext();
        }

        return accepted
            ? new ZLinkActorJoinResult.Accepted(
                joinResult.Actor.ToNative(node.MeshStatus().MeshName),
                reply)
            : RejectedWithTrace(reply);
    }

    private ZLinkMessage DecodeNativeJoinReply(
        RequestResult result,
        int failureErrno,
        IReadOnlyList<Message> replyParts,
        string actorId,
        string spotId)
    {
        try
        {
            if (result != RequestResult.Ok)
                //  Classify the join terminal via the shared ownership-aware mapper
                //  (fine code refines the coarse terminal) instead of collapsing
                //  every non-OK terminal to NotFound (spec 32-framework-error-model:
                //  81-118), matching ZLinkNativeActorJoinOperation.
                throw ZLinkRequestFailureMapper.CreateCompletionException(
                    result,
                    failureErrno,
                    $"Actor join for '{actorId}' to SPOT '{spotId}'");

            if (replyParts.Count == 0)
                //  Spec 32-framework-error-model:91-92 — an empty successful
                //  reply cannot be processed: ProtocolError, not a plain
                //  InvalidOperationException.
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.ProtocolError,
                    "Actor join reply was empty.");

            var header = ZLinkEnvelopeCodec.DecodeHeader(
                replyParts,
                runtime.Flow.CaptureEnabled);
            var reply = (Message)ZLinkEnvelopeCodec.DecodeBody(replyParts, typeof(Message))!;
            using var ownedReply = Message.From(reply);
            return ZLinkMessage.FromEnvelopePayload(header.ContentType, ownedReply, registration.Codecs);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(replyParts);
        }
    }

    private static ZLinkActorJoinResult.Rejected RejectedWithTrace(
        ZLinkMessage reply)
    {
        Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
            "actor_join_rejected site=remote_joiner_tail");
        return new ZLinkActorJoinResult.Rejected(reply);
    }
}

internal readonly record struct ZLinkCapturedActorRelocationState(
    string ContentType,
    byte[] Payload);

internal readonly record struct TargetAdmissionReservationRoute(
    string ActorId,
    string HandoffId,
    string ReservationToken,
    RoutingId TargetNodeRid,
    string TargetSpotId,
    ulong TargetSpotGeneration,
    ulong TargetNodeGeneration,
    ulong TargetSpotAuthorityOwnerGeneration,
    ulong TargetOwnerLeaseGeneration,
    string RouterChannelId);
