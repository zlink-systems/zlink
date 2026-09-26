using System.Runtime.ExceptionServices;
using System.Security.Authentication;

namespace Systems.Zlink.Stream.Connector.Runtime;

internal sealed class ZlinkStreamConnectorLifecycle(
    ZlinkStreamConnectorOptions options,
    ZlinkStreamPendingRequests pending,
    ZlinkStreamTaskRunner taskRunner,
    ZlinkStreamConnectorCallbacks callbacks,
    Func<CancellationToken, ValueTask<IZlinkStreamConnection>> connectTransport,
    Action<long> onConnectionEstablished,
    Func<ValueTask> onConnectionEnded,
    Func<ValueTask> failUnwrittenFrames
) : IDisposable
{
    /// <summary>
    ///     Lower bound of the reconnect jitter window: the loop waits between this share of
    ///     the base delay and the full base delay (stream-connector spec §6).
    /// </summary>
    internal const double ReconnectJitterFloor = 0.5;

    private readonly CancellationTokenSource _closeCts = new();
    private readonly object _gate = new();
    private readonly ZlinkStreamHeartbeatMonitor _heartbeat = new(options.Heartbeat);
    private Task? _activeConnectTask;
    private Task? _closeTask;
    private Action? _startActiveConnect;
    private IZlinkStreamConnection? _connection;
    private Task? _heartbeatTask;
    private Task? _receiveTask;
    private Func<CancellationToken, Task>? _runReceiveLoop;
    private Func<CancellationToken, ValueTask>? _sendHeartbeatPing;
    private CancellationTokenSource? _sessionCts;
    private long _connectionGeneration;
    private ZlinkStreamConnectionState _state = ZlinkStreamConnectionState.Created;
    private ZlinkStreamCloseReason? _lastCloseReason;

    public IZlinkStreamConnection? Connection
    {
        get
        {
            lock (_gate)
            {
                return _connection;
            }
        }
    }

    public ZlinkStreamConnectionState State
    {
        get
        {
            lock (_gate)
            {
                return _state;
            }
        }
    }

    public bool IsConnected => State == ZlinkStreamConnectionState.Connected;

    internal ZlinkStreamCloseReason? LastCloseReason
    {
        get
        {
            lock (_gate)
                return _lastCloseReason;
        }
    }

    public void Dispose()
    {
        _closeCts.Dispose();
        _sessionCts?.Dispose();
    }

    public async ValueTask ConnectAsync(
        Func<CancellationToken, Task> runReceiveLoop,
        Func<CancellationToken, ValueTask> sendHeartbeatPing,
        Action throwIfDisposed,
        CancellationToken cancellationToken
    )
    {
        Task? waitTask;
        long observedConnectionGeneration;
        ActiveConnectStart? activeConnectStart = null;
        ZlinkStreamConnectionStateChanged? change = null;
        lock (_gate)
        {
            throwIfDisposed();
            if (_state == ZlinkStreamConnectionState.Closed)
                throw ClosedError();

            _runReceiveLoop = runReceiveLoop;
            _sendHeartbeatPing = sendHeartbeatPing;
            observedConnectionGeneration = _connectionGeneration;

            if (_state == ZlinkStreamConnectionState.Connected)
                return;

            if (_activeConnectTask is not null)
            {
                waitTask = _activeConnectTask;
            }
            else
            {
                change = SetStateLocked(ZlinkStreamConnectionState.Connecting, null);
                activeConnectStart = CreateActiveConnectTask(() =>
                    ConnectOnceAsync(cancellationToken)
                );
                waitTask = activeConnectStart.Value.Task;
                _activeConnectTask = waitTask;
                _startActiveConnect = activeConnectStart.Value.Start;
                ObserveBackgroundTask(waitTask);
            }
        }

        await NotifyStateChangedAsync(change, cancellationToken).ConfigureAwait(false);
        activeConnectStart?.Start();
        try
        {
            await waitTask.WaitAsync(cancellationToken).ConfigureAwait(false);
        }
        catch (Exception) when (State == ZlinkStreamConnectionState.Closed)
        {
            throw ClosedError();
        }

        lock (_gate)
        {
            if (
                _state == ZlinkStreamConnectionState.Closed
                && _connectionGeneration == observedConnectionGeneration
            )
                throw ClosedError();
        }
    }

    public async ValueTask CloseAsync(CancellationToken cancellationToken)
    {
        // A close called from a handler or callback starts the close work and returns; the
        // result is awaited by a close called outside them (stream-connector spec §7).
        var insideCallback = callbacks.IsCurrentCallback;
        Task closeTask;
        TaskCompletionSource<bool>? startClose = null;
        lock (_gate)
        {
            if (_closeTask is null)
            {
                _closeCts.Cancel();
                var snapshot = DetachLocked();
                var activeConnectTask = _activeConnectTask;
                var startActiveConnect = _startActiveConnect;
                _activeConnectTask = null;
                _startActiveConnect = null;
                // Closing an established connection is itself a close reason, and the
                // read surface must show it after the fact (stream-connector spec §6.2).
                if (snapshot.Connection is not null)
                    _lastCloseReason = ZlinkStreamCloseReason.ClientClose;
                var change = SetStateLocked(ZlinkStreamConnectionState.Closed, null);

                startClose = new TaskCompletionSource<bool>(
                    TaskCreationOptions.RunContinuationsAsynchronously
                );
                _closeTask = RunFullCloseAsync(
                    startClose.Task,
                    snapshot,
                    activeConnectTask,
                    startActiveConnect,
                    change
                );
            }

            closeTask = _closeTask;
        }

        startClose?.TrySetResult(true);
        if (!insideCallback)
            await closeTask.WaitAsync(cancellationToken).ConfigureAwait(false);
    }

    private async Task RunFullCloseAsync(
        Task started,
        LifecycleSnapshot snapshot,
        Task? activeConnectTask,
        Action? startActiveConnect,
        ZlinkStreamConnectionStateChanged? change
    )
    {
        await started.ConfigureAwait(false);

        startActiveConnect?.Invoke();
        snapshot.SessionCts?.Cancel();
        Exception? closeException = null;
        try
        {
            await CloseConnectionAsync(snapshot.Connection, CancellationToken.None)
                .ConfigureAwait(false);
        }
        catch (Exception ex)
        {
            closeException = ex;
        }

        // The transport is closed, so no frame not yet written reaches it; their operations
        // fail with Disconnected here, without waiting for the peer (stream-connector spec §7).
        await failUnwrittenFrames().ConfigureAwait(false);
        pending.FailAll(
            new ZlinkStreamError(ZlinkStreamErrorCode.Disconnected, "Connector closed.")
        );
        // Spec §10.1.1: closing the connector ends the connection a wait was observing,
        // and the wait ends with it.
        await onConnectionEnded().ConfigureAwait(false);
        await WaitBackgroundTaskAsync(snapshot.ReceiveTask).ConfigureAwait(false);
        await WaitBackgroundTaskAsync(snapshot.HeartbeatTask).ConfigureAwait(false);
        await WaitBackgroundTaskAsync(activeConnectTask).ConfigureAwait(false);
        snapshot.SessionCts?.Dispose();

        await NotifyStateChangedAsync(change, CancellationToken.None).ConfigureAwait(false);
        if (snapshot.Connection is not null)
            StartDisconnectNotification(ZlinkStreamCloseReason.ClientClose);

        if (closeException is not null)
            ExceptionDispatchInfo.Capture(closeException).Throw();
    }

    /// <summary>
    ///     Runs the registered disconnect handlers and returns without waiting for them to
    ///     finish (stream-connector spec §7).
    /// </summary>
    /// <remarks>
    ///     Making the call is what runs the handlers: under <c>Immediate</c> dispatch each
    ///     handler runs inline up to its first suspension, and under <c>Manual</c> dispatch the
    ///     whole notification completes synchronously by queueing them for the next pump, so
    ///     that mode sees no change. Only a handler that suspends leaves a remainder behind,
    ///     and that remainder is abandoned here: a handler that calls
    ///     <see cref="CloseAsync" /> would otherwise wait on the very close task it is running
    ///     under, and the two would wait on each other. A notification that did finish still
    ///     surfaces its failure the way it always has; the abandoned remainder has its failure
    ///     observed so a handler's exception never reaches the finalizer.
    /// </remarks>
    private void StartDisconnectNotification(ZlinkStreamCloseReason closeReason)
    {
        var notification = callbacks.NotifyDisconnectedAsync(closeReason, CancellationToken.None);
        if (notification.IsCompleted)
        {
            notification.GetAwaiter().GetResult();
            return;
        }

        ObserveBackgroundTask(notification.AsTask());
    }

    public void RecordInbound()
    {
        _heartbeat.RecordInbound();
    }

    /// <summary>
    ///     Publishes <paramref name="error" /> and ends the current connection with
    ///     <paramref name="closeReason" />, which the caller decides where it detected the
    ///     failure (stream-connector spec §6.2, §9).
    /// </summary>
    public async ValueTask HandleTransportErrorAsync(
        ZlinkStreamError error,
        ZlinkStreamCloseReason closeReason,
        CancellationToken cancellationToken = default
    )
    {
        await callbacks.PublishErrorAsync(error, cancellationToken).ConfigureAwait(false);
        await HandleDisconnectAsync(error, closeReason, cancellationToken).ConfigureAwait(false);
    }

    /// <summary>
    ///     Handles a write failure on <paramref name="sourceConnection" />. Returns
    ///     <see langword="false" /> without publishing or disconnecting when that connection
    ///     had already ended — a transport loss or <c>Close</c> — so its failure belongs to
    ///     that ending, not to the current connection (stream-connector spec §7, §9).
    /// </summary>
    public ValueTask<bool> HandleTransportErrorAsync(
        ZlinkStreamError error,
        CancellationToken cancellationToken,
        IZlinkStreamConnection sourceConnection
    ) =>
        HandleDisconnectAsync(
            error,
            ZlinkStreamCloseReason.TransportError,
            cancellationToken,
            sourceConnection,
            publishError: true
        );

    public ValueTask HandleServerCloseAsync(
        ZlinkStreamCloseReason closeReason,
        string? diagnostic,
        CancellationToken cancellationToken = default
    )
    {
        var error = new ZlinkStreamError(
            ZlinkStreamErrorCode.Disconnected,
            diagnostic ?? $"Server closed the stream session ({closeReason})."
        );
        return new ValueTask(HandleDisconnectAsync(error, closeReason, cancellationToken).AsTask());
    }

    private async Task ConnectOnceAsync(CancellationToken cancellationToken)
    {
        try
        {
            var connection = await OpenConnectionAsync(cancellationToken).ConfigureAwait(false);
            await AttachConnectionAsync(connection, cancellationToken).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            await TransitionToDisconnectedAsync(
                    new ZlinkStreamError(ZlinkStreamErrorCode.Disconnected, "Connect canceled."),
                    CancellationToken.None
                )
                .ConfigureAwait(false);
            throw;
        }
        catch (Exception ex) when (ex is not ZlinkStreamException)
        {
            var error = MapConnectException(ex, cancellationToken);
            await TransitionToDisconnectedAsync(error, cancellationToken).ConfigureAwait(false);
            throw new ZlinkStreamException(error);
        }
        catch (ZlinkStreamException ex)
        {
            await TransitionToDisconnectedAsync(ex.Error, cancellationToken).ConfigureAwait(false);
            throw;
        }
    }

    private async Task ReconnectLoopAsync()
    {
        var reconnect = options.Reconnect;
        if (!reconnect.Enabled)
            return;

        var baseDelay =
            reconnect.InitialDelay <= reconnect.MaxDelay
                ? reconnect.InitialDelay
                : reconnect.MaxDelay;
        var attempt = 0;
        ZlinkStreamError? lastError = null;

        try
        {
            while (!_closeCts.IsCancellationRequested)
            {
                // Clients that all dropped together must not all come back at the same
                // instant, so the wait is a random point in the lower half of the base
                // delay's range (stream-connector spec §6).
                await Task.Delay(ApplyReconnectJitter(baseDelay), _closeCts.Token)
                    .ConfigureAwait(false);
                attempt++;

                try
                {
                    var connection = await OpenConnectionAsync(_closeCts.Token)
                        .ConfigureAwait(false);
                    await AttachConnectionAsync(connection, _closeCts.Token).ConfigureAwait(false);
                    return;
                }
                catch (Exception ex) when (!_closeCts.IsCancellationRequested)
                {
                    lastError = ex is ZlinkStreamException streamException
                        ? streamException.Error
                        : MapConnectException(ex, _closeCts.Token);
                    await callbacks
                        .PublishErrorAsync(lastError, CancellationToken.None)
                        .ConfigureAwait(false);

                    if (reconnect.MaxAttempts is { } maxAttempts && attempt >= maxAttempts)
                    {
                        // The attempts are spent: the state settles at Disconnected and
                        // the registered disconnect handlers run (spec §6). A configuration
                        // with unlimited attempts never reaches this point.
                        await TransitionToDisconnectedAsync(lastError, CancellationToken.None)
                            .ConfigureAwait(false);
                        StartDisconnectNotification(ZlinkStreamCloseReason.TransportError);
                        throw new ZlinkStreamException(lastError);
                    }

                    baseDelay = NextReconnectDelay(baseDelay, reconnect);
                }
            }
        }
        catch (OperationCanceledException) when (_closeCts.IsCancellationRequested) { }
        catch (ZlinkStreamException) when (_closeCts.IsCancellationRequested)
        {
            // Close won the race after the transport connected but before the
            // reconnect loop could attach it. AttachConnectionAsync already
            // closed that transport; the background reconnect is complete.
        }
    }

    private async ValueTask<IZlinkStreamConnection> OpenConnectionAsync(
        CancellationToken cancellationToken
    )
    {
        using var timeoutCts = CancellationTokenSource.CreateLinkedTokenSource(
            cancellationToken,
            _closeCts.Token
        );
        timeoutCts.CancelAfter(options.ConnectTimeout);

        try
        {
            return await connectTransport(timeoutCts.Token).ConfigureAwait(false);
        }
        catch (OperationCanceledException ex)
            when (!cancellationToken.IsCancellationRequested && !_closeCts.IsCancellationRequested)
        {
            throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.ConnectTimeout,
                "Connect timed out.",
                ex
            );
        }
        catch (AuthenticationException ex)
        {
            throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.TlsValidationFailed,
                "TLS validation failed.",
                ex
            );
        }
    }

    private async ValueTask AttachConnectionAsync(
        IZlinkStreamConnection connection,
        CancellationToken cancellationToken
    )
    {
        // _runReceiveLoop is assigned under the gate, so it is read under the gate too.
        Func<CancellationToken, Task> runReceiveLoop;
        lock (_gate)
        {
            runReceiveLoop =
                _runReceiveLoop
                ?? throw ZlinkStreamConnector.Error(
                    ZlinkStreamErrorCode.ConfigurationError,
                    "Receive loop is not configured."
                );
        }

        var sessionCts = CancellationTokenSource.CreateLinkedTokenSource(_closeCts.Token);
        _heartbeat.RecordSessionStart();

        LifecycleSnapshot oldSnapshot;
        ZlinkStreamConnectionStateChanged? change;
        var closeAttachedConnection = false;
        lock (_gate)
        {
            if (_state == ZlinkStreamConnectionState.Closed)
            {
                oldSnapshot = default;
                change = null;
                closeAttachedConnection = true;
            }
            else
            {
                oldSnapshot = DetachLocked();
                _connection = connection;
                _sessionCts = sessionCts;
                change = SetStateLocked(ZlinkStreamConnectionState.Connected, null);
            }
        }

        if (closeAttachedConnection)
        {
            sessionCts.Cancel();
            await CloseConnectionAsync(connection, cancellationToken).ConfigureAwait(false);
            sessionCts.Dispose();
            throw ClosedError();
        }

        oldSnapshot.SessionCts?.Cancel();
        await CloseConnectionAsync(oldSnapshot.Connection, cancellationToken).ConfigureAwait(false);
        oldSnapshot.SessionCts?.Dispose();
        await NotifyStateChangedAsync(change, cancellationToken).ConfigureAwait(false);

        long establishedGeneration;
        lock (_gate)
        {
            if (
                !ReferenceEquals(_connection, connection)
                || !ReferenceEquals(_sessionCts, sessionCts)
            )
            {
                if (_state == ZlinkStreamConnectionState.Closed)
                    throw ClosedError();

                return;
            }

            establishedGeneration = ++_connectionGeneration;
        }

        // An established connection is the baseline for the receive queue, so every
        // reconnect starts the counters at zero and drops what the previous connection
        // left unconsumed (spec §10). It runs before the receive loop starts, so no
        // arrival of this connection is lost, and outside this gate, because it takes the
        // receive queue's own lock: holding both here is the order a waiter whose
        // predicate reads State takes them in reverse. The generation makes a call from a
        // superseded attach a no-op.
        onConnectionEstablished(establishedGeneration);

        lock (_gate)
        {
            if (
                !ReferenceEquals(_connection, connection)
                || !ReferenceEquals(_sessionCts, sessionCts)
            )
            {
                if (_state == ZlinkStreamConnectionState.Closed)
                    throw ClosedError();

                return;
            }

            _receiveTask = taskRunner.Run(_ => new ValueTask(
                RunReceiveLoopGuardedAsync(runReceiveLoop, sessionCts.Token)
            ));
            _heartbeatTask = options.Heartbeat.Enabled
                ? taskRunner.Run(_ => new ValueTask(RunHeartbeatLoopAsync(sessionCts.Token)))
                : null;
        }
    }

    private async Task RunReceiveLoopGuardedAsync(
        Func<CancellationToken, Task> runReceiveLoop,
        CancellationToken cancellationToken
    )
    {
        try
        {
            await runReceiveLoop(cancellationToken).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            return;
        }
        catch (Exception ex)
        {
            if (cancellationToken.IsCancellationRequested)
                return;

            // The receive loop is where a read or a decode fails, so the close reason is
            // decided here (stream-connector spec §9): a frame or header that does not decode
            // and a frame over the receive limit are protocol violations, and every other
            // failure of the read is the transport failing.
            var error = ex switch
            {
                ZlinkStreamException streamException => streamException.Error,
                _ => new ZlinkStreamError(
                    ZlinkStreamErrorCode.Disconnected,
                    "Transport read failed.",
                    ex
                ),
            };
            await HandleTransportErrorAsync(error, MapCloseReason(error), CancellationToken.None)
                .ConfigureAwait(false);
            return;
        }

        if (!cancellationToken.IsCancellationRequested)
            await HandleDisconnectAsync(
                    new ZlinkStreamError(
                        ZlinkStreamErrorCode.Disconnected,
                        "Connector disconnected."
                    ),
                    ZlinkStreamCloseReason.TransportError,
                    CancellationToken.None
                )
                .ConfigureAwait(false);
    }

    private async Task RunHeartbeatLoopAsync(CancellationToken cancellationToken)
    {
        // _sendHeartbeatPing is assigned under the gate, so it is read under the gate too.
        Func<CancellationToken, ValueTask>? sendHeartbeatPing;
        lock (_gate)
        {
            sendHeartbeatPing = _sendHeartbeatPing;
        }

        await _heartbeat
            .RunAsync(sendHeartbeatPing, HandleTransportErrorAsync, cancellationToken)
            .ConfigureAwait(false);
    }

    /// <summary>
    ///     Ends the current connection because of <paramref name="error" />. The check under
    ///     the lock is the one decision that this ending belongs to the current connection:
    ///     it returns <see langword="false" /> and does nothing when the connector is already
    ///     closed or disconnected, or when <paramref name="sourceConnection" /> is no longer
    ///     the current connection. When <paramref name="publishError" /> is set, the error is
    ///     published only after this ending has been claimed.
    /// </summary>
    private async ValueTask<bool> HandleDisconnectAsync(
        ZlinkStreamError error,
        ZlinkStreamCloseReason closeReason,
        CancellationToken cancellationToken,
        IZlinkStreamConnection? sourceConnection = null,
        bool publishError = false
    )
    {
        LifecycleSnapshot snapshot;
        ZlinkStreamConnectionStateChanged? change;
        ActiveConnectStart? reconnectStart = null;
        lock (_gate)
        {
            if (sourceConnection is not null && !ReferenceEquals(_connection, sourceConnection))
                return false;
            if (
                _state
                is ZlinkStreamConnectionState.Closed
                    or ZlinkStreamConnectionState.Disconnected
            )
                return false;

            snapshot = DetachLocked();
            _lastCloseReason = closeReason;
            var nextState = options.Reconnect.Enabled
                ? ZlinkStreamConnectionState.Reconnecting
                : ZlinkStreamConnectionState.Disconnected;
            change = SetStateLocked(nextState, error);

            if (nextState == ZlinkStreamConnectionState.Reconnecting && _activeConnectTask is null)
            {
                reconnectStart = CreateActiveConnectTask(ReconnectLoopAsync);
                _activeConnectTask = reconnectStart.Value.Task;
                _startActiveConnect = reconnectStart.Value.Start;
                ObserveBackgroundTask(reconnectStart.Value.Task);
            }
        }

        Exception? closeFailure = null;
        List<Exception>? terminalFailures = null;
        if (publishError)
            await CaptureAsync(() => callbacks.PublishErrorAsync(error, cancellationToken))
                .ConfigureAwait(false);
        Capture(() => snapshot.SessionCts?.Cancel());
        try
        {
            await CloseConnectionAsync(snapshot.Connection, CancellationToken.None)
                .ConfigureAwait(false);
        }
        catch (Exception exception)
        {
            closeFailure = exception;
        }

        Capture(() => snapshot.SessionCts?.Dispose());
        Capture(() => pending.FailAll(GetPendingDisconnectError(error)));
        // Spec §10.1.1: a wait is released when the connection it observed ends, here,
        // and not when the reconnect that may follow establishes the next one.
        await CaptureAsync(onConnectionEnded).ConfigureAwait(false);
        await CaptureAsync(() => NotifyStateChangedAsync(change, CancellationToken.None))
            .ConfigureAwait(false);
        StartDisconnectNotification(closeReason);
        Capture(() => reconnectStart?.Start());

        if (closeFailure is not null && terminalFailures is not null)
            throw new AggregateException([closeFailure, .. terminalFailures]);
        if (closeFailure is not null)
            ExceptionDispatchInfo.Capture(closeFailure).Throw();
        if (terminalFailures is { Count: 1 })
            ExceptionDispatchInfo.Capture(terminalFailures[0]).Throw();
        if (terminalFailures is { Count: > 1 })
            throw new AggregateException(terminalFailures);
        return true;

        async ValueTask CaptureAsync(Func<ValueTask> operation)
        {
            try
            {
                await operation().ConfigureAwait(false);
            }
            catch (Exception exception)
            {
                (terminalFailures ??= []).Add(exception);
            }
        }

        void Capture(Action operation)
        {
            try
            {
                operation();
            }
            catch (Exception exception)
            {
                (terminalFailures ??= []).Add(exception);
            }
        }
    }

    private async ValueTask TransitionToDisconnectedAsync(
        ZlinkStreamError error,
        CancellationToken cancellationToken
    )
    {
        ZlinkStreamConnectionStateChanged? change;
        lock (_gate)
        {
            if (_state == ZlinkStreamConnectionState.Closed)
                return;

            // A first connect that never reached Connected still leaves a reason behind,
            // so callers can tell how the attempt ended (stream-connector spec §6.2).
            // A connect attempt fails at the transport, so the reason is TransportError, where
            // §9 also puts ConnectTimeout and TlsValidationFailed.
            _lastCloseReason = ZlinkStreamCloseReason.TransportError;
            change = SetStateLocked(ZlinkStreamConnectionState.Disconnected, error);
        }

        await NotifyStateChangedAsync(change, cancellationToken).ConfigureAwait(false);
    }

    private LifecycleSnapshot DetachLocked()
    {
        var snapshot = new LifecycleSnapshot(
            _connection,
            _sessionCts,
            _receiveTask,
            _heartbeatTask
        );
        _connection = null;
        _sessionCts = null;
        _receiveTask = null;
        _heartbeatTask = null;
        return snapshot;
    }

    private ZlinkStreamConnectionStateChanged? SetStateLocked(
        ZlinkStreamConnectionState next,
        ZlinkStreamError? error
    )
    {
        if (_state == next)
            return null;

        var previous = _state;
        _state = next;
        return new ZlinkStreamConnectionStateChanged(previous, next, error);
    }

    private ValueTask NotifyStateChangedAsync(
        ZlinkStreamConnectionStateChanged? change,
        CancellationToken cancellationToken
    )
    {
        if (change is null)
            return default(ValueTask);

        var notification = callbacks.NotifyConnectionStateChangedAsync(change, cancellationToken);
        if (notification.IsCompleted)
        {
            notification.GetAwaiter().GetResult();
            return default(ValueTask);
        }

        ObserveBackgroundTask(notification.AsTask());
        return default(ValueTask);
    }

    private static ZlinkStreamCloseReason MapCloseReason(ZlinkStreamError error) =>
        error.Code is ZlinkStreamErrorCode.FrameDecodeFailed or ZlinkStreamErrorCode.FrameTooLarge
            ? ZlinkStreamCloseReason.ProtocolError
            : ZlinkStreamCloseReason.TransportError;

    private static async ValueTask CloseConnectionAsync(
        IZlinkStreamConnection? connection,
        CancellationToken cancellationToken
    )
    {
        if (connection is not null)
            await connection.CloseAsync(cancellationToken).ConfigureAwait(false);
    }

    private static async ValueTask WaitBackgroundTaskAsync(Task? task)
    {
        if (task is null)
            return;

        try
        {
            await task.ConfigureAwait(false);
        }
        catch (OperationCanceledException) { }
        catch (ZlinkStreamException) { }
    }

    private ActiveConnectStart CreateActiveConnectTask(Func<Task> run)
    {
        var started = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        Task? activeTask = null;
        activeTask = RunActiveConnectTaskAsync(started.Task, run, () => activeTask);
        return new ActiveConnectStart(activeTask, () => started.TrySetResult(true));
    }

    private async Task RunActiveConnectTaskAsync(
        Task started,
        Func<Task> run,
        Func<Task?> currentTask
    )
    {
        await started.ConfigureAwait(false);
        try
        {
            await run().ConfigureAwait(false);
        }
        finally
        {
            ClearActiveConnectTask(currentTask());
        }
    }

    private void ClearActiveConnectTask(Task? task)
    {
        if (task is null)
            return;

        ActiveConnectStart? reconnectStart = null;
        lock (_gate)
        {
            if (!ReferenceEquals(_activeConnectTask, task))
                return;

            _activeConnectTask = null;
            _startActiveConnect = null;
            if (
                _state == ZlinkStreamConnectionState.Reconnecting
                && !_closeCts.IsCancellationRequested
            )
            {
                reconnectStart = CreateActiveConnectTask(ReconnectLoopAsync);
                _activeConnectTask = reconnectStart.Value.Task;
                _startActiveConnect = reconnectStart.Value.Start;
                ObserveBackgroundTask(reconnectStart.Value.Task);
            }
        }

        reconnectStart?.Start();
    }

    private static void ObserveBackgroundTask(Task task)
    {
        _ = task.ContinueWith(
            static completed => _ = completed.Exception,
            CancellationToken.None,
            TaskContinuationOptions.OnlyOnFaulted | TaskContinuationOptions.ExecuteSynchronously,
            TaskScheduler.Default
        );
    }

    private static ZlinkStreamError MapConnectException(
        Exception ex,
        CancellationToken cancellationToken
    )
    {
        return ex switch
        {
            OperationCanceledException canceled when !cancellationToken.IsCancellationRequested =>
                new ZlinkStreamError(
                    ZlinkStreamErrorCode.ConnectTimeout,
                    "Connect timed out.",
                    canceled
                ),
            AuthenticationException authentication => new ZlinkStreamError(
                ZlinkStreamErrorCode.TlsValidationFailed,
                "TLS validation failed.",
                authentication
            ),
            _ => new ZlinkStreamError(ZlinkStreamErrorCode.Disconnected, "Connect failed.", ex),
        };
    }

    /// <summary>
    ///     Advances the base delay by one backoff step, stopping at the maximum delay.
    /// </summary>
    /// <remarks>
    ///     The base delay is the deterministic part of the schedule. What the loop actually
    ///     waits is <see cref="ApplyReconnectJitter" /> of this value.
    /// </remarks>
    private static TimeSpan NextReconnectDelay(
        TimeSpan current,
        ZlinkStreamReconnectOptions options
    )
    {
        var nextMilliseconds = current.TotalMilliseconds * options.BackoffFactor;
        if (nextMilliseconds >= options.MaxDelay.TotalMilliseconds)
            return options.MaxDelay;

        return TimeSpan.FromMilliseconds(nextMilliseconds);
    }

    /// <summary>
    ///     Picks the wait before one reconnect attempt: a value between 50% and 100% of
    ///     <paramref name="baseDelay" /> (stream-connector spec §6).
    /// </summary>
    private static TimeSpan ApplyReconnectJitter(TimeSpan baseDelay) =>
        ScaleReconnectDelay(
            baseDelay,
            System.Security.Cryptography.RandomNumberGenerator.GetInt32(int.MaxValue)
                / (double)int.MaxValue
        );

    /// <summary>
    ///     Pure jitter arithmetic, separated from the random source so a test can drive it
    ///     with a chosen sample instead of a real draw.
    /// </summary>
    /// <param name="baseDelay">Deterministic backoff delay for this attempt.</param>
    /// <param name="sample">A value in [0, 1).</param>
    internal static TimeSpan ScaleReconnectDelay(TimeSpan baseDelay, double sample)
    {
        if (baseDelay <= TimeSpan.Zero)
            return TimeSpan.Zero;

        var factor = ReconnectJitterFloor + ((1.0 - ReconnectJitterFloor) * sample);
        return TimeSpan.FromTicks((long)(baseDelay.Ticks * factor));
    }

    private static ZlinkStreamException ClosedError() =>
        ZlinkStreamConnector.Error(ZlinkStreamErrorCode.Disconnected, "Connector is closed.");

    private static ZlinkStreamError GetPendingDisconnectError(ZlinkStreamError cause)
    {
        return cause.Code == ZlinkStreamErrorCode.Disconnected
            ? cause
            : new ZlinkStreamError(
                ZlinkStreamErrorCode.Disconnected,
                "Connector disconnected.",
                cause.Exception
            );
    }

    private readonly record struct LifecycleSnapshot(
        IZlinkStreamConnection? Connection,
        CancellationTokenSource? SessionCts,
        Task? ReceiveTask,
        Task? HeartbeatTask
    );

    private readonly record struct ActiveConnectStart(Task Task, Action Start);
}
