using System.Diagnostics;
using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Runtime.Configuration;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Messaging;
using static Zlink.Framework.Runtime.Execution.ZLinkStateLaneWait;

namespace Zlink.Framework.Runtime.Streams;

internal sealed class ZLinkStreamNodeRuntime : IAsyncDisposable
{
    private readonly ZLinkStreamSessionTable _sessions;
    private readonly ZLinkStreamSessionLiveness.Reader _readerLiveness;
    private readonly ZLinkSessionSerialExecutor _sessionIngress;
    private readonly ZLinkSessionSerialExecutor _controlIngress;
    private readonly CancellationTokenSource _stopSource = new();
    private readonly ZLinkRuntimeTaskRunner _taskRunner;
    private readonly ZLinkFrameworkRuntime _runtime;
    private readonly TimeProvider _timeProvider;
    private readonly ZLinkStreamNodeRegistration _livenessOptions;
    private readonly ZLinkAuthenticationAdmission _authenticationAdmission;
    private readonly string _transport;
    private readonly long _maxMessageSize;
    private readonly ZLinkApplicationJobQueue _applicationJobQueue;
    private readonly bool _ownsApplicationJobQueue;
    private Task? _disposeTask;
    private Task? _receiveLoop;

    internal Task ReceiveCompletion => _receiveLoop ?? Task.CompletedTask;

    // The monitor loop closes the monitor when it returns.
    private readonly ZLinkReceiveSocketOwner _monitorOwner;
    private IZLinkBackendSocketPoller? _receivePoller;

    public ZLinkStreamNodeRuntime(
        string nodeName,
        IServiceProvider services,
        IZLinkBackendStreamSocket socket,
        IZLinkBackendSocketMonitor? monitor,
        Type? headerSessionType,
        ZLinkRuntimeTaskRunner taskRunner,
        string transport,
        TimeProvider? timeProvider = null,
        bool actorDispatchEnabled = false,
        string? boundEndpoint = null,
        string? advertisedEndpoint = null,
        long maxMessageSize = ZLinkSocketConfig.DefaultStreamMaxMessageSize,
        ZLinkApplicationJobQueue? applicationJobQueue = null,
        ZLinkAuthenticationAdmission? authenticationAdmission = null
    )
    {
        NodeName = nodeName;
        Socket = socket;
        Monitor = monitor;
        _monitorOwner = new ZLinkReceiveSocketOwner(CloseMonitorAsync);
        BoundEndpoint = boundEndpoint;
        AdvertisedEndpoint = advertisedEndpoint;
        _taskRunner = taskRunner;
        _ownsApplicationJobQueue = applicationJobQueue is null;
        _applicationJobQueue =
            applicationJobQueue
            ?? new ZLinkApplicationJobQueue(
                ZLinkApplicationJobQueueCapacityResolver.Resolve(
                    ZLinkApplicationJobQueueProfile.Balanced,
                    int.MaxValue,
                    1
                )
            );
        _timeProvider = timeProvider ?? TimeProvider.System;
        _readerLiveness = new ZLinkStreamSessionLiveness.Reader(
            _timeProvider,
            () => _receivePoller!.Wake()
        );
        _transport = transport;
        _maxMessageSize = maxMessageSize;
        var runtime = services.GetRequiredService<ZLinkFrameworkRuntime>();
        _runtime = runtime;
        _livenessOptions =
            runtime.Registration.StreamNodes.GetValueOrDefault(nodeName)
            ?? new ZLinkStreamNodeRegistration { StreamNodeName = nodeName };
        _authenticationAdmission =
            authenticationAdmission
            ?? new ZLinkAuthenticationAdmission(_livenessOptions.AuthenticationAdmission);
        _sessionIngress = new ZLinkSessionSerialExecutor(runtime.ExecutionOwner, runtime.ErrorSink);
        _controlIngress = new ZLinkSessionSerialExecutor(runtime.ExecutionOwner, runtime.ErrorSink);
        _sessions = new ZLinkStreamSessionTable(
            services,
            socket,
            headerSessionType,
            runtime.DrainAdmission,
            transport,
            _timeProvider,
            actorDispatchEnabled,
            _runtime.ErrorSink,
            _livenessOptions,
            _readerLiveness
        );
    }

    public string NodeName { get; }

    public IZLinkBackendStreamSocket Socket { get; }

    public IZLinkBackendSocketMonitor? Monitor { get; private set; }

    internal string? BoundEndpoint { get; private set; }

    internal string? AdvertisedEndpoint { get; private set; }

    /// <summary>
    ///     Records the endpoint the node's socket bound. Startup creates the node as soon as
    ///     its socket exists, so the node owns the socket through every later step.
    /// </summary>
    internal void SetEndpoints(string boundEndpoint, string advertisedEndpoint)
    {
        BoundEndpoint = boundEndpoint;
        AdvertisedEndpoint = advertisedEndpoint;
    }

    /// <summary>Hands the socket monitor opened during startup to the node that owns it.</summary>
    internal void AttachMonitor(IZLinkBackendSocketMonitor monitor) => Monitor = monitor;

    internal int SessionCount => AwaitStateLane(_sessions.GetCountAsync());

    internal async ValueTask RequestStopAsync()
    {
        _stopSource.Cancel();
        _sessionIngress.RequestStop();
        _controlIngress.RequestStop();
        await _sessions.RequestStopAsync().ConfigureAwait(false);
    }

    internal ValueTask<bool> DrainSessionsAsync(CancellationToken cancellationToken) =>
        _sessions.DrainSessionsAsync(cancellationToken);

    internal ValueTask SealSessionAdmissionAsync(CancellationToken cancellationToken) =>
        _sessions.SealAdmissionAsync(cancellationToken);

    internal ValueTask ForceStopSessionsAsync(CancellationToken cancellationToken) =>
        _sessions.ForceStopSessionsAsync(cancellationToken);

    public ValueTask DisposeAsync() => DisposeAsync(CancellationToken.None);

    /// <summary>Disposes the node; the token is the host shutdown deadline that forces session cleanup.</summary>
    internal ValueTask DisposeAsync(CancellationToken deadline)
    {
        return new ValueTask(
            ZLinkRuntimeTaskRunner.RunDisposal(ref _disposeTask, () => DisposeCoreAsync(deadline))
        );
    }

    private async Task DisposeCoreAsync(CancellationToken deadline)
    {
        var sessions = await _sessions.StopAsync().ConfigureAwait(false);
        var failures = new List<Exception>();
        await CaptureAsync(RequestStopAsync).ConfigureAwait(false);
        await CaptureAsync(() => DisposeSessionsAsync(sessions, deadline)).ConfigureAwait(false);
        await CaptureAsync(_sessionIngress.DisposeAsync).ConfigureAwait(false);
        await CaptureAsync(_controlIngress.DisposeAsync).ConfigureAwait(false);
        await CaptureAsync(_monitorOwner.CloseAsync).ConfigureAwait(false);

        if (_receiveLoop is not null)
            try
            {
                await _receiveLoop;
            }
            catch (OperationCanceledException) { }
            catch (ObjectDisposedException) { }
            catch (Exception exception)
            {
                failures.Add(exception);
            }

        await CaptureAsync(() => _receivePoller?.DisposeAsync() ?? ValueTask.CompletedTask)
            .ConfigureAwait(false);

        var socketDisposed = false;
        try
        {
            await Socket.DisposeAsync().ConfigureAwait(false);
            socketDisposed = true;
        }
        catch (Exception exception)
        {
            failures.Add(exception);
        }
        if (socketDisposed)
            foreach (var session in sessions)
                session.ConfirmNodeTransportDisposed();
        if (failures.Count == 1)
            System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(failures[0]).Throw();
        if (failures.Count > 1)
            throw new AggregateException(failures);
        _stopSource.Dispose();
        if (_ownsApplicationJobQueue)
            _applicationJobQueue.Dispose();
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
    }

    private async ValueTask DisposeSessionsAsync(
        IReadOnlyCollection<ZLinkStreamSessionRuntime> sessions,
        CancellationToken deadline
    )
    {
        if (sessions.Count == 0)
            return;

        var disposals = sessions
            .Select(static session => session.DisposeAsync().AsTask())
            .ToArray();
        try
        {
            await Task.WhenAll(disposals).WaitAsync(deadline).ConfigureAwait(false);
            return;
        }
        catch (OperationCanceledException) when (deadline.IsCancellationRequested) { }

        var forcedCloses = sessions
            .Select(static session => session.ForceCloseForShutdownAsync().AsTask())
            .ToArray();
        for (var index = 0; index < disposals.Length; index++)
            ZLinkUnawaitedSubmit.Observe(
                new ValueTask(disposals[index]),
                $"stream-session-late-dispose:{index}",
                _runtime.ErrorSink
            );
        await Task.WhenAll(forcedCloses).ConfigureAwait(false);
    }

    public void Start()
    {
        _receivePoller ??= Socket.CreateReceivePoller();
        _receiveLoop = _taskRunner.Run($"stream-recv:{NodeName}", RunReceiveLoopUntilStopped);
        _monitorOwner.Start(_taskRunner, $"stream-monitor:{NodeName}", RunMonitorLoopUntilStopped);
    }

    private async ValueTask RunMonitorLoopUntilStopped(CancellationToken runtimeToken)
    {
        using var stop = CancellationTokenSource.CreateLinkedTokenSource(
            _stopSource.Token,
            runtimeToken
        );
        await RunMonitorLoop(stop.Token).ConfigureAwait(false);
    }

    private ValueTask CloseMonitorAsync() =>
        Monitor is { } monitor ? monitor.DisposeAsync() : ValueTask.CompletedTask;

    private async ValueTask RunReceiveLoopUntilStopped(CancellationToken runtimeToken)
    {
        using var stop = CancellationTokenSource.CreateLinkedTokenSource(
            _stopSource.Token,
            runtimeToken
        );
        var poller = _receivePoller!;
        var failureBackoff = new ZLinkPollingBackoff();
        try
        {
            while (!stop.IsCancellationRequested)
            {
                // A failed wait or resume ends this logical source.
                var readiness = await poller
                    .WaitAsync(
                        _readerLiveness.WakeRequested ? TimeSpan.Zero : Timeout.InfiniteTimeSpan,
                        stop.Token
                    )
                    .ConfigureAwait(false);
                if (!CanReceive(readiness))
                {
                    if (!_readerLiveness.WakeRequested)
                        continue;

                    // A wake can follow the native poll snapshot. Take a fresh snapshot before
                    // treating a wake without readiness as an empty receive round.
                    readiness = await poller
                        .WaitAsync(TimeSpan.Zero, stop.Token)
                        .ConfigureAwait(false);
                }
                var permitResumedAt = _readerLiveness.PermitResumedAt;
                if (
                    CanReceive(readiness)
                    && !await ReceivePacketsAsync(stop.Token, failureBackoff).ConfigureAwait(false)
                )
                    return;
                if (stop.IsCancellationRequested)
                    return;
                if (permitResumedAt != _readerLiveness.PermitResumedAt)
                    await RestartLivenessAsync().ConfigureAwait(false);
                else if (_readerLiveness.WakeRequested)
                    await AdvanceDueLivenessAsync(confirmExpiry: true).ConfigureAwait(false);
            }
        }
        catch (Exception) when (stop.IsCancellationRequested) { }
        finally
        {
            await stop.CancelAsync().ConfigureAwait(false);
        }
    }

    private static bool CanReceive(ZLinkBackendSocketReadiness readiness) =>
        (
            readiness
            & (
                ZLinkBackendSocketReadiness.Readable
                | ZLinkBackendSocketReadiness.Error
                | ZLinkBackendSocketReadiness.Priority
            )
        ) != 0;

    private async ValueTask AdvanceDueLivenessAsync(bool confirmExpiry)
    {
        var count = await _sessions.TakeDueLivenessWakeAsync().ConfigureAwait(false);
        var timestamp = _timeProvider.GetTimestamp();
        for (var index = 0; index < count; index++)
        {
            var timer = await _sessions.DequeueDueLivenessAsync().ConfigureAwait(false);
            if (timer is null)
                break; // The session table stopped while the reader was processing the wake.
            await AdvanceLivenessAsync(timer.Session, timestamp, confirmExpiry, timer)
                .ConfigureAwait(false);
        }
    }

    private async ValueTask RestartLivenessAsync()
    {
        var sessions = await _sessions.RestartLivenessAsync().ConfigureAwait(false);
        var timestamp = _timeProvider.GetTimestamp();
        foreach (var session in sessions)
            await AdvanceLivenessAsync(session, timestamp, confirmExpiry: true)
                .ConfigureAwait(false);
    }

    private async ValueTask AdvanceLivenessAsync(
        ZLinkStreamSessionRuntime session,
        long timestamp,
        bool confirmExpiry,
        ZLinkStreamSessionRuntime.LivenessTimer? dueTimer = null
    )
    {
        try
        {
            await session
                .AdvanceLivenessAsync(timestamp, confirmExpiry, dueTimer)
                .ConfigureAwait(false);
        }
        catch (Exception exception)
        {
            _runtime.ErrorSink.ReportRuntimeTaskException(
                $"stream-liveness:{session.Stream.SessionId}",
                exception
            );
        }
    }

    private async ValueTask<ZLinkApplicationJobQueueLease> WaitForPermitAsync(
        ValueTask<ZLinkApplicationJobQueueLease> permit,
        CancellationToken stopToken
    )
    {
        var pending = _readerLiveness.WaitForPermitAsync(permit).AsTask();
        var poller = _receivePoller!;
        try
        {
            if (!pending.IsCompleted)
                pending.ConfigureAwait(false).GetAwaiter().UnsafeOnCompleted(poller.Wake);

            while (!pending.IsCompleted && !stopToken.IsCancellationRequested)
            {
                if (_readerLiveness.WakeRequested)
                    await AdvanceDueLivenessAsync(confirmExpiry: false).ConfigureAwait(false);
                if (!pending.IsCompleted)
                    await poller.WaitForWakeAsync(stopToken).ConfigureAwait(false);
            }
        }
        catch
        {
            ZLinkUnawaitedSubmit.Observe(
                ReleasePendingPermitAsync(),
                $"stream-permit:{NodeName}",
                _runtime.ErrorSink
            );
            throw;
        }

        return await pending.ConfigureAwait(false);

        async ValueTask ReleasePendingPermitAsync()
        {
            try
            {
                (await pending.ConfigureAwait(false)).Dispose();
            }
            catch (OperationCanceledException) when (stopToken.IsCancellationRequested) { }
        }
    }

    private async ValueTask<bool> ReceivePacketsAsync(
        CancellationToken stopToken,
        ZLinkPollingBackoff failureBackoff
    )
    {
        var batchStartedAt = Stopwatch.GetTimestamp();
        long batchBytes = 0;
        for (
            var receivedCount = 0;
            receivedCount < ZLinkReceiveBatchBudget.MaximumRecords
                && !stopToken.IsCancellationRequested;
            receivedCount++
        )
        {
            if (ZLinkReceiveBatchBudget.IsExhausted(receivedCount, batchBytes, batchStartedAt))
                break;
            RoutingId? routingId = null;
            ZLinkBackendStreamReceive? received = null;
            var permit = _applicationJobQueue.AcquireAsync(
                stopToken,
                ZLinkApplicationJobOrigin.Remote
            );
            if (!permit.IsCompleted)
                permit = WaitForPermitAsync(permit, stopToken);
            ZLinkApplicationJobQueueLease? admission = await permit.ConfigureAwait(false);
            try
            {
                await _receivePoller!.ResumeAsync().ConfigureAwait(false);
                try
                {
                    try
                    {
                        if (!Socket.RecvPacket(out received, RecvFlags.DontWait))
                        {
                            admission.Dispose();
                            admission = null;
                            break;
                        }

                        routingId = received?.SourceRoutingId;
                        if (
                            routingId is not { } sourceRoutingId
                            || received is null
                            || !received.HasPacket
                        )
                        {
                            received?.Dispose();
                            received = null;
                            admission.Dispose();
                            admission = null;
                            _runtime.ErrorSink.ReportRuntimeTaskException(
                                $"stream-recv:{NodeName}",
                                new InvalidDataException(
                                    "STREAM packet did not provide a source routing id, header, and payload."
                                )
                            );
                            continue;
                        }
                        var receivedBytes = received.ByteLength;
                        if (_maxMessageSize > 0 && receivedBytes > _maxMessageSize)
                            throw new InvalidDataException(
                                "EMSGSIZE: STREAM packet exceeds MaxMessageSize."
                            );
                        var packet = received.TakePacket();
                        received.Dispose();
                        received = null;
                        using var frame = new ZLinkStreamInboundFrame(packet.Header, packet.Payload)
                        {
                            ApplicationJobAdmission = admission,
                        };
                        admission = null;
                        if (!await TryAdmitFrameAsync(sourceRoutingId, frame).ConfigureAwait(false))
                            throw new ZLinkStreamPeerAdmissionException(
                                "STREAM packet could not enter the session queue."
                            );
                        batchBytes = checked(batchBytes + receivedBytes);
                    }
                    catch (Exception exception)
                    {
                        received?.Dispose();
                        received = null;
                        admission?.Dispose();
                        admission = null;
                        if (routingId is { } sourceRoutingId)
                            HandlePeerReceiveFailure(sourceRoutingId, exception);
                        else
                            throw;
                    }
                }
                catch (Exception) when (stopToken.IsCancellationRequested)
                {
                    throw;
                }
                catch (ObjectDisposedException)
                {
                    return false;
                }
                catch (ZlinkRecvException exception)
                    when (exception.Result
                            is ZlinkRecvException.ErrorCode.InternalError
                                or ZlinkRecvException.ErrorCode.InvalidHandle
                    )
                {
                    return false;
                }
                catch (Exception exception)
                {
                    _runtime.ErrorSink.ReportRuntimeTaskException(
                        $"stream-recv:{NodeName}",
                        exception
                    );
                    await failureBackoff.NoDataAsync(stopToken).ConfigureAwait(false);
                    return true;
                }
            }
            finally
            {
                received?.Dispose();
                admission?.Dispose();
            }
        }
        failureBackoff.Reset();
        return true;
    }

    private void HandlePeerReceiveFailure(RoutingId routingId, Exception exception)
    {
        try
        {
            Socket.DisconnectPeer(routingId);
        }
        catch (Exception disconnectFailure)
        {
            _runtime.ErrorSink.ReportRuntimeTaskException(
                $"stream-recv-disconnect:{NodeName}",
                disconnectFailure
            );
        }

        _runtime.ErrorSink.ReportRuntimeTaskException($"stream-recv-peer:{NodeName}", exception);
    }

    private async Task<bool> TryAdmitFrameAsync(RoutingId routingId, ZLinkStreamInboundFrame frame)
    {
        var header =
            frame.Header
            ?? throw new InvalidOperationException("STREAM frame header ownership was lost.");
        var payload =
            frame.Payload
            ?? throw new InvalidOperationException("STREAM frame payload ownership was lost.");

        if (
            _stopSource.IsCancellationRequested
            || await _sessions.IsStoppingAsync().ConfigureAwait(false)
        )
        {
            frame.Dispose();
            return true;
        }

        try
        {
            ZlinkStreamHeader decoded;
            try
            {
                decoded = ZLinkStreamProtocolDefaults.DecodeHeader(
                    header.AsReadOnlyMemory(),
                    _runtime.Flow.CaptureEnabled
                );
            }
            catch (Exception protocolError)
            {
                if (await _sessions.TryGetAsync(routingId).ConfigureAwait(false) is not { } session)
                    throw;
                await session.CloseForProtocolErrorAsync(protocolError).ConfigureAwait(false);
                return true;
            }

            if (decoded.Kind == ZlinkStreamMessageKind.Control)
            {
                // Control is transport infrastructure and does not create an application session.
                if (await _sessions.TryGetAsync(routingId).ConfigureAwait(false) is { } session)
                    await session
                        .DispatchControlPacketAsync(decoded, payload.AsReadOnlyMemory())
                        .ConfigureAwait(false);
                else
                {
                    var stream = new ZLinkManagedStream(
                        Socket,
                        routingId,
                        _runtime.Registration.Codecs,
                        _transport
                    );
                    try
                    {
                        ZLinkStreamControlFrames.Dispatch(
                            stream,
                            decoded,
                            payload.AsReadOnlyMemory(),
                            ZLinkMessageFlowTracer.GetLogger(_runtime.Services)
                        );
                    }
                    catch (Exception protocolError)
                    {
                        _runtime.ErrorSink.ReportHandlerException(protocolError);
                        try
                        {
                            ZLinkStreamControlFrames.SendProtocolError(stream);
                        }
                        finally
                        {
                            await stream.CloseAsync().ConfigureAwait(false);
                        }
                    }
                }
                return true;
            }

            if (!_authenticationAdmission.TryAdmit(decoded, out var authentication))
            {
                frame.Dispose();
                if (await _sessions.TryGetAsync(routingId).ConfigureAwait(false) is { } session)
                    session.RecordRejectedApplicationInbound();

                var error = new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    "Authentication admission is full."
                );
                using var flow = ZLinkFlowContext.Enter(
                    decoded.FlowId,
                    decoded.FlowOrigin is { } origin
                        ? ZLinkStreamHeaderCodec.ToFrameworkOrigin(origin)
                        : null,
                    _runtime.Flow.CaptureEnabled,
                    ZLinkFlowOrigin.Inbound
                );
                if (_runtime.Flow.DispatchErrorsEnabled)
                    _runtime.Flow.TraceDispatchError(
                        new ZLinkDispatchFailure(
                            ZLinkDispatchErrorSurface.StreamSession,
                            ZLinkDispatchMessageKind.Request,
                            ZLinkDispatchErrorReason.Backpressure,
                            ZLinkDispatchErrorAction.ReplyError,
                            decoded.Name,
                            SourceRid: routingId.ToString(),
                            CorrelationId: decoded.CorrelationId,
                            Exception: error
                        )
                    );
                var (_, reply) = ZLinkSessionStreamTransport.EncodeError(
                    decoded,
                    error,
                    _authenticationAdmission.RetryMetadata
                );
                ZLinkUnawaitedSubmit.Observe(
                    new ValueTask(Socket.SendAsync(routingId, reply, _stopSource.Token)),
                    "stream-authentication-admission-rejection",
                    _runtime.ErrorSink
                );
                return true;
            }

            var admitted = false;
            try
            {
                admitted = await AdmitApplicationPacketAsync(
                        routingId,
                        header,
                        payload,
                        decoded,
                        frame.ApplicationJobAdmission,
                        authentication
                    )
                    .ConfigureAwait(false);
                if (admitted)
                    frame.Detach();
                return admitted;
            }
            finally
            {
                if (!admitted)
                    authentication?.Release();
            }
        }
        catch (ZLinkStreamPeerAdmissionException)
        {
            frame.Dispose();
            throw;
        }
        catch (Exception exception)
        {
            _runtime.ErrorSink.ReportHandlerException(exception);
            frame.Dispose();
            throw;
        }
    }

    private async Task<bool> AdmitApplicationPacketAsync(
        RoutingId routingId,
        Message header,
        Message payload,
        ZlinkStreamHeader decoded,
        ZLinkApplicationJobQueueLease? applicationJobAdmission,
        ZLinkAuthenticationAdmission.Lease? authentication
    )
    {
        applicationJobAdmission?.MarkQueued();
        if (await _sessions.TryGetAsync(routingId).ConfigureAwait(false) is { } existing)
        {
            existing.EnqueuePacket(
                header,
                payload,
                decoded,
                applicationJobAdmission,
                authentication
            );
            return true;
        }

        var ingressAdmission = _sessionIngress.ExecuteApplication(async cancellationToken =>
        {
            var ownershipTransferred = false;
            try
            {
                var session = await _sessions
                    .GetOrCreateAsync(routingId, cancellationToken, decoded)
                    .ConfigureAwait(false);
                if (session is not null)
                {
                    session.EnqueuePacket(
                        header,
                        payload,
                        decoded,
                        applicationJobAdmission,
                        authentication
                    );
                    ownershipTransferred = true;
                }
            }
            finally
            {
                if (!ownershipTransferred)
                {
                    DisposeRejectedPacket(header, payload);
                    applicationJobAdmission?.Dispose();
                    authentication?.Release();
                }
            }
        });
        if (ingressAdmission == ZLinkSerialPostAdmission.Accepted)
        {
            return true;
        }

        throw new ZLinkStreamPeerAdmissionException("STREAM session ingress queue is closed.");
    }

    private async Task OnMonitorEventAsync(ZLinkBackendSocketMonitorEvent monitorEvent)
    {
        if (await _sessions.IsStoppingAsync().ConfigureAwait(false))
            return;

        switch (monitorEvent.NativeEvent)
        {
            case ZLinkSocketNativeEventType.ConnectionReady:
                if ((monitorEvent.Flags & MonitorEventFlags.ConnectionReadyEdge) == 0)
                    break;
                if (monitorEvent.RoutingId is RoutingId readyRoutingId)
                {
                    var connectedAt = _timeProvider.GetTimestamp();
                    var connectedAdmission = _controlIngress.ExecuteControl(
                        async cancellationToken =>
                        {
                            var session = await _sessions
                                .GetOrCreateAsync(readyRoutingId, cancellationToken)
                                .ConfigureAwait(false);
                            if (session is not null)
                            {
                                var sessionAdmission = session.EnqueueConnected(
                                    monitorEvent.LocalAddr,
                                    monitorEvent.RemoteAddr,
                                    connectedAt
                                );
                                await ReportControlAdmissionAsync(
                                        "stream-session-connected",
                                        sessionAdmission
                                    )
                                    .ConfigureAwait(false);
                            }
                            await ValueTask.CompletedTask;
                        }
                    );
                    await ReportControlAdmissionAsync(
                            "stream-monitor-connected",
                            connectedAdmission
                        )
                        .ConfigureAwait(false);
                }
                break;
            case ZLinkSocketNativeEventType.Accepted:
                break;
            case ZLinkSocketNativeEventType.Disconnected:
                var disconnectedAdmission = _controlIngress.ExecuteControl(async _ =>
                {
                    if (
                        await _sessions
                            .TryResolveMonitorSessionAsync(monitorEvent.RoutingId)
                            .ConfigureAwait(false) is
                        { } disconnectedSession
                    )
                        disconnectedSession.EnqueueDisconnected(
                            new ZLinkStreamError(
                                ZLinkStreamSessionError.TransportError,
                                monitorEvent.NativeEvent.ToString(),
                                (ZLinkDisconnectReason)monitorEvent.Value
                            )
                        );
                });
                await ReportControlAdmissionAsync(
                        "stream-session-disconnected",
                        disconnectedAdmission
                    )
                    .ConfigureAwait(false);
                break;
        }
    }

    private async Task ReportControlAdmissionAsync(
        string operation,
        ZLinkSerialPostAdmission admission
    )
    {
        if (admission == ZLinkSerialPostAdmission.Accepted)
            return;
        if (
            admission == ZLinkSerialPostAdmission.Closed
            && (
                _stopSource.IsCancellationRequested
                || await _sessions.IsStoppingAsync().ConfigureAwait(false)
            )
        )
            return;

        _runtime.ErrorSink.ReportRuntimeTaskException(
            operation,
            new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ShuttingDown,
                "The STREAM control queue closed before the monitor event was admitted."
            )
        );
    }

    private sealed class ZLinkStreamPeerAdmissionException(string message)
        : InvalidOperationException(message);

    private async ValueTask RunMonitorLoop(CancellationToken cancellationToken)
    {
        var monitor = Monitor!;
        await using var poller = monitor.CreateReceivePoller();
        while (!cancellationToken.IsCancellationRequested)
        {
            try
            {
                if (
                    await poller
                        .WaitAsync(Timeout.InfiniteTimeSpan, cancellationToken)
                        .ConfigureAwait(false) == ZLinkBackendSocketReadiness.None
                )
                    continue;
                var startedAt = Stopwatch.GetTimestamp();
                for (
                    var count = 0;
                    !ZLinkReceiveBatchBudget.IsExhausted(count, 0, startedAt);
                    count++
                )
                {
                    await poller.ResumeAsync().ConfigureAwait(false);
                    if (!monitor.TryRecv(out var monitorEvent))
                        break;
                    await OnMonitorEventAsync(monitorEvent).ConfigureAwait(false);
                }
            }
            catch (Exception) when (cancellationToken.IsCancellationRequested)
            {
                return;
            }
            catch (ObjectDisposedException)
            {
                return;
            }
            catch (ZlinkRecvException ex)
                when (ex.Result
                        is ZlinkRecvException.ErrorCode.InternalError
                            or ZlinkRecvException.ErrorCode.InvalidHandle
                )
            {
                return;
            }
        }
    }

    private static void DisposeRejectedPacket(Message header, Message payload)
    {
        header.Dispose();
        payload.Dispose();
    }
}
