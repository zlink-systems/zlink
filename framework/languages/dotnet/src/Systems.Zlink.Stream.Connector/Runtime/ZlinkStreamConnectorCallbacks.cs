namespace Systems.Zlink.Stream.Connector.Runtime;

internal sealed class ZlinkStreamConnectorCallbacks(
    ZlinkStreamTaskRunner taskRunner,
    ZlinkStreamDispatchMode dispatchMode,
    int maxPendingDispatchCallbacks,
    ZlinkStreamConnectorOptions options)
{
    private readonly object _dispatchGate = new();
    private readonly LinkedList<QueuedCallback> _dispatchQueue = new();
    private readonly object _gate = new();
    private Func<ZlinkStreamConnectionStateChanged, CancellationToken, ValueTask>? _connectionStateChanged;
    private Func<ZlinkStreamDisconnected, CancellationToken, ValueTask>? _disconnected;
    private Func<ZlinkStreamError, CancellationToken, ValueTask>? _errorReceived;
    private bool _accepting = true;
    private int _pendingDispatchCount;
    private int _reservedRequestCallbacks;

    public int PendingDispatchCount => Volatile.Read(ref _pendingDispatchCount);

    public bool IsCurrentCallback => ZlinkStreamCallbackExecutionContext.IsActiveCallbackFor(this);

    public IDisposable EnterCallback() => ZlinkStreamCallbackExecutionContext.EnterCallback(this);

    public void Complete()
    {
        lock (_dispatchGate)
        {
            _accepting = false;
            _dispatchQueue.Clear();
            _reservedRequestCallbacks = 0;
            Volatile.Write(ref _pendingDispatchCount, 0);
        }

        lock (_gate)
        {
            _connectionStateChanged = null;
            _disconnected = null;
            _errorReceived = null;
        }
    }

    public void AddErrorReceived(Func<ZlinkStreamError, CancellationToken, ValueTask>? handler)
    {
        lock (_gate)
        {
            _errorReceived += handler;
        }
    }

    public void RemoveErrorReceived(Func<ZlinkStreamError, CancellationToken, ValueTask>? handler)
    {
        lock (_gate)
        {
            _errorReceived -= handler;
        }
    }

    public void AddDisconnected(Func<ZlinkStreamDisconnected, CancellationToken, ValueTask>? handler)
    {
        lock (_gate)
        {
            _disconnected += handler;
        }
    }

    public void RemoveDisconnected(Func<ZlinkStreamDisconnected, CancellationToken, ValueTask>? handler)
    {
        lock (_gate)
        {
            _disconnected -= handler;
        }
    }

    public void AddConnectionStateChanged(
        Func<ZlinkStreamConnectionStateChanged, CancellationToken, ValueTask>? handler)
    {
        lock (_gate)
        {
            _connectionStateChanged += handler;
        }
    }

    public void RemoveConnectionStateChanged(
        Func<ZlinkStreamConnectionStateChanged, CancellationToken, ValueTask>? handler)
    {
        lock (_gate)
        {
            _connectionStateChanged -= handler;
        }
    }

    public async ValueTask PublishErrorAsync(ZlinkStreamError error, CancellationToken cancellationToken)
    {
        var handlers = SnapshotErrorReceived();
        await DispatchSubscribersAsync(
                handlers,
                async (handler, dispatchedToken) =>
                {
                    using var flow = EnterLifecycleFlow();
                    await handler(error, dispatchedToken).ConfigureAwait(false);
                },
                cancellationToken,
                true)
            .ConfigureAwait(false);
    }

    private async ValueTask DispatchSubscribersAsync<THandler>(
        IReadOnlyList<THandler> handlers,
        Func<THandler, CancellationToken, ValueTask> invoke,
        CancellationToken cancellationToken,
        bool reportErrors)
        where THandler : Delegate
    {
        if (handlers.Count == 0) return;

        await DispatchUserCallbackAsync(
                async dispatchedToken =>
                {
                    foreach (var handler in handlers)
                        await InvokeUserCallbackAsync(
                                token => invoke(handler, token),
                                dispatchedToken,
                                reportErrors)
                            .ConfigureAwait(false);
                },
                cancellationToken,
                reportErrors: false)
            .ConfigureAwait(false);
    }

    public async ValueTask NotifyDisconnectedAsync(
        ZlinkStreamCloseReason closeReason,
        CancellationToken cancellationToken)
    {
        var handlers = SnapshotDisconnected();
        await DispatchSubscribersAsync(
                handlers,
                async (handler, dispatchedToken) =>
                {
                    using var flow = EnterLifecycleFlow();
                    await handler(new ZlinkStreamDisconnected(closeReason), dispatchedToken)
                        .ConfigureAwait(false);
                },
                cancellationToken,
                true)
            .ConfigureAwait(false);
    }

    public async ValueTask NotifyConnectionStateChangedAsync(
        ZlinkStreamConnectionStateChanged change,
        CancellationToken cancellationToken)
    {
        var handlers = SnapshotConnectionStateChanged();
        await DispatchSubscribersAsync(
                handlers,
                async (handler, dispatchedToken) =>
                {
                    using var flow = EnterLifecycleFlow();
                    await handler(change, dispatchedToken).ConfigureAwait(false);
                },
                cancellationToken,
                true)
            .ConfigureAwait(false);
    }

    public async ValueTask DispatchUserCallbackAsync(
        Func<CancellationToken, ValueTask> callback,
        CancellationToken cancellationToken,
        bool reportErrors = true)
    {
        ArgumentNullException.ThrowIfNull(callback);

        if (dispatchMode == ZlinkStreamDispatchMode.Immediate)
        {
            await InvokeUserCallbackAsync(callback, cancellationToken, reportErrors)
                .ConfigureAwait(false);
            return;
        }

        EnqueueDroppable(callback, reportErrors);
    }

    public async ValueTask DispatchAsync(CancellationToken cancellationToken)
    {
        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            QueuedCallback queued;
            lock (_dispatchGate)
            {
                if (_dispatchQueue.First is not { } first) return;
                queued = first.Value;
                _dispatchQueue.RemoveFirst();
                Volatile.Write(ref _pendingDispatchCount, _dispatchQueue.Count);
            }
            await InvokeUserCallbackAsync(queued.Callback, cancellationToken, queued.ReportErrors)
                .ConfigureAwait(false);
        }
    }

    public void QueueRequestCallback<TResult>(
        Func<ValueTask<ZlinkStreamRequestCompletion>> request,
        Func<ZlinkStreamEncodedPayload, TResult> success,
        Func<ZlinkStreamError, TResult> failure,
        Action<TResult> callback)
    {
        var reserved = ReserveRequestCallback();
        try
        {
            taskRunner.RunDetached(
                async _ =>
                {
                    Func<CancellationToken, ValueTask> completion;
                    try
                    {
                        var reply = await request().ConfigureAwait(false);
                        completion = _ =>
                        {
                            // Read once here so the completion closure judges the
                            // reply consistently even if the level changes again
                            // before it runs.
                            var diagnosticsLevel = options.DiagnosticsLevel;
                            using var flow = diagnosticsLevel == ZlinkStreamDiagnosticsLevel.Off
                                ? null
                                : ZlinkStreamFlowContext.Enter(reply.FlowId, reply.FlowOrigin);
                            callback(reply.Error is { } error
                                ? failure(error)
                                : success(reply.Payload!));
                            return ValueTask.CompletedTask;
                        };
                    }
                    catch (ZlinkStreamException ex)
                    {
                        completion = _ =>
                        {
                            callback(failure(ex.Error));
                            return ValueTask.CompletedTask;
                        };
                    }
                    catch (Exception ex)
                    {
                        var error = new ZlinkStreamError(
                            ZlinkStreamErrorCode.SendFailed,
                            ex.Message,
                            ex);
                        completion = _ =>
                        {
                            callback(failure(error));
                            return ValueTask.CompletedTask;
                        };
                    }

                    await DispatchRequestCompletionAsync(completion, reserved).ConfigureAwait(false);
                });
        }
        catch
        {
            ReleaseRequestCallbackReservation(reserved);
            throw;
        }
    }

    private IDisposable? EnterLifecycleFlow()
    {
        // Lifecycle flows are trace-only; at Off no flow context is created. One
        // atomic read decides this single call.
        return options.DiagnosticsLevel == ZlinkStreamDiagnosticsLevel.Off
            ? null
            : ZlinkStreamFlowContext.EnterNew(ZlinkStreamFlowOrigin.Lifecycle);
    }

    private async ValueTask InvokeUserCallbackAsync(
        Func<CancellationToken, ValueTask> callback,
        CancellationToken cancellationToken,
        bool reportErrors)
    {
        using var permit = ZlinkStreamCallbackExecutionContext.EnterCallback(this);
        try
        {
            await callback(cancellationToken).ConfigureAwait(false);
        }
        catch (Exception ex) when (reportErrors)
        {
            await ReportUserCallbackErrorAsync(ex, cancellationToken).ConfigureAwait(false);
        }
        catch
        {
        }
    }

    private async ValueTask ReportUserCallbackErrorAsync(
        Exception exception,
        CancellationToken cancellationToken)
    {
        var handlers = SnapshotErrorReceived();
        if (handlers.Count == 0) return;

        var error = new ZlinkStreamError(
            ZlinkStreamErrorCode.UserCallbackFailed,
            "User callback failed.",
            exception);

        await DispatchSubscribersAsync(
                handlers,
                (handler, dispatchedToken) => handler(error, dispatchedToken),
                cancellationToken,
                false)
            .ConfigureAwait(false);
    }

    private bool ReserveRequestCallback()
    {
        if (dispatchMode == ZlinkStreamDispatchMode.Immediate) return false;

        lock (_dispatchGate)
        {
            ObjectDisposedException.ThrowIf(!_accepting, this);
            while (_dispatchQueue.Count + _reservedRequestCallbacks >= maxPendingDispatchCallbacks)
                if (!TryDropOldestDroppableLocked())
                    throw ZlinkStreamConnector.Error(
                        ZlinkStreamErrorCode.SendFailed,
                        "Connector request callback queue is full.");

            _reservedRequestCallbacks++;
            return true;
        }
    }

    private async ValueTask DispatchRequestCompletionAsync(
        Func<CancellationToken, ValueTask> callback,
        bool reserved)
    {
        if (!reserved)
        {
            await InvokeUserCallbackAsync(callback, CancellationToken.None, true).ConfigureAwait(false);
            return;
        }

        lock (_dispatchGate)
        {
            _reservedRequestCallbacks--;
            if (!_accepting) return;
            _dispatchQueue.AddLast(new QueuedCallback(callback, true, true));
            Volatile.Write(ref _pendingDispatchCount, _dispatchQueue.Count);
        }
    }

    private void EnqueueDroppable(
        Func<CancellationToken, ValueTask> callback,
        bool reportErrors)
    {
        lock (_dispatchGate)
        {
            if (!_accepting) return;
            while (_dispatchQueue.Count + _reservedRequestCallbacks >= maxPendingDispatchCallbacks)
                if (!TryDropOldestDroppableLocked()) return;

            _dispatchQueue.AddLast(new QueuedCallback(callback, reportErrors, false));
            Volatile.Write(ref _pendingDispatchCount, _dispatchQueue.Count);
        }
    }

    private void ReleaseRequestCallbackReservation(bool reserved)
    {
        if (!reserved) return;

        lock (_dispatchGate)
        {
            if (_reservedRequestCallbacks > 0) _reservedRequestCallbacks--;
        }
    }

    private bool TryDropOldestDroppableLocked()
    {
        for (var current = _dispatchQueue.First; current is not null; current = current.Next)
        {
            if (current.Value.Required) continue;
            _dispatchQueue.Remove(current);
            Volatile.Write(ref _pendingDispatchCount, _dispatchQueue.Count);
            return true;
        }

        return false;
    }

    private IReadOnlyList<Func<ZlinkStreamError, CancellationToken, ValueTask>> SnapshotErrorReceived()
    {
        lock (_gate)
        {
            return Snapshot(_errorReceived);
        }
    }

    private IReadOnlyList<Func<ZlinkStreamDisconnected, CancellationToken, ValueTask>> SnapshotDisconnected()
    {
        lock (_gate)
        {
            return Snapshot(_disconnected);
        }
    }

    private IReadOnlyList<Func<ZlinkStreamConnectionStateChanged, CancellationToken, ValueTask>>
        SnapshotConnectionStateChanged()
    {
        lock (_gate)
        {
            return Snapshot(_connectionStateChanged);
        }
    }

    private static IReadOnlyList<THandler> Snapshot<THandler>(THandler? handlers)
        where THandler : Delegate
    {
        if (handlers is null) return Array.Empty<THandler>();

        return handlers.GetInvocationList().Cast<THandler>().ToArray();
    }

    private readonly record struct QueuedCallback(
        Func<CancellationToken, ValueTask> Callback,
        bool ReportErrors,
        bool Required);
}
