using System.Diagnostics;

namespace Zlink.Framework.Runtime.Spots;

internal sealed class ZLinkEntrySpotDispatchPump : IAsyncDisposable
{
    private readonly ZLinkFrameworkRuntime _runtime;
    private readonly ZLinkEntrySpotActivation? _activation;
    private readonly ZLinkRuntimeTaskRunner _taskRunner;
    private readonly ZLinkActorInboundPipeline _actorPipeline;
    private int _stopping;
    private IZLinkBackendSpot? _entrySpot;

    public ZLinkEntrySpotDispatchPump(
        ZLinkFrameworkRuntime runtime,
        ZLinkEntrySpotActivation? activation,
        ZLinkRuntimeTaskRunner taskRunner
    )
    {
        _runtime = runtime;
        _activation = activation;
        _taskRunner = taskRunner;
        _actorPipeline = new(runtime, new ZLinkEntrySpotActorInboundEndpoint(runtime));
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

    public ValueTask DisposeAsync()
    {
        RequestStop();
        return ValueTask.CompletedTask;
    }

    private (ValueTask Completion, Func<CancellationToken, ValueTask>? Drain) OnDispatchEvent(
        ZLinkBackendSpotDispatchInfo info
    )
    {
        if (_activation is not null)
            switch (info.Event)
            {
                case ZLinkBackendSpotDispatchEvent.RouteReadable:
                    if (info.RoutedMessages is { Count: > 0 } routedMessages)
                        return DispatchRoutes(routedMessages);
                    var completion = _activation.DispatchRouteDrainAsync(
                        _taskRunner.ShutdownToken,
                        out var drain
                    );
                    return (completion, drain);
                case ZLinkBackendSpotDispatchEvent.ChannelReplyReadable:
                    info.DrainChannelReply?.Invoke();
                    return (ValueTask.CompletedTask, null);
                case ZLinkBackendSpotDispatchEvent.SubscribeReadable:
                    var subscriptionAdmission = _runtime.TryEnterInboundOperation(
                        countAsRequest: false
                    );
                    if (!subscriptionAdmission.Accepted)
                    {
                        _taskRunner.RunDetached(
                            "entry-spot-subscription-discard",
                            ct => _activation.DiscardSubscriptionsAsync(ct)
                        );
                        return (ValueTask.CompletedTask, null);
                    }
                    _taskRunner.RunDetached(
                        "entry-spot-subscription-dispatch",
                        async ct =>
                        {
                            using (subscriptionAdmission.Lease)
                                await _activation
                                    .DispatchSubscriptionsAsync(ct)
                                    .ConfigureAwait(false);
                        }
                    );
                    return (ValueTask.CompletedTask, null);
                case ZLinkBackendSpotDispatchEvent.ActorJoinReadable:
                    _taskRunner.RunDetached(
                        "entry-spot-actor-join-dispatch",
                        ct => _activation.DispatchActorJoinDrainAsync(ct)
                    );
                    return (ValueTask.CompletedTask, null);
                case ZLinkBackendSpotDispatchEvent.ActorLifecycleReadable:
                    _taskRunner.RunDetached(
                        "entry-spot-actor-lifecycle-dispatch",
                        DispatchActorLifecycleDrainAsync
                    );
                    return (ValueTask.CompletedTask, null);
            }

        if (
            info.Event != ZLinkBackendSpotDispatchEvent.ActorReadable
            || info.ActorParts is not { Count: > 0 } actorParts
        )
            return (ValueTask.CompletedTask, null);

        var dispatchable = ZLinkActorHandoffIngress.CaptureMovingFrames(
            info.ApplicationJobOrigin!.Value,
            _runtime,
            actorParts,
            info.ActorPayloadOwner
        );
        if (dispatchable.Count == 0)
        {
            dispatchable.Dispose();
            return (ValueTask.CompletedTask, null);
        }

        EnqueueActorBatch(dispatchable);
        return (ValueTask.CompletedTask, null);
    }

    private (ValueTask Completion, Func<CancellationToken, ValueTask>? Drain) DispatchRoutes(
        IReadOnlyList<ZLinkBackendRouteReceived> routedMessages
    )
    {
        List<Task>? pending = null;
        Func<CancellationToken, ValueTask>? drain = null;
        foreach (var received in routedMessages)
        {
            ValueTask completion;
            try
            {
                completion = DispatchRoute(received, out var reservedDrain);
                drain ??= reservedDrain;
            }
            catch (Exception exception)
            {
                completion = ValueTask.FromException(exception);
            }
            if (completion.IsCompletedSuccessfully)
                completion.GetAwaiter().GetResult();
            else
                (pending ??= []).Add(completion.AsTask());
        }
        return (
            pending is null ? ValueTask.CompletedTask : new ValueTask(Task.WhenAll(pending)),
            drain
        );
    }

    private ValueTask DispatchRoute(
        ZLinkBackendRouteReceived received,
        out Func<CancellationToken, ValueTask>? drain
    )
    {
        drain = null;
        if (ZLinkSpotActivationDispatcher.IsInfrastructureRoute(received))
            return _activation!.DispatchRouteAsync(received, _taskRunner.ShutdownToken, out drain);

        var admission = _runtime.TryEnterInboundOperation(received.CanReply);
        if (!admission.Accepted)
        {
            ZLinkSpotActivationDispatcher.RejectApplicationRouteForDrain(
                received,
                _activation!.ChannelName,
                ZLinkAcceptedWorkAdmission.Closed,
                _runtime
            );
            return ValueTask.CompletedTask;
        }

        try
        {
            return CompleteRouteAsync(
                _activation!.DispatchRouteAsync(received, _taskRunner.ShutdownToken, out drain),
                admission.Lease
            );
        }
        catch
        {
            admission.Lease.Dispose();
            throw;
        }
    }

    private static async ValueTask CompleteRouteAsync(
        ValueTask completion,
        IDisposable inboundAdmission
    )
    {
        using (inboundAdmission)
            await completion.ConfigureAwait(false);
    }

    private void EnqueueActorBatch(ZLinkSpotActorFrameBatch frames)
    {
        if (Volatile.Read(ref _stopping) != 0)
        {
            RejectActorBatch(frames, ZLinkFrameworkErrorKind.ShuttingDown);
            return;
        }

        var actorId = frames.Count > 0 ? frames[0].Actor.ActorId : string.Empty;
        if (string.IsNullOrWhiteSpace(actorId))
        {
            frames.Dispose();
            return;
        }

        _ = ObserveActorDispatchAsync(DispatchActorBatchAsync(frames));
    }

    private async ValueTask DispatchActorBatchAsync(ZLinkSpotActorFrameBatch frames)
    {
        var admission = _runtime.TryEnterInboundOperation(countAsRequest: false);
        if (!admission.Accepted)
        {
            frames.Dispose();
            return;
        }
        using (admission.Lease)
            await _actorPipeline
                .DispatchAsync(frames, _taskRunner.ShutdownToken)
                .ConfigureAwait(false);
    }

    private void RejectActorBatch(
        ZLinkSpotActorFrameBatch frames,
        ZLinkFrameworkErrorKind errorKind
    )
    {
        var error = new ZLinkFrameworkException(
            errorKind,
            "Entry Actor dispatch is shutting down."
        );
        if (
            !_taskRunner.TryRunDetached(
                "entry-spot-actor-reject",
                cancellationToken => _actorPipeline.RejectAsync(frames, error, cancellationToken)
            )
        )
            frames.Dispose();
    }

    private async Task ObserveActorDispatchAsync(ValueTask dispatch)
    {
        try
        {
            await dispatch.ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (_runtime.ShutdownToken.IsCancellationRequested) { }
        catch (Exception exception)
        {
            _runtime.ErrorSink.ReportRuntimeTaskException("entry-spot-actor-dispatch", exception);
        }
    }

    private async ValueTask DispatchActorLifecycleDrainAsync(CancellationToken cancellationToken)
    {
        if (_entrySpot is not { } entrySpot)
            return;

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
                        DispatchActorLifecycleDrainAsync
                    );
                return;
            }
            var lifecycle = entrySpot.RecvActorLifecycle(RecvFlags.DontWait);
            if (lifecycle is null)
                return;
            using var applicationAdmission = lifecycle.Value.ApplicationJobAdmission
                is { } admission
                ? ZLinkApplicationJobQueueInvocation.Enter(admission)
                : null;
            count++;
            bytes = checked(bytes + (lifecycle.Value.Info.CurrentActor?.ActorId?.Length ?? 0));
            if (lifecycle.Value.Kind != ZLinkBackendActorLifecycleEventKind.Disconnected)
                continue;

            var actorId = lifecycle.Value.Info.CurrentActor?.ActorId;
            if (actorId is null)
                continue;

            if (
                !await _runtime
                    .TryNotifyJoinedSpotActorDisconnectedAsync(actorId, cancellationToken)
                    .ConfigureAwait(false)
            )
                await _runtime
                    .NotifyActorDisconnectedByIdAsync(actorId, cancellationToken)
                    .ConfigureAwait(false);
        }
    }
}
