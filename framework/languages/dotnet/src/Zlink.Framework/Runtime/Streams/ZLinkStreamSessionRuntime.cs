using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Zlink.Framework.Runtime.Handlers;
using Zlink.Framework.Runtime.Messaging;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Execution;

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
    private readonly AsyncServiceScope _scope;
    private readonly ZLinkSessionSerialExecutor _serial;
    private readonly IZLinkBackendStreamSocket _socket;
    private readonly string _transport;
    private readonly TimeProvider _timeProvider;
    private readonly ZLinkStateLane _lane = new();
    private readonly HashSet<ActorBindingReplacementIdentity>
        _receivedBindingReplacements = [];
    private readonly Dictionary<ActorBindingReplacementIdentity, ITimer>
        _replacementCloseTimers = [];
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
    private int _applicationDispatchClosed;
    private int _actorBindingReplacementClosing;

    public static async ValueTask<ZLinkStreamSessionRuntime> CreateAsync(
        IServiceProvider services,
        IZLinkBackendStreamSocket socket,
        RoutingId routingId,
        Type? headerSessionType,
        Action<string> removeSession,
        string transport,
        TimeProvider timeProvider,
        bool actorDispatchEnabled = true,
        bool requireConnectionReady = false)
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
                requireConnectionReady);
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
        bool requireConnectionReady)
    {
        _scope = scope;
        _socket = socket;
        _transport = transport;
        _timeProvider = timeProvider;
        _removeSession = removeSession;
        _runtime = scope.ServiceProvider.GetRequiredService<ZLinkFrameworkRuntime>();
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
            actorDispatchEnabled);
        _context.SessionRuntime = this;
        Handlers = handlers;
        _serial = new ZLinkSessionSerialExecutor(_runtime.ExecutionOwner, _runtime.ErrorSink);
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
        return new ValueTask(AwaitStateLane(_lane.RunAsync(
            () => _disposeTask ??= StartDisposeCore())));
    }

    private Task StartDisposeCore()
    {
        if (ExecutionContext.IsFlowSuppressed())
            return Task.Run(DisposeCoreAsync, CancellationToken.None);

        using (ExecutionContext.SuppressFlow())
            return Task.Run(DisposeCoreAsync, CancellationToken.None);
    }

    private async Task DisposeCoreAsync()
    {
        DisposeReplacementCloseTimers();
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
        return _serial.ExecuteControl(
            cancellationToken => MarkConnectedAsync(localAddr, remoteAddr, cancellationToken));
    }

    public void EnqueuePacket(
        Message header,
        Message payload)
    {
        if (TryEnqueuePacket(header, payload)
            == ZLinkSerialPostAdmission.Accepted)
            return;
        DisposeRejectedPacket(header, payload);
    }

    public ZLinkSerialPostAdmission TryEnqueuePacket(
        Message header,
        Message payload,
        ZLinkApplicationJobQueueLease? applicationJobAdmission = null,
        IDisposable? payloadOwner = null)
    {
        if (Volatile.Read(ref _applicationDispatchClosed) != 0)
            return ZLinkSerialPostAdmission.Closed;
        var signal = ClassifyInboundLiveness(header, payload);
        var admission = _serial.ExecuteApplication(
            async cancellationToken =>
            {
                using var payloadOwnerScope = payloadOwner;
                using var admissionScope =
                    applicationJobAdmission is { } admission
                        ? ZLinkApplicationJobQueueInvocation.Enter(admission)
                        : null;
                await DispatchPacketAsync(
                        header,
                        payload,
                        cancellationToken)
                    .ConfigureAwait(false);
            },
            Math.Max(payload.Size, 0),
            Math.Max(header.Size, 0),
            applicationJobAdmission is not null);
        if (admission == ZLinkSerialPostAdmission.Accepted)
            ApplyInboundLiveness(signal);
        return admission;
    }

    internal bool TryEnqueueActorBindingReplaced(
        string actorId,
        RoutingId sessionOwnerNodeRid,
        ulong sessionOwnerNodeGeneration,
        string sessionOwnerId,
        ulong sessionOwnerLeaseGeneration,
        RoutingId sessionRid,
        ulong retiredBindingGeneration,
        string bindingToken)
    {
        var identity = new ActorBindingReplacementIdentity(
            actorId,
            sessionOwnerNodeRid,
            sessionOwnerNodeGeneration,
            sessionOwnerId,
            sessionOwnerLeaseGeneration,
            sessionRid,
            retiredBindingGeneration,
            bindingToken);
        if (!AwaitStateLane(_lane.RunAsync(() =>
        {
            if (!_receivedBindingReplacements.Add(identity))
                return false;
            _serial.CloseApplicationAdmission();
            Interlocked.Exchange(ref _applicationDispatchClosed, 1);
            Interlocked.Exchange(ref _actorBindingReplacementClosing, 1);
            return true;
        })))
            return true;

        if (_serial.ExecuteInfrastructure(
                () => InvokeActorBindingReplacedAsync(identity)))
            return true;

        ScheduleRetiredSessionClose(identity, force: true);
        return false;
    }

    public ZLinkSerialPostAdmission TryEnqueueControlPacket(
        Message header,
        Message payload,
        ZLinkApplicationJobQueueLease? applicationJobAdmission = null,
        IDisposable? payloadOwner = null)
    {
        var signal = ClassifyInboundLiveness(header, payload);
        var admission = _serial.ExecuteControl(
            async cancellationToken =>
            {
                using var payloadOwnerScope = payloadOwner;
                using var admissionScope =
                    applicationJobAdmission is { } admission
                        ? ZLinkApplicationJobQueueInvocation.Enter(admission)
                        : null;
                await DispatchPacketAsync(
                        header,
                        payload,
                        cancellationToken: cancellationToken)
                    .ConfigureAwait(false);
            },
            Math.Max(payload.Size, 0),
            Math.Max(header.Size, 0),
            applicationJobAdmission is not null);
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
                        ZlinkStreamSessionClosingCodec.EncodeIdleTimeout()));
                return;
            case ZLinkStreamLivenessDecision.HeartbeatTimeout:
                TryScheduleTerminal(
                    "heartbeat_timeout",
                    () => CloseForLivenessTimeoutAsync(
                        ZlinkStreamSessionClosingCodec.EncodeHeartbeatTimeout()));
                return;
            default:
                throw new InvalidOperationException("Unknown STREAM liveness decision.");
        }
    }

    public void EnqueueDisconnected(ZLinkStreamError error)
    {
        TryScheduleTerminal(
            "transport_error",
            () => MarkDisconnectedAsync(error),
            recordTransportClosedOnTerminalCollision: true);
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
            var payload = ZlinkStreamSessionClosingCodec.EncodeServerDrain();
            ZLinkStreamFrameWriter.Write(
                Stream,
                ZlinkStreamSessionClosingCodec.CreateHeader(),
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
        CancellationToken cancellationToken)
    {
        using (header)
            using (payload)
            {
                if (!await EnsureConnectedAsync(cancellationToken).ConfigureAwait(false))
                    return;
                ZlinkStreamHeader decoded;
                try
                {
                    decoded = ZLinkStreamProtocolDefaults.DecodeHeader(
                        header.AsReadOnlyMemory(),
                        _flow.CaptureEnabled);
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

                var dispatch = _context.EnterDispatch(decoded);
                try
                {
                    var decodedPayload = ZLinkStreamPacketPayloadCodec.DecodeMessage(
                        decoded,
                        payload,
                        _runtime.Registration.Codecs,
                        _runtime.Registration.StreamCompressionCodec);
                    await ZLinkApplicationJobQueueInvocation
                        .EnsureQueuedPermitAsync(cancellationToken)
                        .ConfigureAwait(false);
                    ZLinkApplicationJobQueueInvocation.ReleaseForHandlerStart();
                    await _handler.OnDispatchAsync(
                        dispatch,
                        decodedPayload,
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
                ZlinkStreamSessionClosingCodec.CreateHeader(),
                ZlinkStreamSessionClosingCodec.EncodeProtocolError().AsMemory(),
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
                ZlinkStreamSessionClosingCodec.CreateHeader(),
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

    private bool IsClosing => Volatile.Read(ref _terminalClose) is not null
                              || Volatile.Read(
                                  ref _actorBindingReplacementClosing) != 0;

    private async ValueTask InvokeActorBindingReplacedAsync(
        ActorBindingReplacementIdentity identity)
    {
        var deadline = false;
        using var callbackDeadline = CancellationTokenSource
            .CreateLinkedTokenSource(_terminalCallbackStop.Token);
        callbackDeadline.CancelAfter(
            _runtime.Registration.SessionReplacementCallbackTimeout);
        try
        {
            await ZLinkApplicationJobQueueInvocation
                .EnsureQueuedPermitAsync(callbackDeadline.Token)
                .ConfigureAwait(false);
            ZLinkApplicationJobQueueInvocation.ReleaseForHandlerStart();
            var operation = _handler.OnActorBindingReplacedAsync(
                identity.ActorId,
                callbackDeadline.Token);
            if (!operation.IsCompletedSuccessfully)
                await operation.AsTask().WaitAsync(callbackDeadline.Token)
                    .ConfigureAwait(false);
        }
        catch (OperationCanceledException)
            when (callbackDeadline.IsCancellationRequested)
        {
            deadline = true;
        }
        catch (Exception failure)
        {
            _runtime.TryReportUnhandledCallbackException(failure);
        }

        // A callback failure still has a terminal callback boundary. A
        // callback deadline is a force-close boundary and does not extend the
        // fixed grace window.
        ScheduleRetiredSessionClose(identity, force: deadline);
    }

    private void ScheduleRetiredSessionClose(
        ActorBindingReplacementIdentity identity,
        bool force)
    {
        if (force)
        {
            _ = CloseRetiredSessionIfExactAsync(identity);
            return;
        }

        AwaitStateLane(_lane.RunAsync(() =>
        {
            if (_replacementCloseTimers.ContainsKey(identity))
                return;
            var timer = _timeProvider.CreateTimer(
                static state =>
                {
                    var closure = (ReplacementCloseTimerState)state!;
                    closure.Owner.OnReplacementCloseTimer(closure.Identity);
                },
                new ReplacementCloseTimerState(this, identity),
                TimeSpan.FromMilliseconds(100),
                Timeout.InfiniteTimeSpan);
            _replacementCloseTimers.Add(identity, timer);
        }));
    }

    private void OnReplacementCloseTimer(ActorBindingReplacementIdentity identity)
    {
        var timer = AwaitStateLane(_lane.RunAsync(() =>
        {
            if (_replacementCloseTimers.Remove(identity, out var timer))
                return timer;
            return null;
        }));
        timer?.Dispose();
        _ = CloseRetiredSessionIfExactAsync(identity);
    }

    private ValueTask CloseRetiredSessionIfExactAsync(
        ActorBindingReplacementIdentity identity)
    {
        if (!_context.Runtime.TryGetSessionActorBinding(
                identity.ActorId,
                identity.BindingToken,
                out var current)
            || !ReferenceEquals(current.Context, _context)
            || current.ActorRef.SessionRid != identity.SessionRid
            || current.BindingGeneration != identity.RetiredBindingGeneration
            || current.SessionOwnerNodeRid != identity.SessionOwnerNodeRid
            || current.SessionOwnerNodeGeneration
               != identity.SessionOwnerNodeGeneration
            || !string.Equals(
                current.SessionOwnerId,
                identity.SessionOwnerId,
                StringComparison.Ordinal)
            || current.SessionOwnerLeaseGeneration
               != identity.SessionOwnerLeaseGeneration)
            return ValueTask.CompletedTask;

        if (!TryScheduleTerminal(
                "actor_binding_replaced",
                () => CompleteAfterTransportClosedAsync(
                    notifyDisconnected: true)))
            return ValueTask.CompletedTask;

        // The close starts outside the serial executor. The fixed timer above
        // is therefore independent of queued application work and never
        // occupies a serial turn while waiting.
        _ = CloseTransportAsync();
        return ValueTask.CompletedTask;
    }

    private void DisposeReplacementCloseTimers()
    {
        var timers = AwaitStateLane(_lane.RunAsync(() =>
        {
            var timers = _replacementCloseTimers.Values.ToArray();
            _replacementCloseTimers.Clear();
            return timers;
        }));
        foreach (var timer in timers)
            timer.Dispose();
    }

    private bool TryScheduleTerminal(
        string reason,
        Func<ValueTask> finalWork,
        bool recordTransportClosedOnTerminalCollision = false)
    {
        var (scheduled, rejected) = AwaitStateLane(_lane.RunAsync(() =>
        {
            if (_terminalClose is not null)
            {
                // A monitor disconnect remains authoritative transport evidence
                // when another terminal path already owns finalization. Record it
                // in the same state-lane turn that observes the collision so the
                // owner does not race a redundant transport disconnect.
                if (recordTransportClosedOnTerminalCollision)
                    _transportClosed.TrySetResult(true);
                return (Scheduled: false, Rejected: false);
            }

            _terminalClose = new TerminalClose(reason, DisposeOwnsClose: false);
            if (EnqueueTerminalOutsideStateLane(finalWork))
                return (Scheduled: true, Rejected: false);

            _terminalClose = new TerminalClose(reason, DisposeOwnsClose: true);
            return (Scheduled: false, Rejected: true);
        }));

        if (rejected)
        {
            MarkTerminalFailed();
            ZLinkUnawaitedSubmit.Observe(
                DisposeAsync(),
                $"stream-session-rejected-terminal-dispose:{Stream.SessionId}",
                _runtime.ErrorSink);
        }
        return scheduled;
    }

    private bool EnqueueTerminalOutsideStateLane(Func<ValueTask> finalWork)
    {
        // The serial runner creates its task while this state-lane turn owns
        // terminal state. Do not let that task inherit the lane's AsyncLocal:
        // terminal completion re-enters this runtime to start finalization.
        if (ExecutionContext.IsFlowSuppressed())
            return _serial.ExecuteFinal(finalWork);

        using (ExecutionContext.SuppressFlow())
            return _serial.ExecuteFinal(finalWork);
    }

    private bool ClaimCloseForDisposal()
    {
        return AwaitStateLane(_lane.RunAsync(() =>
        {
            if (_terminalClose is { } terminalClose) return terminalClose.DisposeOwnsClose;
            _terminalClose = new TerminalClose("transport_error", DisposeOwnsClose: true);
            return true;
        }));
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
        Message payload)
    {
        header.Dispose();
        payload.Dispose();
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
        await ZLinkApplicationJobQueueInvocation
            .EnsureQueuedPermitAsync(cancellationToken)
            .ConfigureAwait(false);
        ZLinkApplicationJobQueueInvocation.ReleaseForHandlerStart();
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
        var (closeTask, owner) = AwaitStateLane(_lane.RunAsync(() =>
        {
            if (_transportCloseTask is not null) return (_transportCloseTask, (TaskCompletionSource?)null);
            if (_transportClosed.Task.IsCompletedSuccessfully
                && _transportClosed.Task.Result)
                return (Task.CompletedTask, (TaskCompletionSource?)null);
            var owner = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            _transportCloseTask = owner.Task;
            return (_transportCloseTask, owner);
        }));

        if (owner is not null)
            StartCloseTransportCore(owner);
        return closeTask;
    }

    private void StartCloseTransportCore(TaskCompletionSource completion)
    {
        if (ExecutionContext.IsFlowSuppressed())
        {
            _ = CloseTransportCoreAsync(completion);
            return;
        }

        using (ExecutionContext.SuppressFlow())
            _ = CloseTransportCoreAsync(completion);
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
        await ZLinkApplicationJobQueueInvocation
            .EnsureQueuedPermitAsync(_terminalCallbackStop.Token)
            .ConfigureAwait(false);
        ZLinkApplicationJobQueueInvocation.ReleaseForHandlerStart();
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
        AwaitStateLane(_lane.RunAsync(() =>
        {
            if (_terminalCallbackStopDisposed
                || _terminalCallbackStopFinalizing
                || _terminalCallbackStop.IsCancellationRequested)
                return;
            _terminalCallbackCancellation = _terminalCallbackStop.CancelAsync();
        }));
    }

    private async ValueTask DisposeTerminalCallbackStopAsync()
    {
        var cancellation = AwaitStateLane(_lane.RunAsync(() =>
        {
            _terminalCallbackStopFinalizing = true;
            return _terminalCallbackCancellation;
        }));
        try
        {
            await cancellation.ConfigureAwait(false);
        }
        finally
        {
            AwaitStateLane(_lane.RunAsync(() =>
            {
                if (!_terminalCallbackStopDisposed)
                {
                    _terminalCallbackStop.Dispose();
                    _terminalCallbackStopDisposed = true;
                }
            }));
        }
    }

    private static T AwaitStateLane<T>(ValueTask<T> operation) =>
        operation.GetAwaiter().GetResult();

    private static void AwaitStateLane(ValueTask operation) =>
        operation.GetAwaiter().GetResult();

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

    private readonly record struct ActorBindingReplacementIdentity(
        string ActorId,
        RoutingId SessionOwnerNodeRid,
        ulong SessionOwnerNodeGeneration,
        string SessionOwnerId,
        ulong SessionOwnerLeaseGeneration,
        RoutingId SessionRid,
        ulong RetiredBindingGeneration,
        string BindingToken);

    private sealed record ReplacementCloseTimerState(
        ZLinkStreamSessionRuntime Owner,
        ActorBindingReplacementIdentity Identity);
}
