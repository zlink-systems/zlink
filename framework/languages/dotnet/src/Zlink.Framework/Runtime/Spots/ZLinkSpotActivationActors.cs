namespace Zlink.Framework.Runtime.Spots;

internal sealed partial class ZLinkSpotActivation
{
    private readonly HashSet<string> _actorsLeavingForEntrySpot = new(StringComparer.Ordinal);

    public ValueTask LeaveActorAsync(
        IZLinkActor actor,
        CancellationToken cancellationToken = default)
    {
        EnsureContextOperationAllowed();
        ArgumentNullException.ThrowIfNull(actor);
        if (ZLinkBoundSessionDispatchScope.TryDefer(
                actor.Context.ActorId,
                ct => LeaveActorCoreAsync(actor, ct)))
            return ValueTask.CompletedTask;

        return ReferenceEquals(ZLinkSpotAmbientContext.CurrentOrDefault, this)
            ? LeaveActorCoreAsync(actor, cancellationToken)
            : ExecuteSerializedAsync(
                static (activation, state, ct) => activation.LeaveActorCoreAsync(state, ct),
                actor,
                cancellationToken);
    }

    public bool TryResolveActorJoinDescriptor(out ZLinkSpotActorJoinDescriptor? descriptor)
    {
        return _actorJoins.TryResolve(out descriptor);
    }

    public bool TryResolveActorPacketDescriptor(
        Type actorType,
        ZlinkStreamHeader header,
        out ZLinkSpotActorPacketDescriptor? descriptor)
    {
        descriptor = null;
        return _actorHandlers is not null
               && _actorHandlers.TryResolve(actorType, header, out descriptor);
    }

    public bool TryGetJoinedActor(
        string actorId,
        out IZLinkActor? actor)
    {
        return _actors.TryGetActor(actorId, out actor);
    }

    public async ValueTask<ZLinkSpotActorJoinResult> JoinActorAsync(
        IZLinkActor actor,
        ZLinkMessage request,
        CancellationToken cancellationToken,
        DateTimeOffset? absoluteDeadline = null)
    {
        ArgumentNullException.ThrowIfNull(request);

        if (!_actorJoins.TryResolve(out var descriptor)
            || descriptor is null)
            throw new InvalidOperationException(
                $"SPOT '{Spot.GetType()}' does not declare an actor join callback.");

        TraceActorJoin(ZLinkMessageFlowOutcome.Received, actor.Context.ActorId);
        var state = new ActorJoinCallState(
            actor,
            request,
            descriptor,
            absoluteDeadline);
        if (ReferenceEquals(ZLinkSpotAmbientContext.CurrentOrDefault, this))
        {
            state.Result = await InvokeActorJoinAsync(
                    state.Descriptor,
                    state.Actor,
                    state.Request,
                    cancellationToken)
                .ConfigureAwait(false);
            if (state.Result.Accepted)
                await CommitActorJoinCoreAsync(
                        state.Actor,
                        cancellationToken,
                        state.AbsoluteDeadline)
                    .ConfigureAwait(false);

            TraceActorJoin(ZLinkMessageFlowOutcome.Replied, actor.Context.ActorId);
            return state.Result;
        }

        await ExecuteSerializedAsync(
            async static (activation, state, ct) =>
            {
                state.Result = await activation.InvokeActorJoinAsync(
                    state.Descriptor,
                    state.Actor,
                    state.Request,
                    ct);
                if (state.Result.Accepted)
                    await activation.CommitActorJoinCoreAsync(
                            state.Actor,
                            ct,
                            state.AbsoluteDeadline)
                        .ConfigureAwait(false);
            },
            state,
            cancellationToken);

        TraceActorJoin(ZLinkMessageFlowOutcome.Replied, actor.Context.ActorId);
        return state.Result;
    }

    private void TraceActorJoin(ZLinkMessageFlowOutcome outcome, string actorId)
    {
        if (!_runtime.Flow.Enabled(outcome)) return;

        _runtime.Flow.Trace(new ZLinkMessageFlowEvent(
            outcome,
            ZLinkDispatchErrorSurface.SpotActor,
            ZLinkDispatchMessageKind.ActorRequest,
            "JoinSpot",
            ChannelName,
            ActorId: actorId,
            SpotId: SpotId.ToString()));
    }

    public async ValueTask<ZLinkSpotActorJoinResult> AdmitRemoteActorJoinAsync(
        string actorId,
        ZLinkMessage request,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(request);

        if (!_actorJoins.TryResolve(out var descriptor)
            || descriptor is null)
            throw new InvalidOperationException(
                $"SPOT '{Spot.GetType()}' does not declare an actor join callback.");

        var state = new ActorJoinAdmissionCallState(actorId, request, descriptor);
        if (ReferenceEquals(ZLinkSpotAmbientContext.CurrentOrDefault, this))
        {
            state.Result = await HandlerInvoker.InvokeActorJoinAsync(
                    state.Descriptor,
                    state.ActorId,
                    state.Request,
                    cancellationToken)
                .ConfigureAwait(false);
            return state.Result;
        }

        await ExecuteSerializedAsync(
            async static (activation, state, ct) =>
            {
                state.Result = await activation.HandlerInvoker.InvokeActorJoinAsync(
                    state.Descriptor,
                    state.ActorId,
                    state.Request,
                    ct);
            },
            state,
            cancellationToken);

        return state.Result;
    }

    internal async ValueTask<ZLinkSpotActorJoinResult> AdmitActorJoinFromCallerTurnAsync(
        IZLinkActor actor,
        ZLinkMessage request,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(actor);
        ArgumentNullException.ThrowIfNull(request);
        if (!_actorJoins.TryResolve(out var descriptor) || descriptor is null)
            throw new InvalidOperationException(
                $"SPOT '{Spot.GetType()}' does not declare an actor join callback.");

        TraceActorJoin(ZLinkMessageFlowOutcome.Received, actor.Context.ActorId);
        var result = await InvokeActorJoinAsync(descriptor, actor, request, cancellationToken)
            .ConfigureAwait(false);
        TraceActorJoin(ZLinkMessageFlowOutcome.Replied, actor.Context.ActorId);
        return result;
    }

    internal ValueTask CommitActorJoinFromCallerTurnAsync(
        IZLinkActor actor,
        CancellationToken cancellationToken,
        DateTimeOffset? absoluteDeadline = null)
    {
        ArgumentNullException.ThrowIfNull(actor);
        return CommitActorJoinCoreAsync(
            actor,
            cancellationToken,
            absoluteDeadline);
    }

    internal void StageRelocatedPerActorMember(
        IZLinkActor actor,
        ZLinkActorRuntimeState actorState)
    {
        ArgumentNullException.ThrowIfNull(actor);
        ArgumentNullException.ThrowIfNull(actorState);
        if (ExecutionMode != ZLinkUserSpotExecutionMode.PerActor
            || Spot is IZLinkInstanceSpot
            || actor.Context.ActorId != actorState.ActorId)
            throw new InvalidOperationException(
                "A relocated Actor can attach only to its exact PerActor User Spot shell.");

        actorState.JoinSpot(this);
    }

    internal void PublishRelocatedPerActorMember(
        IZLinkActor actor,
        ZLinkActorRuntimeState actorState)
    {
        ArgumentNullException.ThrowIfNull(actor);
        ArgumentNullException.ThrowIfNull(actorState);
        if (!ReferenceEquals(actorState.LiveActivation, this))
            throw new InvalidOperationException(
                "A relocated Actor must retain its staged PerActor shell binding "
                + "until authority publication.");
        _actors.Add(actor);
    }

    internal void AbortStagedRelocatedPerActorMember(
        IZLinkActor actor,
        ZLinkActorRuntimeState actorState) =>
        DetachRelocatedPerActorMember(actor, actorState);

    internal void DetachRelocatedPerActorMember(
        IZLinkActor actor,
        ZLinkActorRuntimeState actorState)
    {
        ArgumentNullException.ThrowIfNull(actor);
        ArgumentNullException.ThrowIfNull(actorState);
        _actors.RemoveIfCurrent(actor);
        actorState.LeaveSpotIfCurrent(this);
        SignalPerActorMembersDrainedIfNeeded();
    }

    public ValueTask PrepareTransferredActorJoinAndReplayAsync(
        IZLinkActor actor,
        ZLinkActorRuntimeState actorState,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(actor);
        return ReferenceEquals(ZLinkSpotAmbientContext.CurrentOrDefault, this)
            ? PrepareTransferredActorJoinAndReplayCoreAsync(actor, actorState, cancellationToken)
            : ExecuteSerializedAsync(
                static (activation, state, ct) => activation.PrepareTransferredActorJoinAndReplayCoreAsync(
                    state.Actor,
                    state.ActorState,
                    ct),
                (Actor: actor, ActorState: actorState),
                cancellationToken);
    }

    private async ValueTask PrepareTransferredActorJoinAndReplayCoreAsync(
        IZLinkActor actor,
        ZLinkActorRuntimeState actorState,
        CancellationToken cancellationToken)
    {
        try
        {
            // Materialize membership during prepare, but do not expose the join to application
            // code until the distributed handoff has committed. A joined callback may push a
            // client notification, and that notification must not overtake source cutover.
            _ = await StageActorMembershipAsync(actor, cancellationToken)
                .ConfigureAwait(false);

        }
        catch
        {
            _actors.RemoveIfCurrent(actor);
            actorState.LeaveSpotIfCurrent(this);
            throw;
        }
    }

    internal ValueTask CompleteTransferredActorJoinAsync(
        ZLinkActorRuntimeState actorState,
        CancellationToken cancellationToken)
    {
        var actor = actorState.Actor
                    ?? throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.NotFound,
                        $"Actor '{actorState.ActorId}' has no transferred instance at commit.");
        return ReferenceEquals(ZLinkSpotAmbientContext.CurrentOrDefault, this)
            ? NotifyJoinedActorCoreAsync(actor, cancellationToken)
            : ExecuteSerializedAsync(
                static (activation, state, ct) => activation.NotifyJoinedActorCoreAsync(state, ct),
                actor,
                cancellationToken);
    }

    internal ValueTask CompleteTransferredActorJoinLifecycleAsync(
        ZLinkActorRuntimeState actorState,
        string handoffId,
        CancellationToken cancellationToken)
    {
        var actor = actorState.Actor
                    ?? throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.NotFound,
                        $"Actor '{actorState.ActorId}' has no transferred instance at commit.");
        return _serial.ExecuteApplicationCallbackAsync(
            static (activation, state, ct) => activation.CompleteTransferredActorJoinLifecycleCoreAsync(
                state.Actor,
                state.ActorState,
                state.HandoffId,
                ct),
            (Actor: actor, ActorState: actorState, HandoffId: handoffId),
            cancellationToken);
    }

    private async ValueTask CompleteTransferredActorJoinLifecycleCoreAsync(
        IZLinkActor actor,
        ZLinkActorRuntimeState actorState,
        string handoffId,
        CancellationToken cancellationToken)
    {
        if (!actorState.Handoff.TryBeginJoinedNotification(handoffId)) return;
        try
        {
            await NotifyJoinedActorCoreAsync(actor, cancellationToken)
                .ConfigureAwait(false);
            actorState.Handoff.CompleteJoinedNotification(handoffId);
        }
        catch
        {
            actorState.Handoff.RetryJoinedNotification(handoffId);
            throw;
        }
    }

    internal ValueTask CompleteTransferredActorJoinSealedAsync(
        ZLinkActorRuntimeState actorState,
        ZLinkSpotRelocationSeal seal,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(seal);
        var actor = actorState.Actor
                    ?? throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.NotFound,
                        $"Actor '{actorState.ActorId}' has no transferred instance at commit.");
        return _serial.ExecuteSealedRelocationAsync(
            seal.QueueSeal,
            (activation, ct) =>
                activation.NotifyJoinedActorCoreAsync(actor, ct),
            cancellationToken);
    }

    internal async ValueTask ReplayTransferredActorHandoffAsync(
        ZLinkActorRuntimeState actorState,
        IReadOnlyList<ZLinkActorHandoffFrame> sourceFrames,
        CancellationToken cancellationToken)
    {
        var frames = actorState.Handoff.PrepareImportedReplay(sourceFrames);
        if (frames.Count == 0) return;

        var actorRef = actorState.NativeActorRef
                       ?? throw new ZLinkFrameworkException(
                           ZLinkFrameworkErrorKind.NotFound,
                           $"Actor '{actorState.ActorId}' does not have a native Actor ref during handoff completion.");
        foreach (var frame in frames.OrderBy(static frame => frame.ArrivalIndex))
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"backlog_enqueued actor={actorState.ActorId} arrival={frame.ArrivalIndex} trailing=true request_id={frame.RequestId} flags={frame.Flags}");
        await _dispatcher.DispatchActorReplayFramesAsync(
                ZLinkActorHandoffFrames.Restore(actorRef, frames),
                actorState.Handoff.AcknowledgeReplayedFrame,
                cancellationToken)
            .ConfigureAwait(false);
    }

    internal async ValueTask ReplayFinalTransferredActorHandoffAsync(
        ZLinkActorRuntimeState actorState,
        string handoffId,
        CancellationToken cancellationToken)
    {
        var actorRef = actorState.NativeActorRef
                       ?? throw new ZLinkFrameworkException(
                           ZLinkFrameworkErrorKind.NotFound,
                           $"Actor '{actorState.ActorId}' does not have a native Actor ref during final handoff replay.");
        while (true)
        {
            var frames = actorState.Handoff.SnapshotFinalReplay();
            _runtime.LogActorHandoff(
                $"handoff_final_replay_snapshot actor={actorState.ActorId} frames={frames.Count}");
            if (frames.Count == 0
                && actorState.Handoff.TryCompleteTransferredActorReplay(handoffId))
                return;
            if (frames.Count == 0)
                continue;

            await _dispatcher.DispatchActorReplayFramesAsync(
                    ZLinkActorHandoffFrames.Restore(actorRef, frames),
                    actorState.Handoff.AcknowledgeReplayedFrame,
                    cancellationToken)
                .ConfigureAwait(false);
            _runtime.LogActorHandoff(
                $"handoff_final_replay_dispatched actor={actorState.ActorId} frames={frames.Count}");
        }
    }

    internal ValueTask ReplayAbortedActorHandoffAsync(
        ZLinkActorRuntimeState actorState,
        IReadOnlyList<ZLinkActorHandoffFrame> frames,
        CancellationToken cancellationToken)
    {
        if (frames.Count == 0) return ValueTask.CompletedTask;

        var actorRef = actorState.NativeActorRef
                       ?? throw new ZLinkFrameworkException(
                           ZLinkFrameworkErrorKind.NotFound,
                           $"Actor '{actorState.ActorId}' does not have a native Actor ref during handoff rollback.");
        return _dispatcher.DispatchActorFramesAsync(
            ZLinkActorHandoffFrames.Restore(actorRef, frames),
            _serial,
            cancellationToken);
    }

    internal ValueTask RestoreActorAfterFailedHandoffAsync(
        IZLinkActor actor,
        CancellationToken cancellationToken)
    {
        return ReferenceEquals(ZLinkSpotAmbientContext.CurrentOrDefault, this)
            ? RestoreActorAfterFailedHandoffCoreAsync(actor, cancellationToken)
            : ExecuteSerializedAsync(
                static (activation, state, ct) => activation.RestoreActorAfterFailedHandoffCoreAsync(state, ct),
                actor,
                cancellationToken);
    }

    private async ValueTask RestoreActorAfterFailedHandoffCoreAsync(
        IZLinkActor actor,
        CancellationToken cancellationToken)
    {
        _ = await StageActorMembershipAsync(actor, cancellationToken)
            .ConfigureAwait(false);
        await NotifyJoinedActorCoreAsync(actor, cancellationToken).ConfigureAwait(false);
        if (_runtime.LocationLifecycle is { } locations)
        {
            _ = locations.SpotLocations.TryGetTrackedGeneration(SpotId, out var spotGeneration);
            await locations.ActorOwnership.NotifyActorJoinedSpotAsync(
                    actor.Context.ActorId,
                    SpotId,
                    spotGeneration,
                    cancellationToken)
                .ConfigureAwait(false);
        }
    }

    public ValueTask NotifyActorDisconnectedAsync(
        IZLinkActor actor,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(actor);
        return ReferenceEquals(ZLinkSpotAmbientContext.CurrentOrDefault, this)
            ? NotifyActorDisconnectedCoreAsync(actor, cancellationToken)
            : ExecuteSerializedAsync(
            static (activation, state, ct) =>
                activation.NotifyActorDisconnectedCoreAsync(state, ct),
            actor,
            cancellationToken);
    }

    internal ValueTask NotifyActorLeftAfterNativeJoinEntrySpotAsync(
        IZLinkActor actor,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(actor);
        return ReferenceEquals(ZLinkSpotAmbientContext.CurrentOrDefault, this)
            ? NotifyActorLeftAfterNativeJoinEntrySpotCoreAsync(actor, cancellationToken)
            : ExecuteSerializedAsync(
                static (activation, state, ct) =>
                    activation.NotifyActorLeftAfterNativeJoinEntrySpotCoreAsync(
                        state,
                        ct),
                actor,
                cancellationToken);
    }

    internal ValueTask NotifyActorLeftAfterManagedJoinSpotAsync(
        IZLinkActor actor,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(actor);
        return ReferenceEquals(ZLinkSpotAmbientContext.CurrentOrDefault, this)
            ? NotifyActorLeftAfterJoinCommitCoreAsync(actor, cancellationToken)
            : ExecuteSerializedAsync(
                static (activation, state, ct) =>
                    activation.NotifyActorLeftAfterJoinCommitCoreAsync(
                        state,
                        ct),
                actor,
                cancellationToken);
    }

    internal ValueTask NotifyActorLeftAfterCommittedMembershipAsync(
        IZLinkActor actor,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(actor);
        if (ExecutionMode == ZLinkUserSpotExecutionMode.PerActor
            && PerActorShellRelocationPlan is not null)
            return _serial.ExecuteActorAsync(
                actor.Context.ActorId,
                static (activation, state, ct) =>
                    activation.NotifyActorLeftAfterCommittedMembershipCoreAsync(
                        state,
                        ct),
                actor,
                cancellationToken);
        return ReferenceEquals(ZLinkSpotAmbientContext.CurrentOrDefault, this)
            ? NotifyActorLeftAfterCommittedMembershipCoreAsync(actor, cancellationToken)
            : ExecuteSerializedAsync(
                static (activation, state, ct) =>
                    activation.NotifyActorLeftAfterCommittedMembershipCoreAsync(state, ct),
                actor,
                cancellationToken);
    }

    private async ValueTask CommitActorJoinCoreAsync(
        IZLinkActor actor,
        CancellationToken cancellationToken,
        DateTimeOffset? absoluteDeadline = null)
    {
        await _membershipPublicationGate.WaitAsync(cancellationToken)
            .ConfigureAwait(false);
        ZLinkSpotActivation? previousActivation;
        try
        {
            if (PerActorShellRelocationPlan is not null)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    $"SPOT '{SpotId}' no longer accepts Actor membership after relocation publication.",
                    ZLinkRetryAdvice.RetryAfterStateChange);
            previousActivation = await _runtime.CommitActorToSpotAsync(
                    this,
                    actor,
                    async ct =>
                    {
                        if (_runtime.LocationLifecycle is not { } locations)
                            return;
                        _ = locations.SpotLocations.TryGetTrackedGeneration(
                            SpotId,
                            out var spotGeneration);
                        await locations.ActorOwnership.NotifyActorJoinedSpotAsync(
                                actor.Context.ActorId,
                                SpotId,
                                spotGeneration,
                                ct)
                            .ConfigureAwait(false);
                        //  Same-node join commits the location authority just as
                        //  a cross-node handoff does, so it reports the commit on
                        //  the same channel. Using the debug console here left
                        //  the marker invisible to anything reading ILogger.
                        _runtime.LogActorHandoff(
                            $"location_committed actor={actor.Context.ActorId} spot={SpotId}");
                    },
                    () =>
                    {
                        _actorsLeavingForEntrySpot.Remove(actor.Context.ActorId);
                        _actors.Add(actor);
                    },
                    cancellationToken)
                .ConfigureAwait(false);
        }
        finally
        {
            _membershipPublicationGate.Release();
        }

        if (ReferenceEquals(previousActivation, this)) return;

        await RetryCommittedMembershipCallbackAsync(
                ct => NotifyJoinedActorCoreAsync(actor, ct),
                cancellationToken,
                absoluteDeadline)
            .ConfigureAwait(false);
        if (previousActivation is null)
            await RetryCommittedMembershipCallbackAsync(
                    ct => _runtime.NotifyEntrySpotActorLeftAsync(
                        actor,
                        NodeRid,
                        ct),
                    cancellationToken,
                    absoluteDeadline)
                .ConfigureAwait(false);
        else if (!ReferenceEquals(previousActivation, this)
                 && !previousActivation.IsDisposed)
            await RetryCommittedMembershipCallbackAsync(
                    ct => previousActivation
                        .NotifyActorLeftAfterCommittedMembershipAsync(actor, ct),
                    cancellationToken,
                    absoluteDeadline)
                .ConfigureAwait(false);
    }

    private static async ValueTask RetryCommittedMembershipCallbackAsync(
        Func<CancellationToken, ValueTask> callback,
        CancellationToken cancellationToken,
        DateTimeOffset? absoluteDeadline)
    {
        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (absoluteDeadline is { } deadline
                && deadline <= DateTimeOffset.UtcNow)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.DeadlineExceeded,
                    "Actor membership post-commit lifecycle did not finish before the deadline.",
                    ZLinkRetryAdvice.DoNotRetry);
            try
            {
                await callback(cancellationToken).ConfigureAwait(false);
                return;
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                throw;
            }
            catch
            {
                var delay = TimeSpan.FromMilliseconds(10);
                if (absoluteDeadline is { } deadlineValue)
                {
                    var remaining = deadlineValue - DateTimeOffset.UtcNow;
                    if (remaining <= TimeSpan.Zero)
                        throw new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.DeadlineExceeded,
                            "Actor membership post-commit lifecycle did not finish before the deadline.",
                            ZLinkRetryAdvice.DoNotRetry);
                    if (remaining < delay) delay = remaining;
                }
                await Task.Delay(delay, cancellationToken)
                    .ConfigureAwait(false);
            }
        }
    }

    private async ValueTask<ZLinkSpotActivation?> StageActorMembershipAsync(
        IZLinkActor actor,
        CancellationToken cancellationToken)
    {
        _actorsLeavingForEntrySpot.Remove(actor.Context.ActorId);
        _actors.Add(actor);
        return await _runtime.JoinActorToSpotAsync(this, actor, cancellationToken)
            .ConfigureAwait(false);
    }

    private async ValueTask NotifyJoinedActorCoreAsync(
        IZLinkActor actor,
        CancellationToken cancellationToken)
    {
        //  "자리에 왔다"만 남기면 해석 성공 여부를 알 수 없다. 결과와 대상까지
        //  함께 남긴다.
        var hasHandlers = _actorHandlers is not null;
        ZLinkSpotActorLifecycleDescriptor? descriptor = null;
        var resolved = hasHandlers
                       && _actorHandlers!.TryResolveJoined(
                           actor.GetType(), out descriptor)
                       && descriptor is not null;
        ZLinkFrameworkDebugLog.SpotDiscovery(
            "actor_joined_hook actor=" + actor.GetType().Name
            + " handlers=" + hasHandlers
            + " resolved=" + resolved);
        if (resolved)
            await HandlerInvoker.InvokeActorLifecycleAsync(descriptor!, actor, cancellationToken)
                .ConfigureAwait(false);
    }

    private async ValueTask LeaveActorCoreAsync(
        IZLinkActor actor,
        CancellationToken cancellationToken)
    {
        _actorsLeavingForEntrySpot.Add(actor.Context.ActorId);
        try
        {
            await _runtime.JoinActorEntrySpotAsync(NodeRid, actor, ZLinkMessage.Empty, cancellationToken)
                .ConfigureAwait(false);
        }
        catch
        {
            _actorsLeavingForEntrySpot.Remove(actor.Context.ActorId);
            throw;
        }
    }

    private async ValueTask NotifyActorLeftAfterNativeJoinEntrySpotCoreAsync(
        IZLinkActor actor,
        CancellationToken cancellationToken)
    {
        await NotifyActorLeftAfterJoinCommitCoreAsync(actor, cancellationToken)
            .ConfigureAwait(false);
    }

    private async ValueTask NotifyActorLeftAfterJoinCommitCoreAsync(
        IZLinkActor actor,
        CancellationToken cancellationToken)
    {
        _actors.RemoveIfCurrent(actor);
        var actorState = _runtime.GetOrCreateActorState(actor.Context.ActorId);
        actorState.LeaveSpotIfCurrent(this);
        SignalPerActorMembersDrainedIfNeeded();

        if (_runtime.LocationLifecycle is { } locations)
        {
            var entrySpot = _runtime.GetMeshNodeRuntime(SpotNodeName)
                                .EntrySpotActivation
                            ?? throw new ZLinkFrameworkException(
                                ZLinkFrameworkErrorKind.NotFound,
                                $"MeshNode '{SpotNodeName}' does not have an Entry Spot activation.");
            await locations.ActorOwnership.NotifyActorLeftSpotAsync(
                    actor.Context.ActorId,
                    entrySpot.SpotId,
                    entrySpot.ObjectGeneration,
                    cancellationToken)
                .ConfigureAwait(false);
        }

        if (_actorHandlers is not null
            && _actorHandlers.TryResolveLeft(actor.GetType(), out var descriptor)
            && descriptor is not null)
            await HandlerInvoker.InvokeActorLifecycleAsync(descriptor, actor, cancellationToken)
                .ConfigureAwait(false);
    }

    private void SignalPerActorMembersDrainedIfNeeded()
    {
        if (PerActorShellRelocationPlan is not null
            && JoinedActorCount == 0)
            _perActorMembersDrained.TrySetResult();
    }

    private async ValueTask NotifyActorLeftAfterCommittedMembershipCoreAsync(
        IZLinkActor actor,
        CancellationToken cancellationToken)
    {
        _actors.RemoveIfCurrent(actor);
        var actorState = _runtime.GetOrCreateActorState(actor.Context.ActorId);
        actorState.LeaveSpotIfCurrent(this);
        SignalPerActorMembersDrainedIfNeeded();

        if (_actorHandlers is not null
            && _actorHandlers.TryResolveLeft(actor.GetType(), out var descriptor)
            && descriptor is not null)
            await HandlerInvoker.InvokeActorLifecycleAsync(descriptor, actor, cancellationToken)
                .ConfigureAwait(false);
    }

    private async ValueTask NotifyActorDisconnectedCoreAsync(
        IZLinkActor actor,
        CancellationToken cancellationToken)
    {
        if (_actorsLeavingForEntrySpot.Remove(actor.Context.ActorId)) return;

        if (_actorHandlers is not null
            && _actorHandlers.TryResolveDisconnected(actor.GetType(), out var descriptor)
            && descriptor is not null)
            await HandlerInvoker.InvokeActorLifecycleAsync(descriptor, actor, cancellationToken)
                .ConfigureAwait(false);
    }

    private async ValueTask<ZLinkSpotActorJoinResult> InvokeActorJoinAsync(
        ZLinkSpotActorJoinDescriptor descriptor,
        IZLinkActor actor,
        ZLinkMessage request,
        CancellationToken cancellationToken)
    {
        return await HandlerInvoker.InvokeActorJoinAsync(descriptor, actor.Context.ActorId, request, cancellationToken)
            .ConfigureAwait(false);
    }

    private sealed class ActorJoinCallState(
        IZLinkActor actor,
        ZLinkMessage request,
        ZLinkSpotActorJoinDescriptor descriptor,
        DateTimeOffset? absoluteDeadline)
    {
        public IZLinkActor Actor { get; } = actor;

        public ZLinkMessage Request { get; } = request;

        public ZLinkSpotActorJoinDescriptor Descriptor { get; } = descriptor;

        public DateTimeOffset? AbsoluteDeadline { get; } = absoluteDeadline;

        public ZLinkSpotActorJoinResult Result { get; set; }
    }

    private sealed class ActorJoinAdmissionCallState(
        string actorId,
        ZLinkMessage request,
        ZLinkSpotActorJoinDescriptor descriptor)
    {
        public string ActorId { get; } = actorId;

        public ZLinkMessage Request { get; } = request;

        public ZLinkSpotActorJoinDescriptor Descriptor { get; } = descriptor;

        public ZLinkSpotActorJoinResult Result { get; set; }
    }
}
