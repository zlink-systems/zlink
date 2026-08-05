using System.Diagnostics;
using System.Threading.Channels;

namespace Zlink.Framework.Runtime.Spots;

internal sealed class ZLinkEntrySpotDispatchPump(
    ZLinkFrameworkRuntime runtime,
    ZLinkEntrySpotActivation? activation,
    ZLinkRuntimeTaskRunner taskRunner) : IAsyncDisposable
{
    private readonly object _activeDispatchGate = new();
    private readonly object _actorChainGate = new();
    private readonly Dictionary<string, Task> _actorChains = new(StringComparer.Ordinal);
    private readonly HashSet<Task> _activeDispatches = [];
    private readonly ZLinkActorInboundPipeline _actorPipeline = new(
        runtime,
        new ZLinkEntrySpotActorInboundEndpoint(runtime));
    private readonly Channel<ZLinkSpotActorFrameBatch> _actorDispatch =
        Channel.CreateUnbounded<ZLinkSpotActorFrameBatch>(new UnboundedChannelOptions
        {
            SingleReader = true,
            SingleWriter = false,
            AllowSynchronousContinuations = false
        });
    private Task? _actorDispatchStarter;
    private IZLinkBackendSpot? _entrySpot;

    public void Attach(IZLinkBackendSpot entrySpot)
    {
        _actorDispatchStarter ??= taskRunner.Run(
            "entry-spot-actor-ingress",
            StartActorDispatchesAsync);
        if (_actorDispatchStarter.IsCompleted) RequestStop();
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

    public void RequestStop() => _actorDispatch.Writer.TryComplete();

    public async ValueTask DisposeAsync()
    {
        _actorDispatch.Writer.TryComplete();
        if (_actorDispatchStarter is not null)
            await _actorDispatchStarter.ConfigureAwait(false);
        while (_actorDispatch.Reader.TryRead(out var pending)) pending.Dispose();
        while (true)
        {
            Task[] active;
            lock (_activeDispatchGate)
            {
                _activeDispatches.RemoveWhere(static task => task.IsCompleted);
                if (_activeDispatches.Count == 0) return;
                active = _activeDispatches.ToArray();
            }
            await Task.WhenAll(active).ConfigureAwait(false);
        }
    }

    private void OnDispatchEvent(ZLinkBackendSpotDispatchInfo info)
    {
        if (activation is not null)
            switch (info.Event)
            {
                case ZLinkBackendSpotDispatchEvent.RouteReadable:
                    if (info.RoutedMessages is { Count: > 0 } routedMessages)
                    {
                        foreach (var received in routedMessages)
                        {
                            if (ZLinkSpotActivationDispatcher.IsInfrastructureRoute(received))
                            {
                                if (!taskRunner.TryRunDetached(
                                        "entry-spot-route-dispatch",
                                        ct => activation.DispatchRouteAsync(received, ct)))
                                    received.Dispose();
                                continue;
                            }

                            if (!runtime.TryEnterInboundOperation(received.CanReply, out var operation))
                            {
                                ZLinkSpotActivationDispatcher.RejectApplicationRouteForDrain(
                                    received,
                                    activation.ChannelName,
                                    ZLinkAcceptedWorkAdmission.Closed,
                                    received.SourceNodeRid is null
                                    || received.SourceNodeRid == activation.NodeRid);
                                continue;
                            }

                            if (!taskRunner.TryRunDetached(
                                    "entry-spot-route-dispatch",
                                    async ct =>
                                    {
                                        using (operation)
                                            await activation.DispatchRouteAsync(received, ct)
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
                        taskRunner.RunDetached(
                            "entry-spot-route-dispatch",
                            ct => activation.DispatchRouteDrainAsync(ct));
                    }

                    return;
                case ZLinkBackendSpotDispatchEvent.ChannelReplyReadable:
                    info.DrainChannelReply?.Invoke();
                    return;
                case ZLinkBackendSpotDispatchEvent.SubscribeReadable:
                    if (!runtime.TryEnterInboundOperation(
                            countAsRequest: false,
                            out var subscriptionOperation))
                    {
                        taskRunner.RunDetached(
                            "entry-spot-subscription-discard",
                            ct => activation.DiscardSubscriptionsAsync(ct));
                        return;
                    }
                    taskRunner.RunDetached(
                        "entry-spot-subscription-dispatch",
                        async ct =>
                        {
                            using (subscriptionOperation)
                                await activation.DispatchSubscriptionsAsync(ct).ConfigureAwait(false);
                        });
                    return;
                case ZLinkBackendSpotDispatchEvent.ActorJoinReadable:
                    taskRunner.RunDetached(
                        "entry-spot-actor-join-dispatch",
                        ct => activation.DispatchActorJoinDrainAsync(ct));
                    return;
                case ZLinkBackendSpotDispatchEvent.ActorLifecycleReadable:
                    taskRunner.RunDetached(
                        "entry-spot-actor-lifecycle-dispatch",
                        DispatchActorLifecycleDrainAsync);
                    return;
            }

        if (info.Event != ZLinkBackendSpotDispatchEvent.ActorReadable
            || info.ActorParts is not { Count: > 0 } actorParts)
            return;

        var dispatchable = ZLinkActorHandoffIngress.CaptureMovingFrames(
            runtime,
            actorParts,
            info.ActorDispatchLease);
        if (dispatchable.Count == 0)
        {
            dispatchable.Dispose();
            return;
        }

        if (!runtime.TryEnterInboundOperation(countAsRequest: false, out var actorOperation))
        {
            dispatchable.Dispose();
            return;
        }

        if (!_actorDispatch.Writer.TryWrite(dispatchable.WithCompletion(actorOperation.Dispose)))
        {
            actorOperation.Dispose();
            dispatchable.Dispose();
        }
    }

    private async ValueTask StartActorDispatchesAsync(CancellationToken cancellationToken)
    {
        await foreach (var frames in _actorDispatch.Reader.ReadAllAsync(cancellationToken)
                           .ConfigureAwait(false))
            StartActorDispatch(frames, cancellationToken);
    }

    private void StartActorDispatch(
        ZLinkSpotActorFrameBatch frames,
        CancellationToken cancellationToken)
    {
        // Per-actor FIFO: sibling batches for the same actor must not race —
        // a later batch overtaking an earlier one breaks the session order
        // and the handoff backlog's arrival sequence (spec 23 §10.2). Chains
        // are keyed per actor so one busy actor never stalls the others.
        var actorId = frames.Count > 0 ? frames[0].Actor.ActorId : string.Empty;
        Task observed;
        lock (_actorChainGate)
        {
            var prior = _actorChains.TryGetValue(actorId, out var chain)
                ? chain
                : Task.CompletedTask;
            observed = DispatchActorBatchAfterAsync(prior, frames, cancellationToken);
            _actorChains[actorId] = observed;
        }

        _ = observed.ContinueWith(
            (completed, state) =>
            {
                var pump = (ZLinkEntrySpotDispatchPump)state!;
                lock (pump._actorChainGate)
                {
                    if (pump._actorChains.TryGetValue(actorId, out var current)
                        && ReferenceEquals(current, completed))
                        pump._actorChains.Remove(actorId);
                }
            },
            this,
            CancellationToken.None,
            TaskContinuationOptions.ExecuteSynchronously,
            TaskScheduler.Default);
        lock (_activeDispatchGate) _activeDispatches.Add(observed);
        _ = observed.ContinueWith(
            static (completed, state) =>
            {
                var pump = (ZLinkEntrySpotDispatchPump)state!;
                lock (pump._activeDispatchGate) pump._activeDispatches.Remove(completed);
            },
            this,
            CancellationToken.None,
            TaskContinuationOptions.ExecuteSynchronously,
            TaskScheduler.Default);
    }

    private async Task DispatchActorBatchAfterAsync(
        Task prior,
        ZLinkSpotActorFrameBatch frames,
        CancellationToken cancellationToken)
    {
        try
        {
            await prior.ConfigureAwait(false);
        }
        catch
        {
            // The prior batch reported its own failure; the chain continues.
        }

        await ObserveActorDispatchAsync(
                _actorPipeline.DispatchAsync(frames, cancellationToken))
            .ConfigureAwait(false);
    }

    private async Task ObserveActorDispatchAsync(ValueTask dispatch)
    {
        try
        {
            await dispatch.ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (runtime.ShutdownToken.IsCancellationRequested)
        {
        }
        catch (Exception exception)
        {
            runtime.ErrorSink.ReportRuntimeTaskException("entry-spot-actor-dispatch", exception);
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
                    taskRunner.RunDetached(
                        "entry-spot-actor-lifecycle-dispatch",
                        DispatchActorLifecycleDrainAsync);
                return;
            }
            var lifecycle = entrySpot.RecvActorLifecycle(RecvFlags.DontWait);
            if (lifecycle is null) return;
            count++;
            bytes = checked(
                bytes + (lifecycle.Value.Info.CurrentActor?.ActorId?.Length ?? 0));
            if (lifecycle.Value.Kind != ZLinkBackendActorLifecycleEventKind.Disconnected)
                continue;

            var actorId = lifecycle.Value.Info.CurrentActor?.ActorId;
            if (actorId is null) continue;

            if (!await runtime.TryNotifyJoinedSpotActorDisconnectedAsync(actorId, cancellationToken)
                    .ConfigureAwait(false))
                await runtime.NotifyActorDisconnectedByIdAsync(actorId, cancellationToken)
                    .ConfigureAwait(false);
        }
    }
}
