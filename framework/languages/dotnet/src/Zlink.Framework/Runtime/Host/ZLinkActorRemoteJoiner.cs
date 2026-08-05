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
                    timeoutToken),
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

    private static bool IsAdmissionTransportRetryable(Exception exception) =>
        exception is ZLinkFrameworkException
        {
            Kind: ZLinkFrameworkErrorKind.Unavailable
        }
        || exception is ZlinkSubmitException
        {
            Result: ZlinkSubmitException.ErrorCode.NotConnected
                or ZlinkSubmitException.ErrorCode.Backpressured
        };

    private static string DescribeAdmissionTransportFailure(Exception exception) =>
        exception is ZLinkFrameworkException frameworkError
            ? $"{frameworkError.Kind}:{frameworkError.Message}"
            : $"{exception.GetType().Name}:{exception.Message}";

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
        CancellationToken cancellationToken)
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
        if (!runtime.RelocationPermits.TryAcquire(
                ZLinkRelocationPermitRequest.Outbound(
                    predictedPayloadBytes,
                    captureRequired),
                out var sourcePermit))
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"Actor '{actor.Context.ActorId}' source relocation admission is busy.");
        using (sourcePermit)
        {
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
                        sourcePermit,
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
                        cancellationToken)
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
                                handoffId)
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
        ZLinkRelocationPermitPool.ZLinkRelocationPermitLease sourcePermit,
        ZLinkActorJoinOperationId? operationId,
        DateTimeOffset absoluteDeadline,
        Action<TargetAdmissionReservationRoute> setTargetReservation,
        Action<ZLinkBackendActorRef, ZLinkMessage> setTargetAccepted,
        Action markSourceCaptureStarted,
        Action markSourceLeft,
        ZLinkRuntimeMetrics.ZLinkRelocationMetricOperation relocationMetric,
        CancellationToken cancellationToken)
    {
        var sourceSpotId = ResolveSourceSpotId(sourceAuthority);

        var admissionDeadline = absoluteDeadline;
        var admission = await ZLinkSpotHandleRequestExecution.ExecuteAsync(
                target,
                snapshot => ZLinkReconciliationRunner.RunAsync(
                    async token =>
                    {
                        var requestTimeout = RemainingTimeout(absoluteDeadline);
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
                        // Both ends of the admission round trip are traced so
                        // a stall shows which side never moved. A retry is
                        // safe here because route convergence rejects the
                        // request before the target admission handler runs.
                        ZLinkFrameworkDebugLog.SpotDiscovery(
                            $"admit_request_sent actor={actor.Context.ActorId} "
                            + $"target_node={snapshot.NodeRid} spot={snapshot.SpotId} "
                            + $"spot_gen={snapshot.Generation} "
                            + $"node_gen={snapshot.NodeGeneration} "
                            + $"authority_gen={snapshot.AuthorityOwnerGeneration} "
                            + $"lease_gen={snapshot.OwnerLeaseGeneration}");
                        var replyParts = await runtime.RequestToSpotViaRouterChannelAsync(
                                snapshot.RouterChannelId,
                                snapshot.NodeRid,
                                snapshot.SpotId,
                                (ulong)snapshot.Generation,
                                snapshot.NodeGeneration,
                                snapshot.AuthorityOwnerGeneration,
                                snapshot.OwnerLeaseGeneration,
                                admissionParts,
                                requestTimeout,
                                token)
                            .ConfigureAwait(false);
                        ZLinkFrameworkDebugLog.SpotDiscovery(
                            $"admit_reply_received actor={actor.Context.ActorId}");
                        var reply = ZLinkRemoteActorJoinPackets.DecodeAdmissionReplyAndDispose(
                            replyParts,
                            actor.Context.ActorId,
                            snapshot.SpotId);
                        return (Snapshot: snapshot, Reply: reply);
                    },
                    exception => ZLinkFrameworkDebugLog.SpotDiscovery(
                        $"admit_transport_retry actor={actor.Context.ActorId} "
                        + $"target_node={snapshot.NodeRid} "
                        + $"error={DescribeAdmissionTransportFailure(exception)}"),
                    cancellationToken,
                    static exception => !IsAdmissionTransportRetryable(exception)),
                cancellationToken)
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
            || admissionReply.TargetAuthorityOwnerGeneration
               <= actorAuthorityOwnerGeneration
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
            admissionReply.TargetSpotAuthorityOwnerGeneration);
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
        if (hasBoundSession)
        {
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
            var sealedHighWater = await SealBoundSessionRouteAsync(
                    actor.Context.ActorId,
                    boundSession,
                    handoffId,
                    cancellationToken)
                .ConfigureAwait(false);
            boundSession = boundSession with
            {
                AcceptedHighWater = sealedHighWater
            };
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

        var header = ZLinkClientCallCodec.CreateEnvelope(
            ZLinkMessageKind.Request,
            routerChannelId,
            ZLinkRemoteActorJoinPackets.CommitPacketName,
            RemainingTimeout(absoluteDeadline));
        // A locally bound session records no SessionNodeRid (null means "this
        // node"); the join commit crosses nodes, so the target must receiver
        // the concrete session node rid — the actor's current owner node — or
        // its pushes can never route back to the session.
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
             targetReservation);
        var recovery = new ZLinkActorRelocationRecoveryRecord(
            requestTemplate,
            targetSpotId,
            targetNodeRid.ToBytes().ToArray(),
            targetReservation.TargetNodeGeneration,
            targetReservation.TargetSpotGeneration,
            targetReservation.TargetAuthorityOwnerGeneration,
            operationId?.High ?? 0,
            operationId?.Low ?? 0,
            operationId is null ? null : admissionReply.ReplyContentType,
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
        var relocationEnvelope =
            ZLinkStandaloneActorRelocationRuntime.CreateImmutableRoot(
                currentAuthority.Snapshot,
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
        var publication = new ZLinkRelocationPublicationCoordinator(
            authorityStore,
            relocationStore);
        var prepared = await publication.PrepareAsync(
                relocationEnvelope,
                cancellationToken)
            .ConfigureAwait(false);
        var joinRequest = requestTemplate with
        {
            RelocationReference = prepared.Relocation.Reference,
            RelocationChecksumCrc32c = prepared.Relocation.ChecksumCrc32c,
            RelocationAggregateId = prepared.Envelope.AggregateId,
            RelocationAggregateGeneration =
                prepared.Envelope.AggregateGeneration,
            RelocationInventoryDigest =
                prepared.Envelope.InventoryDigest.ToArray()
        };
        var payloadBytes = ZLinkRemoteActorJoinPackets.MeasureRelocationPayloadBytes(joinRequest);
        payloadBytes = checked(
            payloadBytes
            + ZLinkRelocationEnvelopeCodec.MeasureEncodedLength(relocationEnvelope));
        if (!sourcePermit.TryShrinkPayload(payloadBytes))
        {
            await publication.DiscardPreparedAsync(prepared)
                .ConfigureAwait(false);
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Rejected,
                $"Actor '{actor.Context.ActorId}' relocation payload exceeded its pre-seal reservation.");
        }

        ZLinkRemoteActorJoinReply reply;
        reply = await ReconcileTargetJoinCommitAsync(
                actor.Context.ActorId,
                handoffId,
                targetNodeRid,
                targetSpotId,
                (ulong)admission.Snapshot.Generation,
                admission.Snapshot.NodeGeneration,
                admission.Snapshot.AuthorityOwnerGeneration,
                admission.Snapshot.OwnerLeaseGeneration,
                routerChannelId,
                absoluteDeadline,
                cancellationToken,
                () => ZLinkRemoteActorJoinPackets.EncodeJoinRequest(
                    header,
                    joinRequest))
            .ConfigureAwait(false);
        if (!reply.Accepted)
        {
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"source_rejected site=join_reply actor={actor.Context.ActorId} "
                + $"spot={targetSpotId} target_rid={targetNodeRid}");
            await publication.DiscardPreparedAsync(prepared)
                .ConfigureAwait(false);
            if (hasBoundSession)
                await AbortBoundSessionRouteSealAsync(
                        actor.Context.ActorId,
                        boundSession,
                        handoffId,
                        cancellationToken)
                    .ConfigureAwait(false);
            Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                "actor_join_rejected site=remote_admission");
            return new ZLinkActorJoinResult.Rejected(admissionReplyMessage);
        }

        var resultActorRef = ZLinkRemoteActorJoinPackets.ToActorRef(reply);
        using var postCommitCancellation =
            CancellationTokenSource.CreateLinkedTokenSource(runtime.ShutdownToken);
        var postCommitRemaining = absoluteDeadline - DateTimeOffset.UtcNow;
        if (postCommitRemaining <= TimeSpan.Zero)
            postCommitCancellation.Cancel();
        else
            postCommitCancellation.CancelAfter(postCommitRemaining);
        var postCommitToken = postCommitCancellation.Token;
        setTargetAccepted(resultActorRef, admissionReplyMessage);
        if (resultActorRef.Generation != actorRef.Generation)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                $"Actor '{actor.Context.ActorId}' target changed ObjectGeneration during handoff.");
        var publishedAuthority = await authorityStore.ReadAuthorityAsync(
                authorityKey,
                postCommitToken)
            .ConfigureAwait(false);
        if (publishedAuthority is not ZLinkAuthorityReadResult.Found published)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.DataLost,
                $"Actor '{actor.Context.ActorId}' relocation authority disappeared after target commit.",
                retryAdvice: ZLinkRetryAdvice.DoNotRetry);
        var targetOwner = new ZLinkLocationOwnerToken(
            published.Snapshot.OwnerId,
            published.Snapshot.OwnerLeaseGeneration);
        var progress = new ZLinkStandaloneActorRelocationProgressCoordinator(
            authorityStore,
            relocationStore,
            new ZLinkStandaloneActorRelocationTargetFence(
                prepared.Envelope.AggregateId,
                admissionReply.TargetAuthorityOwnerGeneration,
                targetNodeRid,
                admissionReply.TargetNodeGeneration,
                targetOwner));
        await progress.AdvancePhaseAsync(
                prepared.Envelope,
                ZLinkActorRelocationAuthorityPhase.Activated,
                ZLinkActorRelocationAuthorityPhase.Cleaning,
                targetOwner,
                postCommitToken)
            .ConfigureAwait(false);
        var trailingFrames = actorState.Handoff.CutoverCaptureToMessageFollow(
            committedFrames.Count,
            actorRef,
            resultActorRef,
            routerChannelId,
            sourceAuthority.NodeGeneration,
            admission.Snapshot.NodeGeneration,
            actorAuthorityOwnerGeneration,
            published.Snapshot.AuthorityOwnerGeneration,
            checked((ulong)runtime.LocationLifecycle!.OwnerToken.LeaseGeneration),
            admission.Snapshot.OwnerLeaseGeneration);
        markSourceLeft();
        runtime.LogActorHandoff($"source_leave_started actor={actor.Context.ActorId}");
        if (!runtime.TryRunDetached(
                "actor-source-leave",
                async shutdownToken =>
                {
                    using var sourceLeaveCancellation =
                        CancellationTokenSource.CreateLinkedTokenSource(shutdownToken);
                    var sourceLeaveRemaining = absoluteDeadline - DateTimeOffset.UtcNow;
                    if (sourceLeaveRemaining <= TimeSpan.Zero)
                        sourceLeaveCancellation.Cancel();
                    else
                        sourceLeaveCancellation.CancelAfter(sourceLeaveRemaining);
                    await ReconcileCommittedSourceLeaveAsync(
                            actor,
                            actorState,
                            sourceLeaveCancellation.Token)
                        .ConfigureAwait(false);
                    runtime.LogActorHandoff(
                        $"source_leave_completed actor={actor.Context.ActorId}");
                }))
            runtime.LogActorHandoff(
                $"source_leave_schedule_rejected actor={actor.Context.ActorId}");
        await progress.PublishAdmissionReadyAuthorityAsync(
                prepared.Envelope,
                targetOwner,
                postCommitToken)
            .ConfigureAwait(false);
        runtime.LogActorHandoff($"admission_ready_published actor={actor.Context.ActorId}");
        actorState.Handoff.CommitMessageFollow(
            registration.Locations.Options.MessageFollowDuration);
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
        await progress.AdvancePhaseAsync(
                prepared.Envelope,
                ZLinkActorRelocationAuthorityPhase.Cleaning,
                ZLinkActorRelocationAuthorityPhase.Completed,
                targetOwner,
                postCommitToken)
            .ConfigureAwait(false);
        runtime.LogActorHandoff($"completed_published actor={actor.Context.ActorId}");
        await ReconcileTargetHandoffCompletionAsync(
                    actor.Context.ActorId,
                    handoffId,
                    trailingFrames,
                    sourceSpotId,
                    actorRef.NodeRid,
                    operationId,
                    admissionReply,
                    hasBoundSession ? boundSession : null,
                    targetNodeRid,
                    targetSpotId,
                    (ulong)admission.Snapshot.Generation,
                    admission.Snapshot.NodeGeneration,
                    admission.Snapshot.AuthorityOwnerGeneration,
                    admission.Snapshot.OwnerLeaseGeneration,
                    routerChannelId,
                    absoluteDeadline,
                    postCommitToken)
                .ConfigureAwait(false);
        runtime.LogActorHandoff($"target_completion_completed actor={actor.Context.ActorId}");
        return new ZLinkActorJoinResult.Accepted(
            resultActorRef.ToNative(sourceAuthority.MeshName),
            admissionReplyMessage);
    }

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
        ZLinkActorRuntimeState actorState,
        ZLinkBackendActorRef sourceActorRef,
        ZLinkBackendActorRef targetActorRef,
        ZLinkAuthoritySnapshot sourceAuthoritySnapshot,
        CancellationToken cancellationToken)
    {
        if (ZLinkBoundSessionDispatchScope.TryDefer(
                actorState.ActorId,
                ct => ReconcileCommittedSourceHandoffCoreAsync(
                    actorState,
                    sourceActorRef,
                    targetActorRef,
                    sourceAuthoritySnapshot,
                    ct)))
            return;

        await ReconcileCommittedSourceHandoffCoreAsync(
                actorState,
                sourceActorRef,
                targetActorRef,
                sourceAuthoritySnapshot,
                cancellationToken)
            .ConfigureAwait(false);
    }

    private async ValueTask ReconcileCommittedSourceHandoffCoreAsync(
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
        ZLinkActorRuntimeState actorState,
        CancellationToken cancellationToken)
    {
        await ZLinkReconciliationRunner.RunAsync(
                token => NotifySourceActorLeftAsync(actor, actorState, token),
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

    private async ValueTask ReconcileTargetHandoffCompletionAsync(
        string actorId,
        string handoffId,
        IReadOnlyList<ZLinkActorHandoffFrame> frames,
        string sourceSpotId,
        RoutingId sourceNodeRid,
        ZLinkActorJoinOperationId? operationId,
        ZLinkRemoteActorAdmissionReply admissionReply,
        ZLinkActorBoundSession? boundSession,
        RoutingId targetNodeRid,
        string targetSpotId,
        ulong targetSpotGeneration,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        ulong ownerLeaseGeneration,
        string routerChannelId,
        DateTimeOffset absoluteDeadline,
        CancellationToken cancellationToken)
    {
        await ZLinkReconciliationRunner.RunAsync(
                token =>
                {
                    token.ThrowIfCancellationRequested();
                    return CompleteTargetHandoffAsync(
                        actorId,
                        handoffId,
                        frames,
                        sourceSpotId,
                        sourceNodeRid,
                        operationId,
                        admissionReply,
                        boundSession,
                        targetNodeRid,
                        targetSpotId,
                        targetSpotGeneration,
                        targetNodeGeneration,
                        authorityOwnerGeneration,
                        ownerLeaseGeneration,
                        routerChannelId,
                        absoluteDeadline,
                        token);
                },
                exception => ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"handoff completion retry actor={actorId} id={handoffId}: {exception.Message}"),
                cancellationToken,
                // Terminal: an explicit rejection (the target's joined
                // callback refused the handoff) or a target that no longer
                // hosts the actor (it already rolled the transfer back) —
                // retrying either would spin for the whole request window.
                exception => exception is ZLinkActorHandoffRejectedException
                             || exception is ZLinkFrameworkException
                             {
                                 Kind: ZLinkFrameworkErrorKind.Rejected
                                     or ZLinkFrameworkErrorKind.NotFound
                             }
                             || (exception is OperationCanceledException
                                 && cancellationToken.IsCancellationRequested))
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

    private async ValueTask CompleteTargetHandoffAsync(
        string actorId,
        string handoffId,
        IReadOnlyList<ZLinkActorHandoffFrame> frames,
        string sourceSpotId,
        RoutingId sourceNodeRid,
        ZLinkActorJoinOperationId? operationId,
        ZLinkRemoteActorAdmissionReply admissionReply,
        ZLinkActorBoundSession? boundSession,
        RoutingId targetNodeRid,
        string targetSpotId,
        ulong targetSpotGeneration,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        ulong ownerLeaseGeneration,
        string routerChannelId,
        DateTimeOffset absoluteDeadline,
        CancellationToken cancellationToken)
    {
        var header = ZLinkClientCallCodec.CreateEnvelope(
            ZLinkMessageKind.Request,
            routerChannelId,
            ZLinkRemoteActorJoinPackets.HandoffCompletionPacketName,
            RemainingTimeout(absoluteDeadline));
        var parts = ZLinkRemoteActorJoinPackets.EncodeHandoffCompletionRequest(
            header,
            actorId,
            handoffId,
            sourceSpotId,
            sourceNodeRid,
            targetSpotId,
            operationId,
            admissionReply,
            boundSession,
            frames);
        //  The reconciliation runner retries this, so a request that never
        //  succeeds leaves the target completion - and with it the session
        //  route commit - simply absent. Name each attempt and its outcome.
        IReadOnlyList<Systems.Zlink.Message> replyParts;
        replyParts = await runtime.RequestToSpotViaRouterChannelAsync(
                routerChannelId,
                targetNodeRid,
                targetSpotId,
                targetSpotGeneration,
                targetNodeGeneration,
                authorityOwnerGeneration,
                ownerLeaseGeneration,
                parts,
                RemainingTimeout(absoluteDeadline),
                cancellationToken)
            .ConfigureAwait(false);
        _ = ZLinkClientCallCodec.DecodeEnvelopeReplyAndDispose<ZLinkRemoteActorHandoffCompletionRequest>(
            replyParts,
            "Remote actor handoff completion reply was empty.",
            $"Remote actor handoff completion failed for '{actorId}'.",
            null);
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

    private async ValueTask<ulong> SealBoundSessionRouteAsync(
        string actorId,
        ZLinkActorBoundSession session,
        string handoffId,
        CancellationToken cancellationToken)
    {
        var request = new ZLinkSessionRouteSealRequest(
            actorId,
            session.BindingToken,
            session.BindingGeneration,
            session.ObjectGeneration,
            session.AuthorityOwnerGeneration,
            session.MeshName,
            session.TargetNodeGeneration,
            session.OwnerLeaseGeneration,
            session.SessionOwnerNodeGeneration,
            handoffId);
        var sessionOwnerNode = session.SessionNodeRid!.Value;
        var meshName = session.MeshName;
        //  Placed before the local/remote branch: putting it inside one arm
        //  made its absence read as "never reached" when it only meant the
        //  other arm ran.
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"bound_seal_begin actor={actorId} session_node={sessionOwnerNode} "
            + $"local={sessionOwnerNode == runtime.GetMeshNodeRuntime(meshName).Node.RoutingId}");
        ZLinkSessionRouteSealReply reply;
        if (sessionOwnerNode
            == runtime.GetMeshNodeRuntime(meshName).Node.RoutingId)
        {
            var result = await runtime.SealSessionActorRouteAsync(
                new ZLinkSessionRouteSeal(
                    request.ActorId,
                    request.BindingToken,
                    request.BindingGeneration,
                    request.ObjectGeneration,
                    request.AuthorityOwnerGeneration,
                    request.MeshName,
                    request.TargetNodeGeneration,
                    request.OwnerLeaseGeneration,
                    request.SessionOwnerNodeGeneration,
                    request.HandoffId),
                cancellationToken).ConfigureAwait(false);
            reply = new ZLinkSessionRouteSealReply(
                result.Acknowledged,
                result.AcceptedHighWater);
        }
        else
        {
            reply = await runtime
                .RequestSessionRouteSealAsync(
                    meshName,
                    sessionOwnerNode,
                    request,
                    cancellationToken)
                .ConfigureAwait(false);
        }
        if (!reply.Acknowledged)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                $"Actor '{actorId}' session ingress seal was fenced by its binding identity.");
        return reply.AcceptedHighWater;
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
        try
        {
            await AbortBoundSessionRouteSealAsync(
                    actorId,
                    session,
                    handoffId,
                    CancellationToken.None)
                .ConfigureAwait(false);
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
        string handoffId,
        CancellationToken cancellationToken)
    {
        var request = new ZLinkSessionRouteAbortRequest(
            actorId,
            session.BindingToken,
            session.BindingGeneration,
            session.ObjectGeneration,
            session.AuthorityOwnerGeneration,
            session.MeshName,
            session.TargetNodeGeneration,
            session.OwnerLeaseGeneration,
            session.SessionOwnerNodeGeneration,
            handoffId);
        var sessionOwnerNode = session.SessionNodeRid!.Value;
        var meshName = session.MeshName;
        if (sessionOwnerNode
            == runtime.GetMeshNodeRuntime(meshName).Node.RoutingId)
        {
            var seal = new ZLinkSessionRouteSeal(
                request.ActorId,
                request.BindingToken,
                request.BindingGeneration,
                    request.ObjectGeneration,
                    request.AuthorityOwnerGeneration,
                    request.MeshName,
                    request.TargetNodeGeneration,
                    request.OwnerLeaseGeneration,
                    request.SessionOwnerNodeGeneration,
                request.HandoffId);
            _ = runtime.AbortSessionActorRouteSeal(seal);
            return;
        }

        _ = await runtime
            .RequestSessionRouteAbortAsync(
                meshName,
                sessionOwnerNode,
                request,
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
        actorState.InvalidateContext();
        await ReconcileActorLocationAfterMoveAsync(
                actorState,
                sourceAuthoritySnapshot,
                cancellationToken)
            .ConfigureAwait(false);
    }

    private async ValueTask NotifySourceActorLeftAsync(
        IZLinkActor actor,
        ZLinkActorRuntimeState actorState,
        CancellationToken cancellationToken)
    {
        if (actorState.LiveActivation is { } previousActivation)
            await previousActivation.NotifyActorLeftAfterCommittedMembershipAsync(
                    actor,
                    cancellationToken)
                .ConfigureAwait(false);
        else
            await runtime.NotifyEntrySpotActorLeftAsync(actor, cancellationToken: cancellationToken)
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
        IReadOnlyList<Message> replyParts,
        string actorId,
        string spotId)
    {
        try
        {
            if (result != RequestResult.Ok)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.NotFound,
                    $"Actor join was rejected for '{actorId}' to SPOT '{spotId}'.");

            if (replyParts.Count == 0)
                throw new InvalidOperationException(
                    "Actor join reply was empty.");

            var header = ZLinkEnvelopeCodec.DecodeHeader(replyParts);
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
