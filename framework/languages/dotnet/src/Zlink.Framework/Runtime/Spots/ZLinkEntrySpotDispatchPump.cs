using System.Diagnostics;

namespace Zlink.Framework.Runtime.Spots;

internal sealed class ZLinkEntrySpotDispatchPump : IAsyncDisposable
{
    private readonly ZLinkFrameworkRuntime _runtime;
    private readonly ZLinkEntrySpotActivation? _activation;
    private readonly ZLinkRuntimeTaskRunner _taskRunner;
    private readonly System.Collections.Concurrent.ConcurrentDictionary<
        string,
        ActorLane> _actorLanes;
    private readonly ZLinkActorInboundPipeline _actorPipeline;
    private int _stopping;
    private IZLinkBackendSpot? _entrySpot;

    public ZLinkEntrySpotDispatchPump(
        ZLinkFrameworkRuntime runtime,
        ZLinkEntrySpotActivation? activation,
        ZLinkRuntimeTaskRunner taskRunner)
    {
        _runtime = runtime;
        _activation = activation;
        _taskRunner = taskRunner;
        _actorLanes = new(StringComparer.Ordinal);
        _actorPipeline = new(
            runtime,
            new ZLinkEntrySpotActorInboundEndpoint(runtime));
    }

    public void Attach(IZLinkBackendSpot entrySpot)
    {
        _entrySpot = entrySpot;
        var previous = SynchronizationContext.Current;
        SynchronizationContext.SetSynchronizationContext(null);
        try
        {
            entrySpot.OnDispatchEvent(OnDispatchEvent);
        }
        finally
        {
            SynchronizationContext.SetSynchronizationContext(previous);
        }
    }

    public void RequestStop() => Interlocked.Exchange(ref _stopping, 1);

    public async ValueTask DisposeAsync()
    {
        RequestStop();
        foreach (var lane in _actorLanes.Values)
            await lane.DisposeAsync().ConfigureAwait(false);
        _actorLanes.Clear();
    }

    private void OnDispatchEvent(ZLinkBackendSpotDispatchInfo info)
    {
        if (_activation is not null)
            switch (info.Event)
            {
                case ZLinkBackendSpotDispatchEvent.RouteReadable:
                    if (info.RoutedMessages is { Count: > 0 } routedMessages)
                    {
                        foreach (var received in routedMessages)
                        {
                            if (ZLinkSpotActivationDispatcher.IsInfrastructureRoute(received))
                            {
                                if (!_taskRunner.TryRunDetached(
                                        "entry-spot-route-dispatch",
                                        ct => _activation.DispatchRouteAsync(received, ct)))
                                    received.Dispose();
                                continue;
                            }

                            if (!_runtime.TryEnterInboundOperation(received.CanReply, out var operation))
                            {
                                ZLinkSpotActivationDispatcher.RejectApplicationRouteForDrain(
                                    received,
                                    _activation.ChannelName,
                                    ZLinkAcceptedWorkAdmission.Closed,
                                    received.SourceNodeRid is null
                                    || received.SourceNodeRid == _activation.NodeRid,
                                    _runtime.Flow.CaptureEnabled);
                                continue;
                            }

                            if (!_taskRunner.TryRunDetached(
                                    "entry-spot-route-dispatch",
                                    async ct =>
                                    {
                                        using (operation)
                                            await _activation.DispatchRouteAsync(received, ct)
                                                .ConfigureAwait(false);
                                    }))
                            {
                                operation.Dispose();
                                received.Dispose();
                            }
                        }
                    }
                    else
                    {
                        _taskRunner.RunDetached(
                            "entry-spot-route-dispatch",
                            ct => _activation.DispatchRouteDrainAsync(ct));
                    }

                    return;
                case ZLinkBackendSpotDispatchEvent.ChannelReplyReadable:
                    info.DrainChannelReply?.Invoke();
                    return;
                case ZLinkBackendSpotDispatchEvent.SubscribeReadable:
                    if (!_runtime.TryEnterInboundOperation(
                            countAsRequest: false,
                            out var subscriptionOperation))
                    {
                        _taskRunner.RunDetached(
                            "entry-spot-subscription-discard",
                            ct => _activation.DiscardSubscriptionsAsync(ct));
                        return;
                    }
                    _taskRunner.RunDetached(
                        "entry-spot-subscription-dispatch",
                        async ct =>
                        {
                            using (subscriptionOperation)
                                await _activation.DispatchSubscriptionsAsync(ct).ConfigureAwait(false);
                        });
                    return;
                case ZLinkBackendSpotDispatchEvent.ActorJoinReadable:
                    _taskRunner.RunDetached(
                        "entry-spot-actor-join-dispatch",
                        ct => _activation.DispatchActorJoinDrainAsync(ct));
                    return;
                case ZLinkBackendSpotDispatchEvent.ActorLifecycleReadable:
                    _taskRunner.RunDetached(
                        "entry-spot-actor-lifecycle-dispatch",
                        DispatchActorLifecycleDrainAsync);
                    return;
            }

        if (info.Event != ZLinkBackendSpotDispatchEvent.ActorReadable
            || info.ActorParts is not { Count: > 0 } actorParts)
            return;

        var dispatchable = ZLinkActorHandoffIngress.CaptureMovingFrames(
            _runtime,
            actorParts,
            info.ActorCreditOwner);
        if (dispatchable.Count == 0)
        {
            dispatchable.Dispose();
            return;
        }

        if (!_runtime.TryEnterInboundOperation(countAsRequest: false, out var actorOperation))
        {
            dispatchable.Dispose();
            return;
        }

        EnqueueActorBatch(dispatchable.WithCompletion(actorOperation.Dispose));
    }

    private void EnqueueActorBatch(ZLinkSpotActorFrameBatch frames)
    {
        if (Volatile.Read(ref _stopping) != 0)
        {
            RejectActorBatch(frames, ZLinkFrameworkErrorKind.ShuttingDown);
            return;
        }

        var actorId = frames.Count > 0
            ? frames[0].Actor.ActorId
            : string.Empty;
        if (string.IsNullOrWhiteSpace(actorId))
        {
            frames.Dispose();
            return;
        }

        while (true)
        {
            var lane = _actorLanes.GetOrAdd(
                actorId,
                static (id, pump) => new ActorLane(
                    pump,
                    id),
                this);
            var admission = lane.TryEnqueue(frames);
            if (admission == ZLinkSerialPostAdmission.Accepted)
                return;
            continue;
        }
    }

    private void RejectActorBatch(
        ZLinkSpotActorFrameBatch frames,
        ZLinkFrameworkErrorKind errorKind)
    {
        var error = new ZLinkFrameworkException(
            errorKind,
            "Entry Actor dispatch is shutting down.");
        if (!_taskRunner.TryRunDetached(
                "entry-spot-actor-reject",
                cancellationToken => _actorPipeline.RejectAsync(
                    frames,
                    error,
                    cancellationToken)))
            frames.Dispose();
    }

    private async Task ObserveActorDispatchAsync(ValueTask dispatch)
    {
        try
        {
            await dispatch.ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (_runtime.ShutdownToken.IsCancellationRequested)
        {
        }
        catch (Exception exception)
        {
            _runtime.ErrorSink.ReportRuntimeTaskException("entry-spot-actor-dispatch", exception);
        }
    }

    private sealed class ActorLane(
        ZLinkEntrySpotDispatchPump owner,
        string actorId) : IAsyncDisposable
    {
        private readonly object _lifecycleGate = new();
        private readonly ZLinkSerialExecutionQueue _queue = new(
            owner._taskRunner,
            owner._runtime.ErrorSink,
            owner._runtime.ShutdownToken);
        private bool _retired;
        private bool _retirementScheduled;

        internal ZLinkSerialPostAdmission TryEnqueue(
            ZLinkSpotActorFrameBatch frames)
        {
            lock (_lifecycleGate)
            {
                if (_retired) return ZLinkSerialPostAdmission.Closed;
                var admission = _queue.TryPostApplicationWithAdmission(
                    async cancellationToken =>
                    {
                        await owner.ObserveActorDispatchAsync(
                                owner._actorPipeline.DispatchAsync(
                                    frames,
                                    cancellationToken))
                            .ConfigureAwait(false);
                    },
                    out _);
                if (admission == ZLinkSerialPostAdmission.Accepted
                    && !_retirementScheduled)
                {
                    _retirementScheduled = true;
                    _ = RetireWhenDrainedAsync();
                }
                return admission;
            }
        }

        private async Task RetireWhenDrainedAsync()
        {
            while (true)
            {
                await _queue.ApplicationDrained.ConfigureAwait(false);
                lock (_lifecycleGate)
                {
                    if (_retired) return;
                    if (_queue.ApplicationPendingCount != 0)
                        continue;
                    _retired = true;
                    owner._actorLanes.TryRemove(
                        new KeyValuePair<string, ActorLane>(actorId, this));
                    break;
                }
            }
            await _queue.DisposeAsync().ConfigureAwait(false);
        }

        public async ValueTask DisposeAsync()
        {
            lock (_lifecycleGate) _retired = true;
            await _queue.DisposeAsync().ConfigureAwait(false);
        }
    }

    private async ValueTask DispatchActorLifecycleDrainAsync(CancellationToken cancellationToken)
    {
        if (_entrySpot is not { } entrySpot) return;

        var startedAt = Stopwatch.GetTimestamp();
        var count = 0;
        long bytes = 0;
        while (true)
        {
            if (ZLinkReceiveBatchBudget.IsExhausted(count, bytes, startedAt))
            {
                if (!cancellationToken.IsCancellationRequested)
                    _taskRunner.RunDetached(
                        "entry-spot-actor-lifecycle-dispatch",
                        DispatchActorLifecycleDrainAsync);
                return;
            }
            var lifecycle = entrySpot.RecvActorLifecycle(RecvFlags.DontWait);
            if (lifecycle is null) return;
            using var applicationAdmission =
                lifecycle.Value.ApplicationJobAdmission is { } admission
                    ? ZLinkApplicationJobQueueInvocation.Enter(admission)
                    : null;
            count++;
            bytes = checked(
                bytes + (lifecycle.Value.Info.CurrentActor?.ActorId?.Length ?? 0));
            if (lifecycle.Value.Kind != ZLinkBackendActorLifecycleEventKind.Disconnected)
                continue;

            var actorId = lifecycle.Value.Info.CurrentActor?.ActorId;
            if (actorId is null) continue;

            if (!await _runtime.TryNotifyJoinedSpotActorDisconnectedAsync(actorId, cancellationToken)
                    .ConfigureAwait(false))
                await _runtime.NotifyActorDisconnectedByIdAsync(actorId, cancellationToken)
                    .ConfigureAwait(false);
        }
    }
}
