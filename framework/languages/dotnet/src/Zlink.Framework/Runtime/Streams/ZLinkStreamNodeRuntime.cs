using System.Diagnostics;
using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Messaging;

namespace Zlink.Framework.Runtime.Streams;

internal sealed class ZLinkStreamNodeRuntime : IAsyncDisposable
{
    internal static readonly TimeSpan SessionShutdownUpperBound = TimeSpan.FromMilliseconds(900);
    internal static readonly TimeSpan SessionForceCleanupUpperBound = TimeSpan.FromMilliseconds(100);
    private const int ReceiveBatchSize = 64;
    private static readonly TimeSpan ReceivePollInterval =
        TimeSpan.FromMilliseconds(100);
    private readonly ZLinkStreamSessionTable _sessions;
    private readonly ZLinkSessionSerialExecutor _sessionIngress;
    private readonly ZLinkSessionSerialExecutor _controlIngress;
    private readonly CancellationTokenSource _stopSource = new();
    private readonly ZLinkRuntimeTaskRunner _taskRunner;
    private readonly IZLinkRuntimeFailureReporter _errorSink;
    private readonly TimeProvider _timeProvider;
    private readonly string _transport;
    private readonly long _maxMessageSize;
    private readonly ZLinkApplicationJobQueue _applicationJobQueue;
    private readonly bool _ownsApplicationJobQueue;
    private readonly ZLinkStateLane _lane = new();
    private readonly Dictionary<RoutingId, LinkedListNode<RoutingId>>
        _disconnectedRoutingIds = [];
    private readonly LinkedList<RoutingId> _disconnectedRoutingIdOrder = [];
    private const int DisconnectedRoutingIdLimit = 4096;
    private Task? _disposeTask;
    private Task? _receiveLoop;
    private Task? _livenessLoop;
    private Task? _monitorLoop;
    private IZLinkBackendSocketPoller? _receivePoller;

    public ZLinkStreamNodeRuntime(
        string nodeName,
        IServiceProvider services,
        IZLinkBackendStreamSocket socket,
        IZLinkBackendSocketMonitor monitor,
        Type? headerSessionType,
        ZLinkRuntimeTaskRunner taskRunner,
        string transport,
        TimeProvider? timeProvider = null,
        bool actorDispatchEnabled = false,
        string? boundEndpoint = null,
        string? advertisedEndpoint = null,
        long maxMessageSize = 64L * 1024L,
        ZLinkApplicationJobQueue? applicationJobQueue = null)
    {
        NodeName = nodeName;
        Socket = socket;
        Monitor = monitor;
        BoundEndpoint = boundEndpoint;
        AdvertisedEndpoint = advertisedEndpoint;
        _taskRunner = taskRunner;
        _ownsApplicationJobQueue = applicationJobQueue is null;
        _applicationJobQueue = applicationJobQueue
            ?? new ZLinkApplicationJobQueue(
                ZLinkApplicationJobQueueCapacityResolver.Resolve(
                    ZLinkApplicationJobQueueProfile.Balanced,
                    int.MaxValue,
                    1));
        _timeProvider = timeProvider ?? TimeProvider.System;
        _transport = transport;
        _maxMessageSize = maxMessageSize;
        var runtime = services.GetRequiredService<ZLinkFrameworkRuntime>();
        _errorSink = runtime.ErrorSink;
        _sessionIngress = new ZLinkSessionSerialExecutor(runtime.ExecutionOwner, runtime.ErrorSink);
        _controlIngress = new ZLinkSessionSerialExecutor(
            runtime.ExecutionOwner,
            runtime.ErrorSink);
        _sessions = new ZLinkStreamSessionTable(
            services,
            socket,
            headerSessionType,
            runtime.DrainAdmission,
            transport,
            _timeProvider,
            actorDispatchEnabled);
    }

    public string NodeName { get; }

    public IZLinkBackendStreamSocket Socket { get; }

    public IZLinkBackendSocketMonitor Monitor { get; }

    internal string? BoundEndpoint { get; }

    internal string? AdvertisedEndpoint { get; }

    internal int SessionCount => AwaitStateLane(_sessions.GetCountAsync());

    internal void RequestStop()
    {
        _stopSource.Cancel();
        _sessionIngress.RequestStop();
        _controlIngress.RequestStop();
        AwaitStateLane(_sessions.RequestStopAsync());
    }

    internal ValueTask<bool> DrainSessionsAsync(CancellationToken cancellationToken) =>
        _sessions.DrainSessionsAsync(cancellationToken);

    internal void ForceStopSessions() =>
        AwaitStateLane(_sessions.ForceStopSessionsAsync());

    public ValueTask DisposeAsync()
    {
        var task = AwaitStateLane(_lane.RunAsync(() =>
        {
            if (_disposeTask is not null) return _disposeTask;
            using (ExecutionContext.SuppressFlow())
                _disposeTask = Task.Run(DisposeCoreAsync);
            return _disposeTask;
        }));
        return new ValueTask(task);
    }

    private async Task DisposeCoreAsync()
    {
        var sessions = await _sessions.StopAsync().ConfigureAwait(false);
        var failures = new List<Exception>();
        Capture(RequestStop);
        await CaptureAsync(() => DisposeSessionsAsync(sessions)).ConfigureAwait(false);
        await CaptureAsync(_sessionIngress.DisposeAsync).ConfigureAwait(false);
        await CaptureAsync(_controlIngress.DisposeAsync).ConfigureAwait(false);
        await CaptureAsync(Monitor.DisposeAsync).ConfigureAwait(false);

        if (_monitorLoop is not null)
            try
            {
                await _monitorLoop;
            }
            catch (OperationCanceledException)
            {
            }
            catch (ObjectDisposedException)
            {
            }
            catch (Exception exception)
            {
                failures.Add(exception);
            }

        if (_livenessLoop is not null)
            try
            {
                await _livenessLoop;
            }
            catch (OperationCanceledException)
            {
            }
            catch (ObjectDisposedException)
            {
            }
            catch (Exception exception)
            {
                failures.Add(exception);
            }

        if (_receiveLoop is not null)
            try
            {
                await _receiveLoop;
            }
            catch (OperationCanceledException)
            {
            }
            catch (ObjectDisposedException)
            {
            }
            catch (Exception exception)
            {
                failures.Add(exception);
            }

        Capture(() => _receivePoller?.Dispose());

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
            foreach (var session in sessions) session.ConfirmNodeTransportDisposed();
        Capture(_stopSource.Dispose);
        if (_ownsApplicationJobQueue)
            Capture(_applicationJobQueue.Dispose);
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

    private async ValueTask DisposeSessionsAsync(
        IReadOnlyCollection<ZLinkStreamSessionRuntime> sessions)
    {
        if (sessions.Count == 0) return;

        var disposals = sessions.Select(static session => session.DisposeAsync().AsTask()).ToArray();
        try
        {
            await Task.WhenAll(disposals)
                .WaitAsync(SessionShutdownUpperBound)
                .ConfigureAwait(false);
            return;
        }
        catch (TimeoutException)
        {
        }

        var forcedCloses = sessions
            .Select(static session => session.ForceCloseForShutdownAsync().AsTask())
            .ToArray();
        try
        {
            await Task.WhenAll(disposals.Concat(forcedCloses))
                .WaitAsync(SessionForceCleanupUpperBound)
                .ConfigureAwait(false);
        }
        catch (TimeoutException)
        {
            for (var index = 0; index < disposals.Length; index++)
                ZLinkUnawaitedSubmit.Observe(
                    new ValueTask(disposals[index]),
                    $"stream-session-late-dispose:{index}",
                    _errorSink);
            for (var index = 0; index < forcedCloses.Length; index++)
                ZLinkUnawaitedSubmit.Observe(
                    new ValueTask(forcedCloses[index]),
                    $"stream-session-late-force-close:{index}",
                    _errorSink);
        }
    }

    public void Start()
    {
        _receivePoller ??= Socket.CreateReceivePoller();
        _receiveLoop = _taskRunner.RunLongRunning(
            $"stream-recv:{NodeName}",
            runtimeToken => new ValueTask(RunReceiveLoopUntilStoppedAsync(runtimeToken)));
        _monitorLoop = _taskRunner.RunLongRunning(
            $"stream-monitor:{NodeName}",
            runtimeToken => new ValueTask(RunMonitorLoopUntilStoppedAsync(runtimeToken)));
        _livenessLoop = _taskRunner.Run(
            $"stream-liveness:{NodeName}",
            runtimeToken => new ValueTask(RunLivenessLoopUntilStoppedAsync(runtimeToken)));
    }

    private async Task RunMonitorLoopUntilStoppedAsync(CancellationToken runtimeToken)
    {
        using var stop = CancellationTokenSource.CreateLinkedTokenSource(
            _stopSource.Token,
            runtimeToken);
        await RunMonitorLoopAsync(stop.Token).ConfigureAwait(false);
    }

    private async Task RunLivenessLoopUntilStoppedAsync(CancellationToken runtimeToken)
    {
        using var stop = CancellationTokenSource.CreateLinkedTokenSource(
            _stopSource.Token,
            runtimeToken);
        while (!stop.IsCancellationRequested)
        {
            await Task.Delay(
                ZLinkStreamSessionLiveness.SweepInterval,
                    _timeProvider,
                    stop.Token)
                .ConfigureAwait(false);
            foreach (var session in await _sessions.SnapshotAsync().ConfigureAwait(false))
                session.CheckLiveness();
        }
    }

    private async Task RunReceiveLoopUntilStoppedAsync(CancellationToken runtimeToken)
    {
        using var stop = CancellationTokenSource.CreateLinkedTokenSource(
            _stopSource.Token,
            runtimeToken);
        var failureBackoff = new ZLinkPollingBackoff();
        while (!stop.IsCancellationRequested)
        {
            try
            {
                var readiness = _receivePoller!.Wait(ReceivePollInterval);
                if ((readiness & (ZLinkBackendSocketReadiness.Readable
                                  | ZLinkBackendSocketReadiness.Error
                                  | ZLinkBackendSocketReadiness.Priority)) == 0)
                    continue;

                var batchStartedAt = Stopwatch.GetTimestamp();
                long batchBytes = 0;
                for (var receivedCount = 0;
                     receivedCount < ReceiveBatchSize
                     && !stop.IsCancellationRequested;
                     receivedCount++)
                {
                    if (ZLinkReceiveBatchBudget.IsExhausted(
                            receivedCount,
                            batchBytes,
                            batchStartedAt))
                        break;
                    RoutingId? routingId = null;
                    ZLinkBackendStreamReceive? received = null;
                    ZLinkApplicationJobQueueLease? admission = null;
                    try
                    {
                        admission = await _applicationJobQueue
                            .AcquireAsync(stop.Token)
                            .ConfigureAwait(false);
                        if (!Socket.RecvPacket(
                                out received,
                                RecvFlags.DontWait))
                        {
                            admission.Dispose();
                            admission = null;
                            break;
                        }

                        routingId = received?.SourceRoutingId;
                        if (routingId is not { } sourceRoutingId
                            || received is null
                            || !received.HasPacket)
                        {
                            received?.Dispose();
                            received = null;
                            admission.Dispose();
                            admission = null;
                            _errorSink.ReportRuntimeTaskException(
                                $"stream-recv:{NodeName}",
                                new InvalidDataException(
                                    "STREAM packet did not provide a source routing id, header, and payload."));
                            continue;
                        }
                        var receivedBytes = received.ByteLength;
                        if (_maxMessageSize > 0
                            && receivedBytes > _maxMessageSize)
                            throw new InvalidDataException(
                                "EMSGSIZE: STREAM packet exceeds MaxMessageSize.");
                        var packet = received.TakePacket();
                        received.Dispose();
                        received = null;
                        using var frame = new ZLinkStreamInboundFrame(
                            packet.Header,
                            packet.Payload)
                        {
                            ApplicationJobAdmission = admission
                        };
                        admission = null;
                        if (!TryAdmitFrame(sourceRoutingId, frame))
                            throw new ZLinkStreamPeerAdmissionException(
                                "STREAM packet could not enter the session queue.");
                        batchBytes = checked(batchBytes + receivedBytes);
                    }
                    catch (Exception exception)
                    {
                        received?.Dispose();
                        admission?.Dispose();
                        if (routingId is { } sourceRoutingId)
                        {
                            HandlePeerReceiveFailure(sourceRoutingId, exception);
                        }
                        else
                            throw;
                    }
                }
                failureBackoff.Reset();
            }
            catch (OperationCanceledException) when (stop.IsCancellationRequested)
            {
                return;
            }
            catch (ObjectDisposedException)
            {
                return;
            }
            catch (ZlinkRecvException exception)
                when (stop.IsCancellationRequested
                      || exception.Result is ZlinkRecvException.ErrorCode.InternalError
                          or ZlinkRecvException.ErrorCode.InvalidHandle)
            {
                return;
            }
            catch (ZlinkException) when (stop.IsCancellationRequested)
            {
                return;
            }
            catch (Exception exception)
            {
                _errorSink.ReportRuntimeTaskException(
                    $"stream-recv:{NodeName}", exception);
                try
                {
                    await failureBackoff.NoDataAsync(stop.Token).ConfigureAwait(false);
                }
                catch (OperationCanceledException)
                    when (stop.IsCancellationRequested)
                {
                    return;
                }
            }
        }
    }

    private void HandlePeerReceiveFailure(RoutingId routingId, Exception exception)
    {
        MarkDisconnectedRoutingId(routingId);
        try
        {
            Socket.DisconnectPeer(routingId);
        }
        catch (Exception disconnectFailure)
        {
            _errorSink.ReportRuntimeTaskException(
                $"stream-recv-disconnect:{NodeName}", disconnectFailure);
        }

        _errorSink.ReportRuntimeTaskException(
            $"stream-recv-peer:{NodeName}", exception);
    }

    private bool TryAdmitFrame(
        RoutingId routingId,
        ZLinkStreamInboundFrame frame)
    {
        var header = frame.Header
            ?? throw new InvalidOperationException("STREAM frame header ownership was lost.");
        var payload = frame.Payload
            ?? throw new InvalidOperationException("STREAM frame payload ownership was lost.");

        if (_stopSource.IsCancellationRequested || AwaitStateLane(_sessions.IsStoppingAsync()))
        {
            frame.Dispose();
            return true;
        }

        try
        {
            var admitted = IsApplicationPacket(header)
                ? AdmitApplicationPacket(
                    routingId,
                    frame,
                    header,
                    payload,
                    frame.ApplicationJobAdmission)
                : AdmitControlPacket(
                    routingId,
                    header,
                    payload,
                    frame.ApplicationJobAdmission);
            if (admitted) frame.Detach();
            return admitted;
        }
        catch (ZLinkStreamPeerAdmissionException)
        {
            frame.Dispose();
            throw;
        }
        catch (Exception exception)
        {
            _errorSink.ReportHandlerException(exception);
            frame.Dispose();
            throw;
        }
    }

    private bool AdmitApplicationPacket(
        RoutingId routingId,
        ZLinkStreamInboundFrame frame,
        Message header,
        Message payload,
        ZLinkApplicationJobQueueLease? applicationJobAdmission)
    {
        applicationJobAdmission?.MarkQueued();
        if (AwaitStateLane(_sessions.TryGetAsync(routingId)) is { } existing)
        {
            var admission = existing.TryEnqueuePacket(
                    header,
                    payload,
                    applicationJobAdmission);
                if (admission == ZLinkSerialPostAdmission.Accepted)
                {
                    return true;
                }
                throw new ZLinkStreamPeerAdmissionException(
                    "STREAM peer session queue is closed.");
        }

            var ingressAdmission = _sessionIngress.ExecuteApplication(
                async cancellationToken =>
                {
                    var ownershipTransferred = false;
                    try
                    {
                        var session = await _sessions.GetOrCreateAsync(routingId)
                            .ConfigureAwait(false);
                        if (session is not null)
                            ownershipTransferred = session.TryEnqueuePacket(
                                    header,
                                    payload,
                                    applicationJobAdmission)
                                == ZLinkSerialPostAdmission.Accepted;
                    }
                    finally
                    {
                        if (!ownershipTransferred)
                        {
                            DisposeRejectedPacket(header, payload);
                            applicationJobAdmission?.Dispose();
                        }
                    }
                });
            if (ingressAdmission == ZLinkSerialPostAdmission.Accepted)
            {
                return true;
            }

        throw new ZLinkStreamPeerAdmissionException(
            "STREAM session ingress queue is closed.");
    }

    private bool AdmitControlPacket(
        RoutingId routingId,
        Message header,
        Message payload,
        ZLinkApplicationJobQueueLease? applicationJobAdmission)
    {
        if (AwaitStateLane(_sessions.TryGetAsync(routingId)) is { } existing)
        {
            var admission = existing.TryEnqueueControlPacket(
                header,
                payload,
                applicationJobAdmission);
            if (admission == ZLinkSerialPostAdmission.Accepted) return true;
            throw new ZLinkStreamPeerAdmissionException(
                "STREAM peer control queue is closed.");
        }

        if (_controlIngress.ExecuteControl(async cancellationToken =>
        {
            var ownershipTransferred = false;
            try
            {
                var session = await _sessions.GetOrCreateAsync(routingId)
                    .ConfigureAwait(false);
                if (session is not null)
                    ownershipTransferred = session.TryEnqueueControlPacket(
                            header,
                            payload,
                            applicationJobAdmission)
                        == ZLinkSerialPostAdmission.Accepted;
            }
            finally
            {
                if (!ownershipTransferred)
                {
                    DisposeRejectedPacket(header, payload);
                    applicationJobAdmission?.Dispose();
                }
            }
        }) == ZLinkSerialPostAdmission.Accepted)
            return true;

        return false;
    }

    private static bool IsApplicationPacket(Message header)
    {
        try
        {
            var bytes = header.AsReadOnlySpan();
            if (bytes.Length < 2) return false;
            return (ZlinkStreamMessageKind)bytes[1] is
                ZlinkStreamMessageKind.Send or ZlinkStreamMessageKind.Request;
        }
        catch
        {
            return false;
        }
    }

    private void OnMonitorEvent(ZLinkBackendSocketMonitorEvent monitorEvent)
    {
        if (AwaitStateLane(_sessions.IsStoppingAsync())) return;

        switch (monitorEvent.NativeEvent)
        {
            case ZLinkSocketNativeEventType.ConnectionReady:
                if (monitorEvent.RoutingId is RoutingId readyRoutingId)
                {
                    ClearDisconnectedRoutingId(readyRoutingId);
                    var connectedAdmission = _controlIngress.ExecuteControl(async cancellationToken =>
                    {
                        var session = await _sessions.GetOrCreateAsync(readyRoutingId).ConfigureAwait(false);
                        if (session is not null)
                        {
                            var sessionAdmission = session.EnqueueConnected(
                                monitorEvent.LocalAddr,
                                monitorEvent.RemoteAddr);
                            ReportControlAdmission(
                                "stream-session-connected",
                                sessionAdmission);
                        }
                        await ValueTask.CompletedTask;
                    });
                    ReportControlAdmission("stream-monitor-connected", connectedAdmission);
                }
                break;
            case ZLinkSocketNativeEventType.Accepted:
                break;
            case ZLinkSocketNativeEventType.Disconnected:
                if (monitorEvent.RoutingId is { } disconnectedRoutingId)
                {
                    MarkDisconnectedRoutingId(disconnectedRoutingId);
                }
                var disconnectedAdmission = _controlIngress.ExecuteControl(async _ =>
                {
                    if (await _sessions.TryResolveMonitorSessionAsync(monitorEvent.RoutingId)
                            .ConfigureAwait(false) is { } disconnectedSession)
                        disconnectedSession.EnqueueDisconnected(
                            new ZLinkStreamError(
                                ZLinkStreamSessionError.TransportError,
                                monitorEvent.NativeEvent.ToString()));
                });
                ReportControlAdmission("stream-session-disconnected", disconnectedAdmission);
                break;
        }
    }

    private void ReportControlAdmission(
        string operation,
        ZLinkSerialPostAdmission admission)
    {
        if (admission == ZLinkSerialPostAdmission.Accepted)
            return;
        if (admission == ZLinkSerialPostAdmission.Closed
            && (_stopSource.IsCancellationRequested || AwaitStateLane(_sessions.IsStoppingAsync())))
            return;

        _errorSink.ReportRuntimeTaskException(
            operation,
            new ZLinkFrameworkException(
                admission == ZLinkSerialPostAdmission.CapacityExceeded
                    ? ZLinkFrameworkErrorKind.CapacityExceeded
                    : ZLinkFrameworkErrorKind.ShuttingDown,
                admission == ZLinkSerialPostAdmission.CapacityExceeded
                    ? "The STREAM control queue capacity was exceeded."
                    : "The STREAM control queue closed before the monitor event was admitted."));
    }

    private void MarkDisconnectedRoutingId(RoutingId routingId)
    {
        AwaitStateLane(_lane.RunAsync(() =>
        {
            if (_disconnectedRoutingIds.ContainsKey(routingId)) return;
            while (_disconnectedRoutingIdOrder.Count >= DisconnectedRoutingIdLimit)
            {
                var oldest = _disconnectedRoutingIdOrder.First;
                if (oldest is null) break;
                _disconnectedRoutingIdOrder.RemoveFirst();
                _disconnectedRoutingIds.Remove(oldest.Value);
            }

            var node = _disconnectedRoutingIdOrder.AddLast(routingId);
            _disconnectedRoutingIds.Add(routingId, node);
        }));
    }

    private void ClearDisconnectedRoutingId(RoutingId routingId)
    {
        AwaitStateLane(_lane.RunAsync(() =>
        {
            if (!_disconnectedRoutingIds.Remove(routingId, out var node)) return;
            _disconnectedRoutingIdOrder.Remove(node);
        }));
    }

    private sealed class ZLinkStreamPeerAdmissionException(string message)
        : InvalidOperationException(message);

    private async Task RunMonitorLoopAsync(CancellationToken cancellationToken)
    {
        var backoff = new ZLinkPollingBackoff();
        while (!cancellationToken.IsCancellationRequested)
        {
            try
            {
                if (!Monitor.TryRecv(out var monitorEvent))
                {
                    await backoff.NoDataAsync(cancellationToken).ConfigureAwait(false);
                    continue;
                }

                backoff.Reset();
                OnMonitorEvent(monitorEvent);
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
                when (cancellationToken.IsCancellationRequested
                      || ex.Result is ZlinkRecvException.ErrorCode.InternalError
                          or ZlinkRecvException.ErrorCode.InvalidHandle)
            {
                return;
            }
            await Task.Yield();
        }
    }

    private static void DisposeRejectedPacket(
        Message header,
        Message payload)
    {
        header.Dispose();
        payload.Dispose();
    }

    private static T AwaitStateLane<T>(ValueTask<T> operation) =>
        operation.GetAwaiter().GetResult();

    private static void AwaitStateLane(ValueTask operation) =>
        operation.GetAwaiter().GetResult();
}
