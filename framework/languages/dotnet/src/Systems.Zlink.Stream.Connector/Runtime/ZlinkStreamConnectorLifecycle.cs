using System.Runtime.ExceptionServices;
using System.Security.Authentication;

namespace Systems.Zlink.Stream.Connector.Runtime;

internal sealed class ZlinkStreamConnectorLifecycle(
    ZlinkStreamConnectorOptions options,
    ZlinkStreamPendingRequests pending,
    ZlinkStreamTaskRunner taskRunner,
    ZlinkStreamConnectorCallbacks callbacks,
    Func<CancellationToken, ValueTask<IZlinkStreamConnection>> connectTransport)
    : IDisposable
{
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
            lock (_gate) return _lastCloseReason;
        }
    }

    private bool IsReentrantCallback =>
        ZlinkStreamCallbackExecutionContext.CurrentWorkerCallbackKindFor(this) is not null;

    public void Dispose()
    {
        _closeCts.Dispose();
        _sessionCts?.Dispose();
    }

    public async ValueTask ConnectAsync(
        Func<CancellationToken, Task> runReceiveLoop,
        Func<CancellationToken, ValueTask> sendHeartbeatPing,
        Action throwIfDisposed,
        CancellationToken cancellationToken)
    {
        Task? waitTask;
        long observedConnectionGeneration;
        ActiveConnectStart? activeConnectStart = null;
        ZlinkStreamConnectionStateChanged? change = null;
        lock (_gate)
        {
            throwIfDisposed();
            if (_state == ZlinkStreamConnectionState.Closed)
                throw new ObjectDisposedException(nameof(ZlinkStreamConnector), "Connector is closed.");

            _runReceiveLoop = runReceiveLoop;
            _sendHeartbeatPing = sendHeartbeatPing;
            observedConnectionGeneration = _connectionGeneration;

            if (_state == ZlinkStreamConnectionState.Connected) return;

            if (_activeConnectTask is not null)
            {
                waitTask = _activeConnectTask;
            }
            else
            {
                change = SetStateLocked(ZlinkStreamConnectionState.Connecting, null);
                activeConnectStart = CreateActiveConnectTask(() => ConnectOnceAsync(cancellationToken));
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
            throw new ObjectDisposedException(nameof(ZlinkStreamConnector), "Connector is closed.");
        }

        lock (_gate)
        {
            if (_state == ZlinkStreamConnectionState.Closed
                && _connectionGeneration == observedConnectionGeneration)
                throw new ObjectDisposedException(nameof(ZlinkStreamConnector), "Connector is closed.");
        }
    }

    public async ValueTask CloseAsync(CancellationToken cancellationToken)
    {
        var isReentrant = IsReentrantCallback;
        Task closeTask;
        TaskCompletionSource? startClose = null;
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
                var change = SetStateLocked(ZlinkStreamConnectionState.Closed, null);

                startClose = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
                _closeTask = RunFullCloseAsync(
                    startClose.Task,
                    snapshot,
                    activeConnectTask,
                    startActiveConnect,
                    change);
            }

            closeTask = _closeTask;
        }

        startClose?.TrySetResult();
        if (!isReentrant) await closeTask.WaitAsync(cancellationToken).ConfigureAwait(false);
    }

    private async Task RunFullCloseAsync(
        Task started,
        LifecycleSnapshot snapshot,
        Task? activeConnectTask,
        Action? startActiveConnect,
        ZlinkStreamConnectionStateChanged? change)
    {
        await started.ConfigureAwait(false);
        using var work = EnterWorker(ZlinkStreamLifecycleWorkKind.CloseCompletion);

        startActiveConnect?.Invoke();
        snapshot.SessionCts?.Cancel();
        Exception? closeException = null;
        try
        {
            await CloseConnectionAsync(snapshot.Connection, CancellationToken.None).ConfigureAwait(false);
        }
        catch (Exception ex)
        {
            closeException = ex;
        }

        pending.FailAll(new ZlinkStreamError(ZlinkStreamErrorCode.Disconnected, "Connector closed."));
        await WaitBackgroundTaskAsync(snapshot.ReceiveTask).ConfigureAwait(false);
        await WaitBackgroundTaskAsync(snapshot.HeartbeatTask).ConfigureAwait(false);
        await WaitBackgroundTaskAsync(activeConnectTask).ConfigureAwait(false);
        snapshot.SessionCts?.Dispose();

        await NotifyStateChangedAsync(change, CancellationToken.None).ConfigureAwait(false);
        if (snapshot.Connection is not null)
            await callbacks.NotifyDisconnectedAsync(ZlinkStreamCloseReason.ClientClose, CancellationToken.None)
                .ConfigureAwait(false);

        if (closeException is not null) ExceptionDispatchInfo.Capture(closeException).Throw();
    }

    public void RecordInbound()
    {
        _heartbeat.RecordInbound();
    }

    public async ValueTask HandleTransportErrorAsync(
        ZlinkStreamError error,
        CancellationToken cancellationToken = default)
    {
        await callbacks.PublishErrorAsync(error, cancellationToken).ConfigureAwait(false);
        await HandleDisconnectAsync(error, cancellationToken).ConfigureAwait(false);
    }

    public ValueTask HandleServerCloseAsync(
        ZlinkStreamCloseReason closeReason,
        string? diagnostic,
        CancellationToken cancellationToken = default)
    {
        var error = new ZlinkStreamError(
            ZlinkStreamErrorCode.Disconnected,
            diagnostic ?? $"Server closed the stream session ({closeReason}).");
        return HandleDisconnectAsync(error, cancellationToken, closeReason);
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
                CancellationToken.None).ConfigureAwait(false);
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
        if (!reconnect.Enabled) return;

        var delay = reconnect.InitialDelay <= reconnect.MaxDelay
            ? reconnect.InitialDelay
            : reconnect.MaxDelay;
        var attempt = 0;
        ZlinkStreamError? lastError = null;
        var transport = ZlinkStreamRuntimeMetrics.TransportLabel(options);

        try
        {
            while (!_closeCts.IsCancellationRequested)
            {
                await Task.Delay(delay, _closeCts.Token).ConfigureAwait(false);
                attempt++;

                try
                {
                    var connection = await OpenConnectionAsync(_closeCts.Token).ConfigureAwait(false);
                    await AttachConnectionAsync(connection, _closeCts.Token).ConfigureAwait(false);
                    ZlinkStreamRuntimeMetrics.RecordReconnect(
                        transport,
                        "connected",
                        "transport_closed");
                    return;
                }
                catch (Exception ex) when (!_closeCts.IsCancellationRequested)
                {
                    lastError = ex is ZlinkStreamException streamException
                        ? streamException.Error
                        : MapConnectException(ex, _closeCts.Token);
                    ZlinkStreamRuntimeMetrics.RecordReconnect(
                        transport,
                        "failed",
                        ReconnectFailureReason(lastError));
                    await callbacks.PublishErrorAsync(lastError, CancellationToken.None)
                        .ConfigureAwait(false);

                    if (reconnect.MaxAttempts is { } maxAttempts && attempt >= maxAttempts)
                    {
                        await TransitionToDisconnectedAsync(lastError, CancellationToken.None).ConfigureAwait(false);
                        throw new ZlinkStreamException(lastError);
                    }

                    delay = NextReconnectDelay(delay, reconnect);
                }
            }
        }
        catch (OperationCanceledException) when (_closeCts.IsCancellationRequested)
        {
            ZlinkStreamRuntimeMetrics.RecordReconnect(
                transport,
                "shutdown",
                "requested");
        }
        catch (ObjectDisposedException) when (_closeCts.IsCancellationRequested)
        {
            // Close won the race after the transport connected but before the
            // reconnect loop could attach it. AttachConnectionAsync already
            // closed that transport; the background reconnect is complete.
        }
    }

    private static string ReconnectFailureReason(ZlinkStreamError error) =>
        error.Code switch
        {
            ZlinkStreamErrorCode.ConnectTimeout => "timeout",
            ZlinkStreamErrorCode.TlsValidationFailed => "tls_failed",
            _ => "connect_failed"
        };

    private async ValueTask<IZlinkStreamConnection> OpenConnectionAsync(CancellationToken cancellationToken)
    {
        using var timeoutCts = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken, _closeCts.Token);
        timeoutCts.CancelAfter(options.ConnectTimeout);

        try
        {
            return await connectTransport(timeoutCts.Token).ConfigureAwait(false);
        }
        catch (OperationCanceledException ex) when (!cancellationToken.IsCancellationRequested &&
                                                    !_closeCts.IsCancellationRequested)
        {
            throw ZlinkStreamConnector.Error(ZlinkStreamErrorCode.ConnectTimeout, "Connect timed out.", ex);
        }
        catch (AuthenticationException ex)
        {
            throw ZlinkStreamConnector.Error(ZlinkStreamErrorCode.TlsValidationFailed, "TLS validation failed.", ex);
        }
    }

    private async ValueTask AttachConnectionAsync(IZlinkStreamConnection connection,
        CancellationToken cancellationToken)
    {
        var runReceiveLoop = _runReceiveLoop
                             ?? throw ZlinkStreamConnector.Error(ZlinkStreamErrorCode.ConfigurationError,
                                 "Receive loop is not configured.");
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
            throw new ObjectDisposedException(nameof(ZlinkStreamConnector), "Connector is closed.");
        }

        oldSnapshot.SessionCts?.Cancel();
        await CloseConnectionAsync(oldSnapshot.Connection, cancellationToken).ConfigureAwait(false);
        oldSnapshot.SessionCts?.Dispose();
        await NotifyStateChangedAsync(change, cancellationToken).ConfigureAwait(false);

        lock (_gate)
        {
            if (!ReferenceEquals(_connection, connection) || !ReferenceEquals(_sessionCts, sessionCts))
            {
                if (_state == ZlinkStreamConnectionState.Closed)
                    throw new ObjectDisposedException(nameof(ZlinkStreamConnector), "Connector is closed.");

                return;
            }

            _connectionGeneration++;
            _receiveTask = taskRunner.Run(
                _ => new ValueTask(RunReceiveLoopGuardedAsync(runReceiveLoop, sessionCts.Token)));
            _heartbeatTask = options.Heartbeat.Enabled
                ? taskRunner.Run(
                    _ => new ValueTask(RunHeartbeatLoopAsync(sessionCts.Token)))
                : null;
        }
    }

    private async Task RunReceiveLoopGuardedAsync(
        Func<CancellationToken, Task> runReceiveLoop,
        CancellationToken cancellationToken)
    {
        using var work = EnterWorker(ZlinkStreamLifecycleWorkKind.Receive);
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
            if (cancellationToken.IsCancellationRequested) return;

            var error = ex is ZlinkStreamException streamException
                ? streamException.Error
                : new ZlinkStreamError(ZlinkStreamErrorCode.FrameDecodeFailed, "Receive loop failed.", ex);
            await HandleTransportErrorAsync(error, CancellationToken.None).ConfigureAwait(false);
            return;
        }

        if (!cancellationToken.IsCancellationRequested)
            await HandleDisconnectAsync(
                new ZlinkStreamError(ZlinkStreamErrorCode.Disconnected, "Connector disconnected."),
                CancellationToken.None).ConfigureAwait(false);
    }

    private async Task RunHeartbeatLoopAsync(CancellationToken cancellationToken)
    {
        using var work = EnterWorker(ZlinkStreamLifecycleWorkKind.Heartbeat);
        await _heartbeat.RunAsync(
                _sendHeartbeatPing,
                HandleTransportErrorAsync,
                cancellationToken)
            .ConfigureAwait(false);
    }

    private async ValueTask HandleDisconnectAsync(
        ZlinkStreamError error,
        CancellationToken cancellationToken,
        ZlinkStreamCloseReason? explicitCloseReason = null)
    {
        LifecycleSnapshot snapshot;
        ZlinkStreamConnectionStateChanged? change;
        ActiveConnectStart? reconnectStart = null;
        lock (_gate)
        {
            if (_state is ZlinkStreamConnectionState.Closed or ZlinkStreamConnectionState.Disconnected) return;

            snapshot = DetachLocked();
            _lastCloseReason = explicitCloseReason ?? MapCloseReason(error);
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
        Capture(() => snapshot.SessionCts?.Cancel());
        try
        {
            await CloseConnectionAsync(snapshot.Connection, CancellationToken.None).ConfigureAwait(false);
        }
        catch (Exception exception)
        {
            closeFailure = exception;
        }

        Capture(() => snapshot.SessionCts?.Dispose());
        await CaptureAsync(() => NotifyStateChangedAsync(change, CancellationToken.None)).ConfigureAwait(false);
        Capture(() => pending.FailAll(GetPendingDisconnectError(error)));
        await CaptureAsync(() => callbacks.NotifyDisconnectedAsync(
                explicitCloseReason ?? MapCloseReason(error),
                CancellationToken.None))
            .ConfigureAwait(false);
        Capture(() => reconnectStart?.Start());

        if (closeFailure is not null && terminalFailures is not null)
            throw new AggregateException([closeFailure, .. terminalFailures]);
        if (closeFailure is not null) ExceptionDispatchInfo.Capture(closeFailure).Throw();
        if (terminalFailures is { Count: 1 }) ExceptionDispatchInfo.Capture(terminalFailures[0]).Throw();
        if (terminalFailures is { Count: > 1 }) throw new AggregateException(terminalFailures);
        return;

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

    private async ValueTask TransitionToDisconnectedAsync(ZlinkStreamError error, CancellationToken cancellationToken)
    {
        ZlinkStreamConnectionStateChanged? change;
        lock (_gate)
        {
            if (_state == ZlinkStreamConnectionState.Closed) return;

            change = SetStateLocked(ZlinkStreamConnectionState.Disconnected, error);
        }

        await NotifyStateChangedAsync(change, cancellationToken).ConfigureAwait(false);
    }

    private static ZlinkStreamCloseReason MapCloseReason(ZlinkStreamError error)
    {
        if (error.Code == ZlinkStreamErrorCode.FrameDecodeFailed)
            return ZlinkStreamCloseReason.ProtocolError;

        if (error.Message.Contains("heartbeat", StringComparison.OrdinalIgnoreCase)
            && error.Message.Contains("timeout", StringComparison.OrdinalIgnoreCase))
            return ZlinkStreamCloseReason.HeartbeatTimeout;

        return ZlinkStreamCloseReason.TransportError;
    }

    private LifecycleSnapshot DetachLocked()
    {
        var snapshot = new LifecycleSnapshot(_connection, _sessionCts, _receiveTask, _heartbeatTask);
        _connection = null;
        _sessionCts = null;
        _receiveTask = null;
        _heartbeatTask = null;
        return snapshot;
    }

    private ZlinkStreamConnectionStateChanged? SetStateLocked(
        ZlinkStreamConnectionState next,
        ZlinkStreamError? error)
    {
        if (_state == next) return null;

        var previous = _state;
        _state = next;
        return new ZlinkStreamConnectionStateChanged(previous, next, error);
    }

    private async ValueTask NotifyStateChangedAsync(
        ZlinkStreamConnectionStateChanged? change,
        CancellationToken cancellationToken)
    {
        if (change is not null)
            await callbacks.NotifyConnectionStateChangedAsync(change, cancellationToken).ConfigureAwait(false);
    }

    private IDisposable EnterWorker(ZlinkStreamLifecycleWorkKind workKind) =>
        ZlinkStreamCallbackExecutionContext.EnterWorker(this, workKind);

    private static async ValueTask CloseConnectionAsync(
        IZlinkStreamConnection? connection,
        CancellationToken cancellationToken)
    {
        if (connection is not null) await connection.CloseAsync(cancellationToken).ConfigureAwait(false);
    }

    private static async ValueTask WaitBackgroundTaskAsync(Task? task)
    {
        if (task is null) return;

        try
        {
            await task.ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
        }
        catch (ZlinkStreamException)
        {
        }
    }

    private ActiveConnectStart CreateActiveConnectTask(Func<Task> run)
    {
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        Task? activeTask = null;
        activeTask = RunActiveConnectTaskAsync(started.Task, run, () => activeTask);
        return new ActiveConnectStart(activeTask, () => started.TrySetResult());
    }

    private async Task RunActiveConnectTaskAsync(
        Task started,
        Func<Task> run,
        Func<Task?> currentTask)
    {
        await started.ConfigureAwait(false);
        using var work = EnterWorker(ZlinkStreamLifecycleWorkKind.ActiveConnect);
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
        if (task is null) return;

        ActiveConnectStart? reconnectStart = null;
        lock (_gate)
        {
            if (!ReferenceEquals(_activeConnectTask, task)) return;

            _activeConnectTask = null;
            _startActiveConnect = null;
            if (_state == ZlinkStreamConnectionState.Reconnecting && !_closeCts.IsCancellationRequested)
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
            TaskScheduler.Default);
    }

    private static ZlinkStreamError MapConnectException(Exception ex, CancellationToken cancellationToken)
    {
        return ex switch
        {
            OperationCanceledException canceled when !cancellationToken.IsCancellationRequested =>
                new ZlinkStreamError(ZlinkStreamErrorCode.ConnectTimeout, "Connect timed out.", canceled),
            AuthenticationException authentication =>
                new ZlinkStreamError(ZlinkStreamErrorCode.TlsValidationFailed, "TLS validation failed.",
                    authentication),
            _ => new ZlinkStreamError(ZlinkStreamErrorCode.Disconnected, "Connect failed.", ex)
        };
    }

    private static TimeSpan NextReconnectDelay(TimeSpan current, ZlinkStreamReconnectOptions options)
    {
        var nextMilliseconds = current.TotalMilliseconds * options.BackoffFactor;
        if (nextMilliseconds >= options.MaxDelay.TotalMilliseconds) return options.MaxDelay;

        return TimeSpan.FromMilliseconds(nextMilliseconds);
    }

    private static ZlinkStreamError GetPendingDisconnectError(ZlinkStreamError cause)
    {
        return cause.Code == ZlinkStreamErrorCode.Disconnected
            ? cause
            : new ZlinkStreamError(ZlinkStreamErrorCode.Disconnected, "Connector disconnected.", cause.Exception);
    }

    private readonly record struct LifecycleSnapshot(
        IZlinkStreamConnection? Connection,
        CancellationTokenSource? SessionCts,
        Task? ReceiveTask,
        Task? HeartbeatTask);

    private readonly record struct ActiveConnectStart(Task Task, Action Start);
}
