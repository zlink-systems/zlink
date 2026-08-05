namespace Zlink.Framework.Runtime.Spots;

internal sealed partial class ZLinkEntrySpotActivation
{
    public ValueTask<IZLinkTimer> AddTimer<THandler>(
        string name,
        TimeSpan period,
        ZLinkTimerOptions? options = null,
        CancellationToken cancellationToken = default)
        where THandler : class
    {
        return _timers.AddAsync(
            name,
            period,
            options,
            typeof(THandler),
            EntrySpot.GetType(),
            _stopSource.Token,
            async (descriptor, tick, ct) =>
            {
                await ExecuteQueuedAsync(
                        static (activation, state, innerCt) => activation._invoker.InvokeTimerAsync(
                            state.Descriptor,
                            state.Tick,
                            innerCt),
                        (Descriptor: descriptor, Tick: tick),
                        ct)
                    .ConfigureAwait(false);
                return true;
            },
            PublishTimerFailureAsync,
            cancellationToken);
    }

    /// <summary>
    ///     Runs Entry Spot-owned callbacks on the Entry Spot serial execution
    ///     line. Lifecycle, route packets, subscriptions, timers, and worker
    ///     completions go through this line. Entry Spot actor packets are
    ///     serialized by the target actor mailbox instead.
    /// </summary>
    private async ValueTask ExecuteAsync(
        Func<ZLinkEntrySpotActivation, CancellationToken, ValueTask> operation,
        CancellationToken cancellationToken)
    {
        if (ReferenceEquals(Current.Value, this))
        {
            using var _ = ZLinkSpotAmbientContext.Push(this);
            await operation(this, cancellationToken).ConfigureAwait(false);
            return;
        }

        await _serial.RunAsync(
                ct => RunOnLineAsync(operation, ct),
                cancellationToken)
            .ConfigureAwait(false);
    }

    private async ValueTask ExecuteAsync<TState>(
        Func<ZLinkEntrySpotActivation, TState, CancellationToken, ValueTask> operation,
        TState state,
        CancellationToken cancellationToken)
    {
        if (ReferenceEquals(Current.Value, this))
        {
            using var _ = ZLinkSpotAmbientContext.Push(this);
            await operation(this, state, cancellationToken).ConfigureAwait(false);
            return;
        }

        var capturedOperation = operation;
        var capturedState = state;
        await _serial.RunAsync(
                ct => RunOnLineAsync(
                    (activation, innerCt) => capturedOperation(activation, capturedState, innerCt),
                    ct),
                cancellationToken)
            .ConfigureAwait(false);
    }

    private async ValueTask ExecuteQueuedAsync<TState>(
        Func<ZLinkEntrySpotActivation, TState, CancellationToken, ValueTask> operation,
        TState state,
        CancellationToken cancellationToken)
    {
        var capturedOperation = operation;
        var capturedState = state;
        await _serial.RunAsync(
                ct => RunOnLineAsync(
                    (activation, innerCt) => capturedOperation(activation, capturedState, innerCt),
                    ct),
            cancellationToken)
            .ConfigureAwait(false);
    }

    private async ValueTask ExecuteLifecycleAsync(
        Func<ZLinkEntrySpotActivation, CancellationToken, ValueTask> operation,
        CancellationToken cancellationToken)
    {
        if (ReferenceEquals(Current.Value, this))
        {
            using var _ = ZLinkSpotAmbientContext.Push(this);
            await operation(this, cancellationToken).ConfigureAwait(false);
            return;
        }

        await _serial.RunLifecycleAsync(
                ct => RunOnLineAsync(operation, ct),
                cancellationToken)
            .ConfigureAwait(false);
    }

    private async ValueTask ExecuteActorPacketAsync<TState>(
        Func<ZLinkEntrySpotActivation, TState, CancellationToken, ValueTask> operation,
        TState state,
        CancellationToken cancellationToken)
    {
        var previous = Current.Value;
        Current.Value = this;
        try
        {
            using var _ = ZLinkSpotAmbientContext.Push(this);
            using var turn = ZLinkSerialTurn.Suppress();
            await operation(this, state, cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            Current.Value = previous;
        }
    }

    private void QueueSerialized(
        Func<ZLinkEntrySpotActivation, CancellationToken, ValueTask> operation)
    {
        var admission = _serial.TryPostApplicationWithAdmission(
            ct => RunOnLineAsync(operation, ct),
            out _);
        ReportSerialAdmissionFailure("entry-spot-serial", admission);
    }

    private async ValueTask RunOnLineAsync(
        Func<ZLinkEntrySpotActivation, CancellationToken, ValueTask> operation,
        CancellationToken cancellationToken)
    {
        if (IsDisposed) return;

        var previous = Current.Value;
        Current.Value = this;
        try
        {
            using var _ = ZLinkSpotAmbientContext.Push(this);
            await operation(this, cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            Current.Value = previous;
        }
    }

    private ValueTask PublishTimerFailureAsync(
        ZLinkSpotTimerDescriptor descriptor,
        ZLinkTimerTick tick,
        Exception exception,
        bool stopped,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        _runtime.ReportTimerFailure(
            SpotNodeName,
            SpotId,
            true,
            descriptor.Name,
            descriptor.HandlerType,
            tick,
            exception,
            stopped);
        return ValueTask.CompletedTask;
    }

    public async ValueTask DispatchRouteDrainAsync(CancellationToken cancellationToken)
    {
        await DispatchRouteDrainTurnAsync(cancellationToken).ConfigureAwait(false);
    }

    public async ValueTask DispatchRouteAsync(
        ZLinkBackendRouteReceived received,
        CancellationToken cancellationToken)
    {
        await ExecuteAsync(
            static (activation, state, ct) => activation._dispatcher.DispatchRouteAsync(state, ct),
            received,
            cancellationToken).ConfigureAwait(false);
    }

    public async ValueTask DispatchActorJoinDrainAsync(CancellationToken cancellationToken)
    {
        await DispatchActorJoinDrainTurnAsync(cancellationToken).ConfigureAwait(false);
    }

    private async ValueTask DispatchRouteDrainTurnAsync(
        CancellationToken cancellationToken)
    {
        var hasMore = false;
        await ExecuteLifecycleAsync(
                async (activation, ct) => hasMore =
                    await activation._dispatcher.DispatchRouteDrainAsync(ct)
                        .ConfigureAwait(false),
                cancellationToken)
            .ConfigureAwait(false);
        if (hasMore && !cancellationToken.IsCancellationRequested)
            QueueRouteDrainTurn();
    }

    private async ValueTask DispatchActorJoinDrainTurnAsync(
        CancellationToken cancellationToken)
    {
        var hasMore = false;
        await ExecuteLifecycleAsync(
                async (activation, ct) => hasMore =
                    await activation._dispatcher.DispatchActorJoinDrainAsync(ct)
                        .ConfigureAwait(false),
                cancellationToken)
            .ConfigureAwait(false);
        if (hasMore && !cancellationToken.IsCancellationRequested)
            QueueActorJoinDrainTurn();
    }

    private void QueueRouteDrainTurn()
    {
        var admission = _serial.TryPostNextWithAdmission(
            ct => DispatchRouteDrainTurnAsync(ct),
            out _);
        ReportSerialAdmissionFailure("entry-spot-route-drain", admission);
    }

    private void QueueActorJoinDrainTurn()
    {
        var admission = _serial.TryPostNextWithAdmission(
            ct => DispatchActorJoinDrainTurnAsync(ct),
            out _);
        ReportSerialAdmissionFailure("entry-spot-actor-join-drain", admission);
    }

    private void ReportSerialAdmissionFailure(
        string operationName,
        ZLinkSerialPostAdmission admission)
    {
        if (admission is
            ZLinkSerialPostAdmission.Accepted
            or ZLinkSerialPostAdmission.Closed)
            return;
        _runtime.ErrorSink.ReportRuntimeTaskException(
            operationName,
            new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.CapacityExceeded,
                "The Entry SPOT serial queue is at capacity."));
    }

    public async ValueTask DispatchSubscriptionsAsync(CancellationToken cancellationToken)
    {
        await ExecuteAsync(
            static (activation, ct) => activation._dispatcher.DispatchSubscriptionsAsync(ct),
            cancellationToken).ConfigureAwait(false);
    }

    public async ValueTask DiscardSubscriptionsAsync(CancellationToken cancellationToken)
    {
        await ExecuteAsync(
            static (activation, ct) => activation._dispatcher.DiscardSubscriptionsAsync(ct),
            cancellationToken).ConfigureAwait(false);
    }
}
