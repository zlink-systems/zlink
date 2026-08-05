using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Zlink.Framework.Runtime.Handlers;
using Zlink.Framework.Runtime.Messaging;
using Zlink.Framework.Runtime.Dispatch;

namespace Zlink.Framework.Runtime.Streams;

internal sealed class ZLinkStreamSessionRuntime : IAsyncDisposable
{
    private enum InboundLivenessSignal
    {
        None,
        Application,
        HeartbeatPong
    }

    private readonly ZLinkSessionContext _context;
    private readonly ZLinkMessageFlowTracer _flow;
    private readonly ZLinkStreamSessionLiveness _liveness;
    private readonly ZLinkScopedHandlerInstanceOwner _handlerInstances;
    private IZLinkSession _handler = null!;
    private readonly Action<string> _removeSession;
    private readonly ZLinkFrameworkRuntime _runtime;
    private readonly ZLinkCompletionAdmissionOwner? _completionAdmission;
    private readonly AsyncServiceScope _scope;
    private readonly ZLinkStreamSessionSerialExecutor _serial;
    private readonly IZLinkBackendStreamSocket _socket;
    private readonly string _transport;
    private readonly object _disposeGate = new();
    private readonly object _terminalGate = new();
    private readonly object _transportCloseGate = new();
    private readonly bool _requireConnectionReady;
    private readonly TaskCompletionSource<(string LocalAddr, string RemoteAddr)>
        _connectionReady = new(TaskCreationOptions.RunContinuationsAsynchronously);
    private int _connected;
    private readonly TaskCompletionSource<bool> _completion =
        new(TaskCreationOptions.RunContinuationsAsynchronously);
    private readonly TaskCompletionSource<bool> _transportClosed =
        new(TaskCreationOptions.RunContinuationsAsynchronously);
    private readonly CancellationTokenSource _terminalCallbackStop = new();
    private Task _terminalCallbackCancellation = Task.CompletedTask;
    private bool _terminalCallbackStopDisposed;
    private bool _terminalCallbackStopFinalizing;
    private TerminalClose? _terminalClose;
    private Task? _transportCloseTask;
    private Task? _disposeTask;
    private int _streamMetricActive;
    private int _terminalSucceeded = 1;
    private int _suppressTerminalCallbacks;
    private int _serverDrainClosingSent;

    public static async ValueTask<ZLinkStreamSessionRuntime> CreateAsync(
        IServiceProvider services,
        IZLinkBackendStreamSocket socket,
        RoutingId routingId,
        Type? headerSessionType,
        Action<string> removeSession,
        string transport,
        TimeProvider timeProvider,
        bool actorDispatchEnabled = true,
        ZLinkAsyncSubmitter? sendSubmitter = null,
        bool requireConnectionReady = false,
        ZLinkCompletionAdmissionOwner? completionAdmission = null)
    {
        AsyncServiceScope scope = default;
        var scopeCreated = false;
        ZLinkStreamSessionRuntime? session = null;
        try
        {
            scope = services.CreateAsyncScope();
            scopeCreated = true;
            session = new ZLinkStreamSessionRuntime(
                scope,
                socket,
                routingId,
                removeSession,
                transport,
                timeProvider,
                actorDispatchEnabled,
                sendSubmitter,
                requireConnectionReady,
                completionAdmission);
            session.Initialize(headerSessionType);
            return session;
        }
        catch (Exception initializationFailure)
        {
            var failures = new ZLinkFailureCollector(initializationFailure);
            if (session is not null)
                await failures.CaptureAsync(session.DisposeInitializationAsync).ConfigureAwait(false);
            else if (scopeCreated)
                await failures.CaptureAsync(scope.DisposeAsync).ConfigureAwait(false);
            failures.ThrowIfAny();
            throw new InvalidOperationException("Unreachable after session initialization cleanup.");
        }
    }

    private ZLinkStreamSessionRuntime(
        AsyncServiceScope scope,
        IZLinkBackendStreamSocket socket,
        RoutingId routingId,
        Action<string> removeSession,
        string transport,
        TimeProvider timeProvider,
        bool actorDispatchEnabled,
        ZLinkAsyncSubmitter? sendSubmitter,
        bool requireConnectionReady,
        ZLinkCompletionAdmissionOwner? completionAdmission)
    {
        _scope = scope;
        _socket = socket;
        _transport = transport;
        _removeSession = removeSession;
        _runtime = scope.ServiceProvider.GetRequiredService<ZLinkFrameworkRuntime>();
        _completionAdmission = completionAdmission;
        _requireConnectionReady = requireConnectionReady;
        _handlerInstances = new ZLinkScopedHandlerInstanceOwner(scope.ServiceProvider);
        Stream = new ZLinkManagedStream(socket, routingId, _runtime.Registration.Codecs, transport);
        _flow = new ZLinkMessageFlowTracer(
            _runtime.Registration.DispatchOptions,
            ZLinkMessageFlowTracer.CreateLogger(scope.ServiceProvider.GetService<ILoggerFactory>()),
            _runtime,
            errorSink: _runtime.ErrorSink);
        _liveness = new ZLinkStreamSessionLiveness(timeProvider);
        var handlers = new ZLinkSessionHandlerRegistry(_handlerInstances);
        _context = new ZLinkSessionContext(
            _runtime,
            Stream,
            handlers,
            CloseAsync,
            CloseByProxyAsync,
            actorDispatchEnabled,
            sendSubmitter);
        Handlers = handlers;
        _serial = new ZLinkStreamSessionSerialExecutor(_runtime.ExecutionOwner, _runtime.ErrorSink);
    }

    public ZLinkManagedStream Stream { get; }

    private ZLinkSessionHandlerRegistry Handlers { get; }

    internal void RequestStop() => _serial.RequestStop();

    private void Initialize(Type? headerSessionType)
    {
        Handlers.BindContext(_context);
        _handler = (IZLinkSession)ActivatorUtilities.CreateInstance(
            _scope.ServiceProvider,
            headerSessionType!,
            _context);
        if (!ReferenceEquals(_handler.Context, _context))
            throw new InvalidOperationException(
                $"Session '{_handler.GetType().FullName}' must expose the context provided by the runtime.");
        Handlers.BindSession(_handler);
        Handlers.AddScannedHandlers(_runtime.Registration.ScannedHandlerCatalog.SessionHandlers);
        _handler.Configure();
        Handlers.Bind();
    }

    private async ValueTask DisposeInitializationAsync()
    {
        var failures = new ZLinkFailureCollector();
        await failures.CaptureAsync(_serial.DisposeAsync).ConfigureAwait(false);
        await failures.CaptureAsync(DisposeTerminalCallbackStopAsync).ConfigureAwait(false);
        await failures.CaptureAsync(_handlerInstances.DisposeAsync).ConfigureAwait(false);
        await failures.CaptureAsync(_scope.DisposeAsync).ConfigureAwait(false);
        failures.ThrowIfAny();
    }

    internal async ValueTask DisposeUncommittedAsync()
    {
        var failures = new ZLinkFailureCollector();
        await failures.CaptureAsync(_serial.DisposeAsync).ConfigureAwait(false);
        await failures.CaptureAsync(DisposeTerminalCallbackStopAsync).ConfigureAwait(false);
        await failures.CaptureAsync(() => _context.CleanupAsync(CancellationToken.None)).ConfigureAwait(false);
        await failures.CaptureAsync(_handlerInstances.DisposeAsync).ConfigureAwait(false);
        await failures.CaptureAsync(_scope.DisposeAsync).ConfigureAwait(false);
        failures.ThrowIfAny();
    }

    public ValueTask DisposeAsync()
    {
        lock (_disposeGate)
            return new ValueTask(_disposeTask ??= DisposeCoreAsync());
    }

    private async Task DisposeCoreAsync()
    {
        var disposeOwnsDisconnect = ClaimCloseForDisposal();
        var failures = new List<Exception>();
        await CaptureAsync(_serial.DisposeAsync).ConfigureAwait(false);
        if (disposeOwnsDisconnect)
        {
            if (await TryCloseTransportAsync().ConfigureAwait(false))
                RecordStreamClosedMetric(Volatile.Read(ref _terminalClose)!.Reason);
            else
                MarkTerminalFailed();
            if (Volatile.Read(ref _suppressTerminalCallbacks) == 0)
                await CaptureAsync(InvokeDisconnectedLifecycleAsync).ConfigureAwait(false);
        }
        await CaptureAsync(DisposeTerminalCallbackStopAsync).ConfigureAwait(false);
        await CaptureAsync(() => _context.CleanupAsync(CancellationToken.None)).ConfigureAwait(false);
        Capture(() => _removeSession(Stream.SessionId));
        await CaptureAsync(_handlerInstances.DisposeAsync).ConfigureAwait(false);
        await CaptureAsync(_scope.DisposeAsync).ConfigureAwait(false);
        if (failures.Count > 0) MarkTerminalFailed();
        _completion.TrySetResult(
            failures.Count == 0 && Volatile.Read(ref _terminalSucceeded) != 0);
        if (failures.Count == 1)
            System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(failures[0]).Throw();
        if (failures.Count > 1) throw new AggregateException(failures);
        return;

        async ValueTask CaptureAsync(Func<ValueTask> cleanup)
        {
            try
            {
                await cleanup().ConfigureAwait(false);
            }
            catch (Exception exception)
            {
                failures.Add(exception);
            }
        }

        void Capture(Action cleanup)
        {
            try
            {
                cleanup();
            }
            catch (Exception exception)
            {
                failures.Add(exception);
            }
        }
    }

    public ZLinkSerialPostAdmission EnqueueConnected(
        string localAddr,
        string remoteAddr)
    {
        _connectionReady.TrySetResult((localAddr, remoteAddr));
        return _serial.EnqueueControl(
            cancellationToken => MarkConnectedAsync(localAddr, remoteAddr, cancellationToken));
    }

    public void EnqueuePacket(
        Message header,
        Message payload,
        ZLinkInboundDispatchLease? inboundDispatchLease = null)
    {
        if (TryEnqueuePacket(header, payload, inboundDispatchLease)
            == ZLinkSerialPostAdmission.Accepted)
            return;
        DisposeRejectedPacket(header, payload, inboundDispatchLease);
    }

    public ZLinkSerialPostAdmission TryEnqueuePacket(
        Message header,
        Message payload,
        ZLinkInboundDispatchLease? inboundDispatchLease = null)
    {
        var signal = ClassifyInboundLiveness(header, payload);
        var admission = _serial.EnqueueApplication(
            cancellationToken => DispatchPacketAsync(
                header,
                payload,
                inboundDispatchLease,
                cancellationToken));
        if (admission == ZLinkSerialPostAdmission.Accepted)
            ApplyInboundLiveness(signal);
        return admission;
    }

    public ZLinkSerialPostAdmission TryEnqueueControlPacket(
        Message header,
        Message payload)
    {
        var signal = ClassifyInboundLiveness(header, payload);
        var admission = _serial.EnqueueControl(
            cancellationToken => DispatchPacketAsync(
                header,
                payload,
                inboundDispatchLease: null,
                cancellationToken: cancellationToken));
        if (admission == ZLinkSerialPostAdmission.Accepted)
            ApplyInboundLiveness(signal);
        return admission;
    }

    public void CheckLiveness()
    {
        if (IsClosing) return;
        switch (_liveness.Evaluate())
        {
            case ZLinkStreamLivenessDecision.None:
                return;
            case ZLinkStreamLivenessDecision.SendHeartbeat:
                try
                {
                    ZLinkStreamControlFrames.SendHeartbeatPing(Stream);
                    _liveness.RecordHeartbeatPing();
                }
                catch (Exception error)
                {
                    TryScheduleTerminal(
                        "transport_error",
                        () => CloseForTransportErrorAsync(error));
                }
                return;
            case ZLinkStreamLivenessDecision.IdleTimeout:
                TryScheduleTerminal(
                    "idle_timeout",
                    () => CloseForLivenessTimeoutAsync(
                        ZLinkStreamSessionClosingCodec.EncodeIdleTimeout()));
                return;
            case ZLinkStreamLivenessDecision.HeartbeatTimeout:
                TryScheduleTerminal(
                    "heartbeat_timeout",
                    () => CloseForLivenessTimeoutAsync(
                        ZLinkStreamSessionClosingCodec.EncodeHeartbeatTimeout()));
                return;
            default:
                throw new InvalidOperationException("Unknown STREAM liveness decision.");
        }
    }

    public void EnqueueDisconnected(ZLinkStreamError error)
    {
        TryScheduleTerminal("transport_error", () => MarkDisconnectedAsync(error));
    }

    public async ValueTask CloseAsync()
    {
        if (!TryScheduleTerminal(
                "client_close",
                () => CompleteAfterTransportClosedAsync(notifyDisconnected: true)))
            return;

        await CloseTransportAsync().ConfigureAwait(false);
    }

    public async ValueTask CloseByProxyAsync(CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (!TryScheduleTerminal(
                "client_close",
                () => CompleteAfterTransportClosedAsync(notifyDisconnected: false)))
            return;

        await CloseTransportAsync().WaitAsync(cancellationToken).ConfigureAwait(false);
    }

    internal async ValueTask<bool> CloseForDrainAsync(CancellationToken cancellationToken)
    {
        var scheduled = TryScheduleTerminal("server_drain", CloseForDrainCoreAsync);
        if (!scheduled && !IsClosing) return false;
        try
        {
            return await _completion.Task.WaitAsync(cancellationToken).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            return false;
        }
        catch
        {
            return false;
        }
    }

    private async ValueTask CloseForDrainCoreAsync()
    {
        var transportClosed = false;
        try
        {
            SubmitServerDrainClosing();

            transportClosed = await TryCloseTransportAsync().ConfigureAwait(false);
        }
        finally
        {
            _transportClosed.TrySetResult(transportClosed);
            await CompleteSessionAsync(null, true).ConfigureAwait(false);
        }
    }

    internal void RequestForceStopForDrain()
    {
        SubmitServerDrainClosing();
        ClaimCloseForDisposal();
        Volatile.Write(ref _suppressTerminalCallbacks, 1);
        RequestTerminalCallbackStop();
        _serial.ForceStop();
        MarkTerminalFailed();
        _completion.TrySetResult(false);
    }

    private void SubmitServerDrainClosing()
    {
        if (Interlocked.Exchange(ref _serverDrainClosingSent, 1) != 0) return;
        try
        {
            var payload = ZLinkStreamSessionClosingCodec.EncodeServerDrain();
            ZLinkStreamFrameWriter.Write(
                Stream,
                ZLinkStreamSessionClosingCodec.CreateHeader(),
                payload.AsMemory(),
                "Could not submit the session-closing control packet.");
        }
        catch
        {
            MarkTerminalFailed();
        }
    }

    private async ValueTask MarkConnectedAsync(
        string localAddr,
        string remoteAddr,
        CancellationToken cancellationToken)
    {
        Stream.UpdateAddresses(localAddr, remoteAddr);
        if (Interlocked.Exchange(ref _connected, 1) != 0) return;

        RecordStreamOpenedMetric();
        await InvokeConnectedLifecycleAsync(cancellationToken).ConfigureAwait(false);
    }

    private async ValueTask DispatchPacketAsync(
        Message header,
        Message payload,
        ZLinkInboundDispatchLease? inboundDispatchLease,
        CancellationToken cancellationToken)
    {
        inboundDispatchLease?.StartDispatch();
        try
        {
            using (header)
            using (payload)
            {
                if (IsClosing) return;
                if (!await EnsureConnectedAsync(cancellationToken).ConfigureAwait(false))
                    return;
                ZlinkStreamHeader decoded;
                try
                {
                    decoded = ZLinkStreamProtocolDefaults.DecodeHeader(header.AsReadOnlyMemory());
                    if (decoded.Kind == ZlinkStreamMessageKind.Control)
                    {
                        ZLinkStreamControlFrames.Dispatch(Stream, decoded, payload.AsReadOnlyMemory());
                        return;
                    }
                }
                catch (Exception protocolError)
                {
                    await CloseForProtocolErrorAsync(protocolError).ConfigureAwait(false);
                    return;
                }

                using var completionPermit = decoded.RequestSeq.HasValue
                    && _completionAdmission is not null
                    ? await _completionAdmission.AcquireResponderAsync(cancellationToken)
                        .ConfigureAwait(false)
                    : null;

                using var currentFlow = ZLinkFlowContext.Enter(
                    decoded.FlowId,
                    decoded.FlowOrigin is { } streamOrigin ? (ZLinkFlowOrigin)(byte)streamOrigin : null,
                    _flow.CaptureEnabled,
                    ZLinkFlowOrigin.Inbound);

                if (_flow.Enabled(ZLinkMessageFlowOutcome.Received))
                    _flow.Trace(new ZLinkMessageFlowEvent(
                        ZLinkMessageFlowOutcome.Received,
                        ZLinkDispatchErrorSurface.StreamSession,
                        decoded.RequestSeq.HasValue
                            ? ZLinkDispatchMessageKind.Request
                            : ZLinkDispatchMessageKind.Send,
                        decoded.Name,
                        CorrelationId: decoded.CorrelationId));

                var dispatch = _context.EnterDispatch(decoded, completionPermit);
                try
                {
                    await _handler.OnDispatchAsync(
                        dispatch,
                        ZLinkStreamPacketPayloadCodec.DecodeMessage(
                            decoded,
                            payload,
                            _runtime.Registration.Codecs,
                            _runtime.Registration.StreamCompressionCodec),
                        cancellationToken);

                    if (!decoded.RequestSeq.HasValue
                        && _flow.Enabled(ZLinkMessageFlowOutcome.Dispatched))
                        _flow.Trace(new ZLinkMessageFlowEvent(
                            ZLinkMessageFlowOutcome.Dispatched,
                            ZLinkDispatchErrorSurface.StreamSession,
                            ZLinkDispatchMessageKind.Send,
                            decoded.Name,
                            CorrelationId: decoded.CorrelationId));
                }
                catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
                {
                }
                catch (Exception ex)
                {
                    if (decoded.RequestSeq.HasValue && dispatch.TryClaimReply())
                    {
                        try
                        {
                            await _context.ReplyErrorAsync(decoded, ex, cancellationToken)
                                .ConfigureAwait(false);
                        }
                        catch (Exception replyException) when (IsClosedReplyFailure(replyException))
                        {
                        }
                    }
                }
                finally
                {
                    _context.ExitDispatch();
                }
            }
        }
        finally
        {
            inboundDispatchLease?.Dispose();
        }
    }

    private async ValueTask MarkDisconnectedAsync(ZLinkStreamError error)
    {
        _transportClosed.TrySetResult(true);
        await CompleteSessionAsync(error, true);
    }

    private async ValueTask CloseForProtocolErrorAsync(Exception error)
    {
        TryScheduleTerminal(
            "protocol_error",
            () => CloseForProtocolErrorCoreAsync(error));
        await ValueTask.CompletedTask;
    }

    private async ValueTask CloseForProtocolErrorCoreAsync(Exception error)
    {
        try
        {
            ZLinkStreamFrameWriter.Write(
                Stream,
                ZLinkStreamSessionClosingCodec.CreateHeader(),
                ZLinkStreamSessionClosingCodec.EncodeProtocolError().AsMemory(),
                "Could not submit the protocol-error session-closing control packet.");
        }
        catch
        {
        }
        _ = await TryCloseTransportAsync().ConfigureAwait(false);
        await CompleteSessionAsync(
                new ZLinkStreamError(
                    ZLinkStreamSessionError.Internal,
                    error.Message),
                notifyDisconnected: true)
            .ConfigureAwait(false);
    }

    private async ValueTask CloseForLivenessTimeoutAsync(byte[] payload)
    {
        try
        {
            ZLinkStreamFrameWriter.Write(
                Stream,
                ZLinkStreamSessionClosingCodec.CreateHeader(),
                payload.AsMemory(),
                "Could not submit the liveness session-closing control packet.");
        }
        catch
        {
        }
        _ = await TryCloseTransportAsync().ConfigureAwait(false);
        await CompleteSessionAsync(null, notifyDisconnected: true).ConfigureAwait(false);
    }

    private async ValueTask CloseForTransportErrorAsync(Exception error)
    {
        _ = await TryCloseTransportAsync().ConfigureAwait(false);

        await CompleteSessionAsync(
                new ZLinkStreamError(
                    ZLinkStreamSessionError.TransportError,
                    error.Message),
                notifyDisconnected: true)
            .ConfigureAwait(false);
    }

    private async ValueTask CompleteAfterTransportClosedAsync(bool notifyDisconnected)
    {
        _ = await _transportClosed.Task.ConfigureAwait(false);
        await CompleteSessionAsync(null, notifyDisconnected).ConfigureAwait(false);
    }

    internal async ValueTask ForceCloseForShutdownAsync()
    {
        ClaimCloseForDisposal();
        Volatile.Write(ref _suppressTerminalCallbacks, 1);
        RequestTerminalCallbackStop();
        _serial.ForceStop();
        MarkTerminalFailed();
        _completion.TrySetResult(false);
        if (await TryCloseTransportAsync().ConfigureAwait(false))
            RecordStreamClosedMetric(Volatile.Read(ref _terminalClose)!.Reason);
    }

    internal void ConfirmNodeTransportDisposed()
    {
        RecordStreamClosedMetric(Volatile.Read(ref _terminalClose)?.Reason ?? "transport_error");
    }

    private async ValueTask CompleteSessionAsync(
        ZLinkStreamError? error,
        bool notifyDisconnected)
    {
        if (await _transportClosed.Task.ConfigureAwait(false))
            RecordStreamClosedMetric(Volatile.Read(ref _terminalClose)!.Reason);
        else
            MarkTerminalFailed();

        Exception? callbackFailure = null;
        if (Volatile.Read(ref _suppressTerminalCallbacks) == 0
            && error is { } streamError)
            try
            {
                using var flow = ZLinkFlowContext.Enter(
                    null,
                    null,
                    _flow.CaptureEnabled,
                    ZLinkFlowOrigin.Lifecycle);
                await InvokeTerminalCallbackAsync(
                        cancellationToken => _handler.OnErrorAsync(streamError, cancellationToken))
                    .ConfigureAwait(false);
            }
            catch (Exception exception)
            {
                callbackFailure = exception;
            }

        if (notifyDisconnected && Volatile.Read(ref _suppressTerminalCallbacks) == 0)
            try
            {
                await InvokeDisconnectedLifecycleAsync().ConfigureAwait(false);
            }
            catch (Exception exception)
            {
                MarkTerminalFailed();
                callbackFailure = callbackFailure is null
                    ? exception
                    : new AggregateException(callbackFailure, exception);
            }

        ZLinkUnawaitedSubmit.Observe(
            DisposeAsync(),
            $"stream-session-dispose:{Stream.SessionId}",
            _runtime.ErrorSink);

        if (callbackFailure is not null)
        {
            MarkTerminalFailed();
            System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(callbackFailure).Throw();
        }
    }

    private bool IsClosing => Volatile.Read(ref _terminalClose) is not null;

    private bool TryScheduleTerminal(string reason, Func<ValueTask> finalWork)
    {
        var rejected = false;
        lock (_terminalGate)
        {
            if (_terminalClose is not null) return false;

            _terminalClose = new TerminalClose(reason, DisposeOwnsClose: false);
            if (_serial.EnqueueFinal(finalWork)) return true;

            _terminalClose = new TerminalClose(reason, DisposeOwnsClose: true);
            rejected = true;
        }

        if (rejected)
        {
            MarkTerminalFailed();
            ZLinkUnawaitedSubmit.Observe(
                DisposeAsync(),
                $"stream-session-rejected-terminal-dispose:{Stream.SessionId}",
                _runtime.ErrorSink);
        }
        return false;
    }

    private bool ClaimCloseForDisposal()
    {
        lock (_terminalGate)
        {
            if (_terminalClose is { } terminalClose) return terminalClose.DisposeOwnsClose;
            _terminalClose = new TerminalClose("transport_error", DisposeOwnsClose: true);
            return true;
        }
    }

    private static InboundLivenessSignal ClassifyInboundLiveness(
        Message header,
        Message payload)
    {
        try
        {
            var headerBytes = header.AsReadOnlySpan();
            if (headerBytes.Length < 2) return InboundLivenessSignal.None;
            var kind = (ZlinkStreamMessageKind)headerBytes[1];
            if (kind != ZlinkStreamMessageKind.Control)
                return InboundLivenessSignal.Application;

            var decoded = ZLinkStreamProtocolDefaults.DecodeHeader(header.AsReadOnlyMemory());
            if (payload.AsReadOnlyMemory().Length == 0
                && ZLinkStreamControlFrames.IsHeartbeatPong(decoded))
                return InboundLivenessSignal.HeartbeatPong;
        }
        catch
        {
        }

        return InboundLivenessSignal.None;
    }

    private void ApplyInboundLiveness(InboundLivenessSignal signal)
    {
        switch (signal)
        {
            case InboundLivenessSignal.Application:
                _liveness.RecordApplicationInbound();
                break;
            case InboundLivenessSignal.HeartbeatPong:
                _liveness.RecordHeartbeatPong();
                break;
        }
    }

    private static void DisposeRejectedPacket(
        Message header,
        Message payload,
        ZLinkInboundDispatchLease? inboundDispatchLease)
    {
        try
        {
            header.Dispose();
            payload.Dispose();
        }
        finally
        {
            inboundDispatchLease?.Dispose();
        }
    }

    private async ValueTask<bool> EnsureConnectedAsync(CancellationToken cancellationToken)
    {
        if (_requireConnectionReady
            && (string.IsNullOrWhiteSpace(Stream.LocalAddr)
                || string.IsNullOrWhiteSpace(Stream.RemoteAddr)))
        {
            try
            {
                var metadata = await _connectionReady.Task.WaitAsync(
                        _runtime.Registration.DefaultRequestTimeout,
                        cancellationToken)
                    .ConfigureAwait(false);
                await MarkConnectedAsync(
                        metadata.LocalAddr,
                        metadata.RemoteAddr,
                        cancellationToken)
                    .ConfigureAwait(false);
            }
            catch (TimeoutException timeout)
            {
                TryScheduleTerminal(
                    "connection_metadata_timeout",
                    () => CloseForTransportErrorAsync(timeout));
                return false;
            }
        }

        if (string.IsNullOrWhiteSpace(Stream.LocalAddr)
            || string.IsNullOrWhiteSpace(Stream.RemoteAddr))
            return !_requireConnectionReady;

        if (Interlocked.CompareExchange(ref _connected, 1, 0) != 0) return true;

        RecordStreamOpenedMetric();
        await InvokeConnectedLifecycleAsync(cancellationToken).ConfigureAwait(false);
        return true;
    }

    private async ValueTask InvokeConnectedLifecycleAsync(CancellationToken cancellationToken)
    {
        using var flow = ZLinkFlowContext.Enter(
            null,
            null,
            _flow.CaptureEnabled,
            ZLinkFlowOrigin.Lifecycle);
        await _handler.OnConnectedAsync(cancellationToken).ConfigureAwait(false);
    }

    private async ValueTask InvokeDisconnectedLifecycleAsync()
    {
        using var flow = ZLinkFlowContext.Enter(
            null,
            null,
            _flow.CaptureEnabled,
            ZLinkFlowOrigin.Lifecycle);
        await InvokeTerminalCallbackAsync(
                _handler.OnDisconnectedAsync)
            .ConfigureAwait(false);
    }

    private async ValueTask<bool> TryCloseTransportAsync()
    {
        try
        {
            await CloseTransportAsync().ConfigureAwait(false);
            return true;
        }
        catch
        {
            return false;
        }
    }

    private Task CloseTransportAsync()
    {
        TaskCompletionSource? owner = null;
        Task closeTask;
        lock (_transportCloseGate)
        {
            if (_transportCloseTask is not null) return _transportCloseTask;
            if (_transportClosed.Task.IsCompletedSuccessfully
                && _transportClosed.Task.Result)
                return Task.CompletedTask;
            owner = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            closeTask = _transportCloseTask = owner.Task;
        }

        _ = CloseTransportCoreAsync(owner);
        return closeTask;
    }

    private async Task CloseTransportCoreAsync(TaskCompletionSource completion)
    {
        try
        {
            await Stream.CloseAsync().ConfigureAwait(false);
            _transportClosed.TrySetResult(true);
            completion.TrySetResult();
        }
        catch (Exception exception)
        {
            _transportClosed.TrySetResult(false);
            completion.TrySetException(exception);
        }
    }

    private async ValueTask InvokeTerminalCallbackAsync(
        Func<CancellationToken, ValueTask> callback)
    {
        _terminalCallbackStop.Token.ThrowIfCancellationRequested();
        var operation = callback(_terminalCallbackStop.Token);
        if (operation.IsCompletedSuccessfully) return;

        var task = operation.AsTask();
        try
        {
            await task.WaitAsync(_terminalCallbackStop.Token).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (_terminalCallbackStop.IsCancellationRequested)
        {
            try
            {
                await task.ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (_terminalCallbackStop.IsCancellationRequested)
            {
            }
        }
    }

    private void RequestTerminalCallbackStop()
    {
        lock (_terminalGate)
        {
            if (_terminalCallbackStopDisposed
                || _terminalCallbackStopFinalizing
                || _terminalCallbackStop.IsCancellationRequested)
                return;
            _terminalCallbackCancellation = _terminalCallbackStop.CancelAsync();
        }
    }

    private async ValueTask DisposeTerminalCallbackStopAsync()
    {
        Task cancellation;
        lock (_terminalGate)
        {
            _terminalCallbackStopFinalizing = true;
            cancellation = _terminalCallbackCancellation;
        }
        try
        {
            await cancellation.ConfigureAwait(false);
        }
        finally
        {
            lock (_terminalGate)
            {
                if (!_terminalCallbackStopDisposed)
                {
                    _terminalCallbackStop.Dispose();
                    _terminalCallbackStopDisposed = true;
                }
            }
        }
    }

    private void MarkTerminalFailed()
    {
        Volatile.Write(ref _terminalSucceeded, 0);
    }

    private void RecordStreamOpenedMetric()
    {
        if (Interlocked.Exchange(ref _streamMetricActive, 1) == 0)
            ZLinkRuntimeMetrics.RecordStreamOpened(_transport);
    }

    private void RecordStreamClosedMetric(string reason)
    {
        if (Interlocked.Exchange(ref _streamMetricActive, 0) != 0)
            ZLinkRuntimeMetrics.RecordStreamClosed(_transport, reason);
    }

    private static bool IsClosedReplyFailure(Exception exception)
    {
        return exception is ObjectDisposedException
                   or ZlinkCloseException
               || exception is ZlinkSubmitException
               {
                   Result: ZlinkSubmitException.ErrorCode.NotConnected
                   or ZlinkSubmitException.ErrorCode.Terminated
                   or ZlinkSubmitException.ErrorCode.InvalidHandle
               };
    }

    private sealed record TerminalClose(string Reason, bool DisposeOwnsClose);
}
