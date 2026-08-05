namespace Zlink.Framework.Runtime.Spots;

internal interface IZLinkActorInboundEndpoint
{
    IZLinkActor? ResolveActor(ZLinkActorRuntimeState state);

    ValueTask NotifyDisconnectedAsync(
        string actorId,
        CancellationToken cancellationToken);

    ValueTask DispatchAsync(
        IZLinkActor actor,
        ZLinkActorRuntimeState state,
        ZlinkStreamHeader header,
        Message body,
        bool relocationReplay,
        CancellationToken cancellationToken);

    ValueTask<ZLinkActorReply?> DispatchForReplyAsync(
        IZLinkActor actor,
        ZLinkActorRuntimeState state,
        ZlinkStreamHeader header,
        Message body,
        bool relocationReplay,
        CancellationToken cancellationToken);
}

internal sealed class ZLinkActorInboundPipeline(
    ZLinkFrameworkRuntime runtime,
    IZLinkActorInboundEndpoint endpoint)
{
    public async ValueTask DispatchAsync(
        ZLinkSpotActorFrameBatch frames,
        CancellationToken cancellationToken)
    {
        Exception? dispatchFailure = null;
        try
        {
            for (var i = 0; i < frames.Count; i++)
            {
                using var frame = frames[i];
                try
                {
                    await DispatchFrameAsync(frame, cancellationToken, allowCapture: true)
                        .ConfigureAwait(false);
                }
                catch (Exception exception)
                {
                    dispatchFailure = dispatchFailure is null
                        ? exception
                        : new AggregateException(dispatchFailure, exception);
                }
            }

            if (dispatchFailure is not null) throw dispatchFailure;
        }
        finally
        {
            frames.Dispose();
        }
    }

    public async ValueTask DispatchAsync(
        ZLinkSpotActorFrameBatch frames,
        ZLinkSpotSerialExecutor executor,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(executor);
        var dispatches = new Task[frames.Count];
        try
        {
            for (var i = 0; i < frames.Count; i++)
            {
                var frame = frames[i];
                dispatches[i] = executor.ExecuteActorAsync(
                    frame.Actor.ActorId,
                    async static (_, state, ct) =>
                    {
                        using (state.Frame)
                            await state.Pipeline.DispatchFrameAsync(
                                    state.Frame,
                                    ct,
                                    allowCapture: true)
                                .ConfigureAwait(false);
                    },
                    new ScheduledFrame(this, frame),
                    cancellationToken).AsTask();
            }

            await Task.WhenAll(dispatches).ConfigureAwait(false);
        }
        finally
        {
            frames.Dispose();
        }
    }

    public async ValueTask DispatchReplayAsync(
        ZLinkSpotActorFrameBatch frames,
        Action<long> acknowledgeFrame,
        CancellationToken cancellationToken)
    {
        await DispatchReplayAsync(
                frames,
                acknowledgeFrame,
                replayAdmission: null,
                cancellationToken)
            .ConfigureAwait(false);
    }

    internal async ValueTask DispatchSourceRestoreAsync(
        ZLinkSpotActorFrameBatch frames,
        Action acknowledgeFrame,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(acknowledgeFrame);
        try
        {
            for (var i = 0; i < frames.Count; i++)
            {
                using var frame = frames[i];
                await DispatchFrameAsync(
                        frame,
                        cancellationToken,
                        allowCapture: false,
                        acknowledgeHandledFrame: acknowledgeFrame,
                        relocationReplay: false)
                    .ConfigureAwait(false);
            }
        }
        finally
        {
            frames.Dispose();
        }
    }

    internal async ValueTask DispatchReplayAsync(
        ZLinkSpotActorFrameBatch frames,
        Action<long> acknowledgeFrame,
        ZLinkSpotRelocationReplayAdmission? replayAdmission,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(acknowledgeFrame);
        using var replayScope = replayAdmission is null
            ? null
            : ZLinkSpotRelocationReplayScope.Enter(replayAdmission);
        try
        {
            for (var i = 0; i < frames.Count; i++)
            {
                using var frame = frames[i];
                var arrivalIndex = frame.HandoffArrivalIndex
                                   ?? throw new InvalidOperationException(
                                       "Actor replay frame is missing its handoff arrival identity.");
                ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"actor_replay_frame_begin actor={frame.Actor.ActorId} "
                    + $"request_id={frame.RequestId} arrival={arrivalIndex}");
                await DispatchFrameAsync(
                        frame,
                        cancellationToken,
                        allowCapture: false,
                        acknowledgeHandledFrame: () => acknowledgeFrame(arrivalIndex))
                    .ConfigureAwait(false);
            }
        }
        finally
        {
            replayAdmission?.QueueReservation.Discard();
            frames.Dispose();
        }
    }

    internal async ValueTask DispatchCanonicalReplayAsync(
        ZLinkSpotActorFrameBatch frames,
        Func<ZLinkSpotActorFrame, ZLinkActorReply?, CancellationToken, ValueTask>
            completeFrame,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(completeFrame);
        try
        {
            for (var i = 0; i < frames.Count; i++)
            {
                using var frame = frames[i];
                await DispatchFrameAsync(
                        frame,
                        cancellationToken,
                        allowCapture: false,
                        completeCanonicalReplay: completeFrame)
                    .ConfigureAwait(false);
            }
        }
        finally
        {
            frames.Dispose();
        }
    }

    internal Task QueueCanonicalReplayAsync(
        ZLinkSpotActorFrameBatch frames,
        Func<ZLinkSpotActorFrame, ZLinkActorReply?, CancellationToken, ValueTask>
            completeFrame,
        CancellationToken cancellationToken)
    {
        return QueueCanonicalReplayAsync(
            frames,
            completeFrame,
            replayAdmission: null,
            cancellationToken);
    }

    internal Task QueueCanonicalReplayAsync(
        ZLinkSpotActorFrameBatch frames,
        Func<ZLinkSpotActorFrame, ZLinkActorReply?, CancellationToken, ValueTask>
            completeFrame,
        ZLinkSpotRelocationReplayAdmission? replayAdmission,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(completeFrame);
        var dispatches = new Task[frames.Count];
        for (var index = 0; index < frames.Count; index++)
        {
            var frame = frames[index];
            dispatches[index] = DispatchQueuedCanonicalFrameAsync(
                frame,
                completeFrame,
                replayAdmission,
                cancellationToken);
        }
        return CompleteQueuedCanonicalBatchAsync(dispatches, frames);
    }

    private async Task DispatchQueuedCanonicalFrameAsync(
        ZLinkSpotActorFrame frame,
        Func<ZLinkSpotActorFrame, ZLinkActorReply?, CancellationToken, ValueTask>
            completeFrame,
        ZLinkSpotRelocationReplayAdmission? replayAdmission,
        CancellationToken cancellationToken)
    {
        using var replayScope = replayAdmission is null
            ? null
            : ZLinkSpotRelocationReplayScope.Enter(replayAdmission);
        try
        {
            using (frame)
                await DispatchFrameAsync(
                        frame,
                        cancellationToken,
                        allowCapture: false,
                        completeCanonicalReplay: completeFrame)
                    .ConfigureAwait(false);
        }
        finally
        {
            replayAdmission?.QueueReservation.Discard();
        }
    }

    private static async Task CompleteQueuedCanonicalBatchAsync(
        Task[] dispatches,
        ZLinkSpotActorFrameBatch frames)
    {
        try
        {
            await Task.WhenAll(dispatches).ConfigureAwait(false);
        }
        finally
        {
            frames.Dispose();
        }
    }

    private async ValueTask DispatchFrameAsync(
        ZLinkSpotActorFrame frame,
        CancellationToken cancellationToken,
        bool allowCapture,
        Action? acknowledgeHandledFrame = null,
        Func<ZLinkSpotActorFrame, ZLinkActorReply?, CancellationToken, ValueTask>?
            completeCanonicalReplay = null,
        bool? relocationReplay = null)
    {
        using var flow = ZLinkFlowContext.Enter(
            frame.Header.FlowId,
            frame.Header.FlowOrigin is { } streamOrigin
                ? (ZLinkFlowOrigin)(byte)streamOrigin
                : null,
            runtime.Flow.CaptureEnabled,
            ZLinkFlowOrigin.Inbound);
        var state = runtime.GetOrCreateActorState(frame.Actor.ActorId);
        var capture = allowCapture
            ? state.Handoff.TryCapture(
                frame,
                () => EnsureRelocationReplyRoute(runtime, frame))
            : ZLinkActorHandoffCaptureResult.NotSealed;
        if (capture == ZLinkActorHandoffCaptureResult.Captured)
            return;
        if (capture == ZLinkActorHandoffCaptureResult.Full)
        {
            await CompleteMovingBoundaryAsync(
                    frame,
                    acknowledgeHandledFrame,
                    cancellationToken)
                .ConfigureAwait(false);
            return;
        }
        if (allowCapture
            && state.Handoff.BlocksLocalDispatch
            && !frame.RouteContext.IsDirectRoute)
        {
            await CompleteMovingBoundaryAsync(
                    frame,
                    acknowledgeHandledFrame,
                    cancellationToken)
                .ConfigureAwait(false);
            return;
        }
        if (state.IsDispatchBlocked)
        {
            await ZLinkActorBoundSessionRelay.ReplyStaleActorAsync(
                    runtime,
                    frame.Actor,
                    frame.SourceNodeRid,
                    frame.SourceSessionRid,
                    frame.RequestId,
                    frame.Flags,
                    frame.RouteContext.ReplyCapability,
                    frame.Header,
                    new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.NotFound,
                        $"Actor '{frame.Actor.ActorId}' is being destroyed."),
                    cancellationToken,
                    frame.DirectReply)
                .ConfigureAwait(false);
            acknowledgeHandledFrame?.Invoke();
            return;
        }
        if (await RouteAwayFromCurrentActorAsync(state, frame, cancellationToken)
                .ConfigureAwait(false))
        {
            acknowledgeHandledFrame?.Invoke();
            return;
        }

        var actor = endpoint.ResolveActor(state);
        Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
            $"inbound_resolve actor={frame.Actor.ActorId} resolved={actor is not null} "
            + $"request_id={frame.RequestId}");
        if (actor is null)
        {
            //  A bound request whose incarnation is gone used to get no reply at
            //  all: the helper below answers no-bind requests only, and the frame
            //  was then acknowledged and dropped, leaving the caller to time out.
            //  Spec 07-stream-session names this case ActorGenerationStale.
            if (!await ZLinkActorBoundSessionRelay.TryReplyMissingNoBindActorAsync(
                    runtime,
                    frame.Actor,
                    frame.SourceNodeRid,
                    frame.SourceSessionRid,
                    frame.RequestId,
                    frame.Flags,
                    frame.RouteContext.ReplyCapability,
                    frame.Header,
                    frame.DirectReply,
                    cancellationToken)
                .ConfigureAwait(false))
                await ZLinkActorBoundSessionRelay.ReplyStaleActorAsync(
                        runtime,
                        frame.Actor,
                        frame.SourceNodeRid,
                        frame.SourceSessionRid,
                        frame.RequestId,
                        frame.Flags,
                        frame.RouteContext.ReplyCapability,
                        frame.Header,
                        new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.InvalidOperation,
                            $"Actor '{frame.Actor.ActorId}' session binding names an incarnation that no longer exists."),
                        cancellationToken,
                        frame.DirectReply)
                    .ConfigureAwait(false);
            acknowledgeHandledFrame?.Invoke();
            return;
        }

        //  Spec 25는 `zlink.mesh_node.requests.inflight`를 surface별로 요구하고,
        //  가이드 07-actor-spot은 actor가 처리 중인 request 수를 `surface=actor`로
        //  관측한다고 정한다. Channel·spot surface만 계측돼 있어 actor request는
        //  어느 값에도 잡히지 않았다.
        var requestMetric = frame.RequestId != 0
            ? Diagnostics.ZLinkRuntimeMetrics.StartRequest(
                actor.Context.MeshName,
                "actor")
            : null;
        var requestOutcome = "completed";
        try
        {
            await DispatchCurrentActorAsync(
                    actor,
                    state,
                    frame,
                    acknowledgeHandledFrame,
                    completeCanonicalReplay,
                    relocationReplay,
                    cancellationToken)
                .ConfigureAwait(false);
        }
        catch (TimeoutException)
        {
            requestOutcome = "timed_out";
            throw;
        }
        catch (OperationCanceledException)
        {
            requestOutcome = "cancelled";
            throw;
        }
        catch (ZLinkFrameworkException exception)
            when (allowCapture
                  && exception.Kind == ZLinkFrameworkErrorKind.NotFound
                  && state.Handoff.BlocksLocalDispatch)
        {
            var retryCapture = state.Handoff.TryCapture(
                frame,
                () => EnsureRelocationReplyRoute(runtime, frame));
            if (retryCapture == ZLinkActorHandoffCaptureResult.Captured)
                return;

            // A bound-session frame cannot be made durable because it has no
            // owner lease fence. Requests receive an observable retry terminal;
            // one-way sends have no reply contract and stop at this boundary.
            await CompleteMovingBoundaryAsync(
                    frame,
                    acknowledgeHandledFrame,
                    cancellationToken)
                .ConfigureAwait(false);
        }
        catch (ZLinkFrameworkException exception)
            when (allowCapture
                  && exception.Kind == ZLinkFrameworkErrorKind.Rejected
                  && IsRelocationAdmissionRejection(state))
        {
            // The frame won the inbound handoff race, but its actor turn
            // reached the Spot admission barrier after relocation sealed the
            // application queue. It still owns a reply route; letting this
            // rejection escape would leave the session's accepted frame
            // pending until timeout and block the route seal.
            await CompleteMovingBoundaryAsync(
                    frame,
                    acknowledgeHandledFrame,
                    cancellationToken)
                .ConfigureAwait(false);
        }
        catch
        {
            requestOutcome = "failed";
            throw;
        }
        finally
        {
            requestMetric?.Complete(requestOutcome);
        }
    }

    internal static void EnsureRelocationReplyRoute(
        ZLinkSpotActorFrame frame)
    {
        if (frame.RelocationReplyRouteId != 0
            || !frame.RouteContext.IsDirectRoute
            || frame.Header.Kind != ZlinkStreamMessageKind.Request)
            return;
        var routeId = frame.RouteContext.ReplyRequestId;
        if (routeId == 0
            || frame.RouteContext.OperationId.Low == 0
            || frame.RouteContext.OperationId.High == 0)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Rejected,
                "A direct Actor request has no source-owned relocation reply route.");
        frame.BindRelocationReplyRoute(routeId);
    }

    internal static void EnsureRelocationReplyRoute(
        ZLinkFrameworkRuntime runtime,
        ZLinkSpotActorFrame frame)
    {
        EnsureRelocationReplyRoute(frame);
        if (frame.Header.Kind != ZlinkStreamMessageKind.Request
            || frame.RouteContext.ReplyCapability is not null)
            return;
        var directReply = frame.DirectReply
                          ?? throw new ZLinkFrameworkException(
                              ZLinkFrameworkErrorKind.Rejected,
                              "A direct Actor request has no reply route to preserve for relocation.");
        var preserved = runtime.ActorMessageFollower.PreserveDirectReply(
            frame.Actor.NodeRid,
            frame.Actor.ActorId,
            frame.RequestId,
            frame.RouteContext.DeadlineUnixMs,
            directReply);
        frame.BindRelocationReplyCapability(
            preserved.Capability,
            preserved.Reply);
    }

    private async ValueTask CompleteMovingBoundaryAsync(
        ZLinkSpotActorFrame frame,
        Action? acknowledgeHandledFrame,
        CancellationToken cancellationToken)
    {
        if (frame.Header.Kind == ZlinkStreamMessageKind.Request)
            await ReplyMovingAsync(frame, cancellationToken).ConfigureAwait(false);
        acknowledgeHandledFrame?.Invoke();
    }

    private ValueTask ReplyMovingAsync(
        ZLinkSpotActorFrame frame,
        CancellationToken cancellationToken) =>
        ZLinkActorBoundSessionRelay.ReplyStaleActorAsync(
            runtime,
            frame.Actor,
            frame.SourceNodeRid,
            frame.SourceSessionRid,
            frame.RequestId,
            frame.Flags,
            frame.RouteContext.ReplyCapability,
            frame.Header,
            new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"Actor '{frame.Actor.ActorId}' is moving."),
            cancellationToken,
            frame.DirectReply);

    private static bool IsRelocationAdmissionRejection(
        ZLinkActorRuntimeState state) =>
        state.Handoff.BlocksLocalDispatch
        || state.LiveActivation?.HasRelocationBarrier == true;

    private async ValueTask<bool> RouteAwayFromCurrentActorAsync(
        ZLinkActorRuntimeState state,
        ZLinkSpotActorFrame frame,
        CancellationToken cancellationToken)
    {
        try
        {
            if (!ZLinkActorMessageFollowDispatcher.TryFollow(
                    runtime,
                    state,
                    frame.Actor,
                    frame.SourceNodeRid,
                    frame.SourceSessionRid,
                    frame.RequestId,
                    frame.Flags,
                    frame.RouteContext,
                    frame.Header,
                    frame.Body,
                    frame.SourceNodeGeneration,
                    frame.RequestSource,
                    frame.DirectReply))
                return false;
        }
        catch (ZLinkFrameworkException exception)
            when (exception.Kind == ZLinkFrameworkErrorKind.Unavailable)
        {
            await ZLinkActorBoundSessionRelay.ReplyStaleActorAsync(
                    runtime,
                    frame.Actor,
                    frame.SourceNodeRid,
                    frame.SourceSessionRid,
                    frame.RequestId,
                    frame.Flags,
                    frame.RouteContext.ReplyCapability,
                    frame.Header,
                    exception,
                    cancellationToken,
                    frame.DirectReply)
                .ConfigureAwait(false);
            return true;
        }

        return true;
    }

    private async ValueTask DispatchCurrentActorAsync(
        IZLinkActor actor,
        ZLinkActorRuntimeState state,
        ZLinkSpotActorFrame frame,
        Action? acknowledgeHandledFrame,
        Func<ZLinkSpotActorFrame, ZLinkActorReply?, CancellationToken, ValueTask>?
            completeCanonicalReplay,
        bool? relocationReplay,
        CancellationToken cancellationToken)
    {
        //  Between the relay dispatch and this branch there is no record of
        //  what arrived, so a frame that is not recognised as a disconnect
        //  simply continues as ordinary traffic.
        if (ZLinkActorBoundSessionRelay.IsSessionDisconnectedPacket(frame.Header))
        {
            if (!ZLinkActorBoundSessionRelay.TryValidateDisconnectedBinding(
                    state,
                    frame.SourceNodeRid,
                    frame.SourceSessionRid,
                    frame.Body,
                    out var bindingToken))
            {
                if (runtime.Flow.Enabled(ZLinkMessageFlowOutcome.Dropped))
                    runtime.Flow.Trace(new ZLinkMessageFlowEvent(
                        ZLinkMessageFlowOutcome.Dropped,
                        ZLinkDispatchErrorSurface.StreamSession,
                        ZLinkDispatchMessageKind.Send,
                        ActorId: actor.Context.ActorId,
                        ErrorReason: ZLinkDispatchErrorReason.InvalidFrame,
                        ErrorAction: ZLinkDispatchErrorAction.Drop,
                        ErrorMessage: "session disconnect did not match the current binding"));
                acknowledgeHandledFrame?.Invoke();
                return;
            }
            try
            {
                await endpoint.NotifyDisconnectedAsync(actor.Context.ActorId, cancellationToken)
                    .ConfigureAwait(false);
            }
            finally
            {
                runtime.RemoveActorSessionBinding(actor.Context.ActorId, bindingToken);
                acknowledgeHandledFrame?.Invoke();
            }
            return;
        }

        var boundSession = ZLinkActorBoundSessionRelay.EnterDispatch(
            runtime,
            actor.Context.ActorId,
            frame.SourceNodeRid,
            frame.SourceSessionRid,
            frame.RequestId,
            frame.Flags);

        try
        {
            if (await ZLinkRemoteSessionBindingHandler.TryHandleAsync(
                    runtime,
                    actor,
                    state,
                    frame,
                    boundSession,
                    acknowledgeHandledFrame,
                    cancellationToken).ConfigureAwait(false))
            {
                return;
            }

            if (frame.Header.Kind == ZlinkStreamMessageKind.Request
                && frame.Header.RequestSeq is not null)
            {
                var reply = await endpoint.DispatchForReplyAsync(
                        actor,
                        state,
                        frame.Header,
                        frame.Body,
                        relocationReplay: relocationReplay
                            ?? (acknowledgeHandledFrame is not null
                                || completeCanonicalReplay is not null),
                        cancellationToken)
                    .ConfigureAwait(false);
                ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"actor_dispatch_reply actor={actor.Context.ActorId} "
                    + $"request_id={frame.RequestId} reply={reply is not null} "
                    + $"activation={state.LiveActivation?.SpotId ?? "<entry>"}");
                if (completeCanonicalReplay is not null)
                {
                    ZLinkFrameworkDebugLog.SpotDiscovery(
                        $"actor_canonical_replay_begin actor={actor.Context.ActorId} "
                        + $"request_id={frame.RequestId} reply={reply is not null}");
                    await boundSession.DrainAsync(cancellationToken)
                        .ConfigureAwait(false);
                    await completeCanonicalReplay(frame, reply, cancellationToken)
                        .ConfigureAwait(false);
                    ZLinkFrameworkDebugLog.SpotDiscovery(
                        $"actor_canonical_replay_completed actor={actor.Context.ActorId} "
                        + $"request_id={frame.RequestId}");
                    return;
                }
                if (acknowledgeHandledFrame is not null)
                {
                    try
                    {
                        acknowledgeHandledFrame.Invoke();
                        ZLinkFrameworkDebugLog.SpotDiscovery(
                            $"actor_replay_frame_acknowledged actor={actor.Context.ActorId} "
                            + $"request_id={frame.RequestId}");
                    }
                    catch (Exception exception)
                    {
                        ZLinkFrameworkDebugLog.SpotDiscovery(
                            $"actor_replay_frame_ack_failed actor={actor.Context.ActorId} "
                            + $"request_id={frame.RequestId} "
                            + $"error={exception.GetType().Name}:{exception.Message}");
                        throw;
                    }
                }
                if (reply is not null)
                {
                    ZLinkFrameworkDebugLog.SpotDiscovery(
                        $"actor_handoff_reply_begin actor={actor.Context.ActorId} "
                        + $"request_id={frame.RequestId} source_node={frame.SourceNodeRid} "
                        + $"no_bind={boundSession.IsNoBind}");
                    if (acknowledgeHandledFrame is null)
                        await ZLinkActorBoundSessionRelay.SendReplyAsync(
                                runtime,
                                actor.Context.ActorId,
                                frame.Actor,
                                frame.SourceNodeRid,
                                frame.SourceSessionRid,
                                frame.RequestId,
                                frame.Flags,
                                frame.RouteContext.ReplyCapability,
                                boundSession.IsNoBind,
                                frame.Header,
                                reply,
                                cancellationToken,
                                frame.DirectReply)
                            .ConfigureAwait(false);
                    else
                        await ReconcileReplayFinalizationAsync(
                                "actor handoff reply",
                                ct => ZLinkActorBoundSessionRelay.SendReplyAsync(
                                    runtime,
                                    actor.Context.ActorId,
                                    frame.Actor,
                                    frame.SourceNodeRid,
                                    frame.SourceSessionRid,
                                    frame.RequestId,
                                    frame.Flags,
                                    frame.RouteContext.ReplyCapability,
                                    boundSession.IsNoBind,
                                    frame.Header,
                                    reply,
                                    ct,
                                    frame.DirectReply))
                            .ConfigureAwait(false);
                }

                if (acknowledgeHandledFrame is null)
                    await boundSession.DrainAsync(cancellationToken).ConfigureAwait(false);
                else
                    await ReconcileReplayFinalizationAsync(
                            "actor handoff deferred operation",
                            boundSession.DrainAsync)
                        .ConfigureAwait(false);
                return;
            }

            await endpoint.DispatchAsync(
                    actor,
                    state,
                    frame.Header,
                    frame.Body,
                    relocationReplay: relocationReplay
                        ?? (acknowledgeHandledFrame is not null
                            || completeCanonicalReplay is not null),
                    cancellationToken)
                .ConfigureAwait(false);
            if (completeCanonicalReplay is not null)
            {
                await boundSession.DrainAsync(cancellationToken)
                    .ConfigureAwait(false);
                await completeCanonicalReplay(frame, null, cancellationToken)
                    .ConfigureAwait(false);
                return;
            }
            acknowledgeHandledFrame?.Invoke();
            if (acknowledgeHandledFrame is not null)
                await ReconcileReplayFinalizationAsync(
                        "actor handoff deferred operation",
                        boundSession.DrainAsync)
                    .ConfigureAwait(false);
        }
        finally
        {
            if (acknowledgeHandledFrame is null)
                await boundSession.DisposeAsync().ConfigureAwait(false);
            else
                await ReconcileReplayFinalizationAsync(
                        "actor handoff dispatch scope disposal",
                        _ => boundSession.DisposeAsync())
                    .ConfigureAwait(false);
        }
    }

    private async ValueTask ReconcileReplayFinalizationAsync(
        string operation,
        Func<CancellationToken, ValueTask> finalize)
    {
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"actor_handoff_finalize_begin operation={operation}");
        await ZLinkReconciliationRunner.RunAsync(
                finalize,
                exception => ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"{operation} retry: {exception.Message}"),
                runtime.ShutdownToken)
            .ConfigureAwait(false);
    }

    private sealed record ScheduledFrame(
        ZLinkActorInboundPipeline Pipeline,
        ZLinkSpotActorFrame Frame);

}

internal sealed class ZLinkEntrySpotActorInboundEndpoint(
    ZLinkFrameworkRuntime runtime) : IZLinkActorInboundEndpoint
{
    public IZLinkActor? ResolveActor(ZLinkActorRuntimeState state) => state.Actor;

    public async ValueTask NotifyDisconnectedAsync(
        string actorId,
        CancellationToken cancellationToken)
    {
        if (!await runtime.TryNotifyJoinedSpotActorDisconnectedAsync(actorId, cancellationToken)
                .ConfigureAwait(false))
            await runtime.NotifyActorDisconnectedByIdAsync(actorId, cancellationToken)
                .ConfigureAwait(false);
    }

    public ValueTask DispatchAsync(
        IZLinkActor actor,
        ZLinkActorRuntimeState state,
        ZlinkStreamHeader header,
        Message body,
        bool relocationReplay,
        CancellationToken cancellationToken)
    {
        return runtime.SubmitActorAsync(
            actor,
            header,
            body,
            relocationReplay,
            cancellationToken);
    }

    public async ValueTask<ZLinkActorReply?> DispatchForReplyAsync(
        IZLinkActor actor,
        ZLinkActorRuntimeState state,
        ZlinkStreamHeader header,
        Message body,
        bool relocationReplay,
        CancellationToken cancellationToken)
    {
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"actor_dispatch_entry_path actor={actor.Context.ActorId} "
            + $"correlation_id={header.CorrelationId} live_activation={state.LiveActivation is not null}");
        if (state.LiveActivation is not null)
            return await runtime.SubmitActorForReplyAsync(
                    actor.Context.ActorId,
                    header,
                    body,
                    relocationReplay,
                    cancellationToken)
                .ConfigureAwait(false);

        var result = await runtime.TrySubmitEntrySpotActorForReplyAsync(
                actor,
                state,
                header,
                body,
                callerOwnsDispatchTurn: false,
                relocationReplay,
                cancellationToken)
            .ConfigureAwait(false);
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"actor_dispatch_entry_result actor={actor.Context.ActorId} "
            + $"correlation_id={header.CorrelationId} handled={result.Handled} "
            + $"reply={result.Reply is not null}");
        return result.Handled
            ? result.Reply
            : await runtime.SubmitActorForReplyAsync(
                    actor.Context.ActorId,
                    header,
                    body,
                    relocationReplay,
                    cancellationToken)
                .ConfigureAwait(false);
    }
}

internal sealed class ZLinkUserSpotActorInboundEndpoint(
    ZLinkFrameworkRuntime runtime,
    ZLinkSpotActorMembership actors,
    ZLinkSpotActorPacketDispatcher dispatcher) : IZLinkActorInboundEndpoint
{
    public IZLinkActor? ResolveActor(ZLinkActorRuntimeState state)
    {
        return actors.TryGetActor(state.ActorId, out var actor) && actor is not null
            ? actor
            : state.Actor;
    }

    public ValueTask NotifyDisconnectedAsync(
        string actorId,
        CancellationToken cancellationToken)
    {
        return runtime.NotifyActorDisconnectedByIdAsync(actorId, cancellationToken);
    }

    public ValueTask DispatchAsync(
        IZLinkActor actor,
        ZLinkActorRuntimeState state,
        ZlinkStreamHeader header,
        Message body,
        bool relocationReplay,
        CancellationToken cancellationToken)
    {
        return dispatcher.DispatchAsync(actor, state, header, body, cancellationToken);
    }

    public ValueTask<ZLinkActorReply?> DispatchForReplyAsync(
        IZLinkActor actor,
        ZLinkActorRuntimeState state,
        ZlinkStreamHeader header,
        Message body,
        bool relocationReplay,
        CancellationToken cancellationToken)
    {
        return dispatcher.DispatchForReplyAsync(actor, state, header, body, cancellationToken);
    }
}
