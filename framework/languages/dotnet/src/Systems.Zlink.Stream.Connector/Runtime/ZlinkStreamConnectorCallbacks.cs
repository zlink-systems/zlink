namespace Systems.Zlink.Stream.Connector.Runtime;

internal sealed class ZlinkStreamConnectorCallbacks(
    ZlinkStreamTaskRunner taskRunner,
    ZlinkStreamDispatchMode dispatchMode,
    int maxPendingDispatchCallbacks,
    ZlinkStreamConnectorOptions options)
{
    private readonly object _dispatchGate = new();
    private readonly LinkedList<QueuedCallback> _dispatchQueue = new();

    private readonly ZlinkStreamHandlerList<Func<ZlinkStreamConnectionStateChanged, CancellationToken, ValueTask>>
        _connectionStateChanged = new();

    private readonly ZlinkStreamHandlerList<Func<ZlinkStreamDisconnected, CancellationToken, ValueTask>>
        _disconnected = new();

    private readonly ZlinkStreamHandlerList<Func<ZlinkStreamError, CancellationToken, ValueTask>>
        _errorReceived = new();

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

        _connectionStateChanged.Clear();
        _disconnected.Clear();
        _errorReceived.Clear();
    }

    public IDisposable AddErrorReceived(Func<ZlinkStreamError, CancellationToken, ValueTask> handler) =>
        _errorReceived.Add(handler);

    public IDisposable AddDisconnected(Func<ZlinkStreamDisconnected, CancellationToken, ValueTask> handler) =>
        _disconnected.Add(handler);

    public IDisposable AddConnectionStateChanged(
        Func<ZlinkStreamConnectionStateChanged, CancellationToken, ValueTask> handler) =>
        _connectionStateChanged.Add(handler);

    public async ValueTask PublishErrorAsync(ZlinkStreamError error, CancellationToken cancellationToken)
    {
        var handlers = _errorReceived.Snapshot();
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
        IReadOnlyList<ZlinkStreamHandlerList<THandler>.Registration> handlers,
        Func<THandler, CancellationToken, ValueTask> invoke,
        CancellationToken cancellationToken,
        bool reportErrors)
        where THandler : Delegate
    {
        if (handlers.Count == 0) return;

        await DispatchUserCallbackAsync(
                async dispatchedToken =>
                {
                    foreach (var registration in handlers)
                    {
                        // A registration disposed after this snapshot was taken must not
                        // run on this dispatch (stream-connector spec §7).
                        if (registration.IsRemoved) continue;

                        await InvokeUserCallbackAsync(
                                token => invoke(registration.Handler, token),
                                dispatchedToken,
                                reportErrors)
                            .ConfigureAwait(false);
                    }
                },
                cancellationToken,
                reportErrors: false)
            .ConfigureAwait(false);
    }

    public async ValueTask NotifyDisconnectedAsync(
        ZlinkStreamCloseReason closeReason,
        CancellationToken cancellationToken)
    {
        var handlers = _disconnected.Snapshot();
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
        var handlers = _connectionStateChanged.Snapshot();
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
        var handlers = _errorReceived.Snapshot();
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

    private readonly record struct QueuedCallback(
        Func<CancellationToken, ValueTask> Callback,
        bool ReportErrors,
        bool Required);
}

/// <summary>
///     Ordered set of lifecycle handlers where registering hands back the value that
///     removes the registration (stream-connector spec §7).
/// </summary>
/// <remarks>
///     A C# <c>event</c> cannot satisfy that contract: <c>+=</c> returns nothing, so a
///     client whose subscription lives as long as one screen has to keep the delegate
///     itself to unsubscribe later.
/// </remarks>
internal sealed class ZlinkStreamHandlerList<THandler>
    where THandler : Delegate
{
    private readonly object _gate = new();
    private Registration[] _handlers = [];

    public IDisposable Add(THandler handler)
    {
        ArgumentNullException.ThrowIfNull(handler);
        var registration = new Registration(handler);

        lock (_gate)
        {
            var next = new Registration[_handlers.Length + 1];
            Array.Copy(_handlers, next, _handlers.Length);
            next[^1] = registration;
            _handlers = next;
        }

        return new Subscription(() => Remove(registration));
    }

    /// <summary>
    ///     Returns the registrations in registration order. Each one still reports whether
    ///     it has been removed, so a handler disposed between this snapshot and a later
    ///     manual dispatch is skipped instead of invoked.
    /// </summary>
    /// <remarks>
    ///     Every write publishes a new array, so reading the reference is the whole
    ///     snapshot and a lock would add nothing to it.
    /// </remarks>
    public IReadOnlyList<Registration> Snapshot()
    {
        return Volatile.Read(ref _handlers);
    }

    public void Clear()
    {
        lock (_gate)
        {
            foreach (var registration in _handlers) registration.Remove();
            _handlers = [];
        }
    }

    private void Remove(Registration registration)
    {
        lock (_gate)
        {
            registration.Remove();
            var index = Array.IndexOf(_handlers, registration);
            if (index < 0) return;

            var next = new Registration[_handlers.Length - 1];
            if (index > 0) Array.Copy(_handlers, 0, next, 0, index);
            if (index < _handlers.Length - 1)
                Array.Copy(_handlers, index + 1, next, index, _handlers.Length - index - 1);

            _handlers = next;
        }
    }

    internal sealed class Registration(THandler handler)
    {
        private int _removed;

        public THandler Handler { get; } = handler;

        public bool IsRemoved => Volatile.Read(ref _removed) != 0;

        public void Remove() => Volatile.Write(ref _removed, 1);
    }

    private sealed class Subscription(Action dispose) : IDisposable
    {
        private int _disposed;

        public void Dispose()
        {
            if (Interlocked.Exchange(ref _disposed, 1) == 0) dispose();
        }
    }
}
