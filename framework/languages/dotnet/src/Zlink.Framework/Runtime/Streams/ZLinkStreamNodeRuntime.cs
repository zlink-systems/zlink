using System.Diagnostics;
using Microsoft.Extensions.DependencyInjection;
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
    private readonly ZLinkStreamSessionSerialExecutor _sessionIngress;
    private readonly ZLinkStreamSessionSerialExecutor _controlIngress;
    private readonly CancellationTokenSource _stopSource = new();
    private readonly ZLinkRuntimeTaskRunner _taskRunner;
    private readonly IZLinkRuntimeFailureReporter _errorSink;
    private readonly TimeProvider _timeProvider;
    private readonly string _transport;
    private readonly long _maxMessageSize;
    private readonly ZLinkApplicationJobQueue _applicationJobQueue;
    private readonly bool _ownsApplicationJobQueue;
    private readonly object _receiveStateGate = new();
    private readonly Dictionary<RoutingId, ZLinkStreamReceiveState> _receiveStates = [];
    private readonly Dictionary<RoutingId, LinkedListNode<RoutingId>>
        _disconnectedRoutingIds = [];
    private readonly LinkedList<RoutingId> _disconnectedRoutingIdOrder = [];
    private readonly HashSet<ZLinkStreamReceiveState> _blockedReceiveStates = [];
    private readonly List<ZLinkStreamReceiveState> _receiveStateSnapshot = [];
    private string? _receiveStateCursor;
    private const int DisconnectedRoutingIdLimit = 4096;
    private readonly object _disposeGate = new();
    private Task? _disposeTask;
    private Task? _receiveLoop;
    private Task? _livenessLoop;
    private Task? _monitorLoop;
    private IZLinkBackendSocketPoller? _receivePoller;
    // The binding keeps a multipart receive sequence on the same thread until
    // the part flag is final. This loop is the sole RecvPart caller, so the
    // boundary can remain private to the receive owner.
    private RoutingId? _multipartRoutingId;
    private ZLinkApplicationJobQueueLease? _multipartAdmission;

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
        _sessionIngress = new ZLinkStreamSessionSerialExecutor(runtime.ExecutionOwner, runtime.ErrorSink);
        _controlIngress = new ZLinkStreamSessionSerialExecutor(
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

    internal int SessionCount => _sessions.Count;

    internal void RequestStop()
    {
        _stopSource.Cancel();
        _sessionIngress.RequestStop();
        _controlIngress.RequestStop();
        _sessions.RequestStop();
    }

    internal ValueTask<bool> DrainSessionsAsync(CancellationToken cancellationToken) =>
        _sessions.DrainSessionsAsync(cancellationToken);

    internal void ForceStopSessions() =>
        _sessions.ForceStopSessions();

    public ValueTask DisposeAsync()
    {
        lock (_disposeGate)
            return new ValueTask(_disposeTask ??= DisposeCoreAsync());
    }

    private async Task DisposeCoreAsync()
    {
        var sessions = _sessions.Stop();
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

        Capture(DisposeReceiveStates);
        Capture(() => Interlocked.Exchange(
            ref _multipartAdmission,
            null)?.Dispose());
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
            foreach (var session in _sessions.Snapshot()) session.CheckLiveness();
        }
    }

    private async Task RunReceiveLoopUntilStoppedAsync(CancellationToken runtimeToken)
    {
        using var stop = CancellationTokenSource.CreateLinkedTokenSource(
            _stopSource.Token,
            runtimeToken);
        var backoff = new ZLinkPollingBackoff();
        var failureBackoff = new ZLinkPollingBackoff();
        while (!stop.IsCancellationRequested)
        {
            try
            {
                if (_multipartRoutingId is null)
                    await FlushReceiveStatesAsync(stop.Token)
                        .ConfigureAwait(false);

                backoff.Reset();
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
                    if (_multipartRoutingId is null)
                        await FlushReceiveStatesAsync(stop.Token)
                            .ConfigureAwait(false);
                    RoutingId? routingId = null;
                    ZLinkBackendStreamReceive? received = null;
                    ZLinkApplicationJobQueueLease? suppliedAdmission = null;
                    try
                    {
                        if (_multipartRoutingId is null)
                            suppliedAdmission = await _applicationJobQueue
                                .AcquireAsync(stop.Token)
                                .ConfigureAwait(false);
                        if (!Socket.Recv(
                                out received,
                                RecvFlags.DontWait))
                        {
                            suppliedAdmission?.Dispose();
                            break;
                        }

                        routingId = received?.SourceRoutingId;
                        var hasMore = received?.HasMore == true;
                        if (routingId is not { } sourceRoutingId
                            || received is null
                            || received.Parts.Count == 0)
                        {
                            received?.Dispose();
                            received = null;
                            _multipartRoutingId = null;
                            suppliedAdmission?.Dispose();
                            Interlocked.Exchange(
                                ref _multipartAdmission,
                                null)?.Dispose();
                            _errorSink.ReportRuntimeTaskException(
                                $"stream-recv:{NodeName}",
                                new InvalidDataException(
                                    "STREAM receive did not provide a source routing id or part."));
                            continue;
                        }
                        if (_multipartRoutingId is { } expectedRoutingId
                            && expectedRoutingId != sourceRoutingId)
                        {
                            _multipartRoutingId = null;
                            suppliedAdmission?.Dispose();
                            Interlocked.Exchange(
                                ref _multipartAdmission,
                                null)?.Dispose();
                            received.Dispose();
                            received = null;
                            throw new InvalidDataException(
                                "STREAM multipart receive changed its source routing id.");
                        }
                        if (_multipartRoutingId is null)
                        {
                            _multipartAdmission = suppliedAdmission;
                            suppliedAdmission = null;
                        }
                        _multipartRoutingId = hasMore ? sourceRoutingId : null;
                        var completedAdmission = Interlocked.Exchange(
                            ref _multipartAdmission,
                            null);
                        var receivedBytes = received.Parts.Aggregate(
                            0L,
                            static (total, receivedPart) =>
                                checked(total + Math.Max(receivedPart.Size, 0)));
                        var processing = received;
                        received = null;
                        await ProcessReceivedAsync(
                                sourceRoutingId,
                                processing,
                                completedAdmission,
                                drain: true,
                                stop.Token)
                            .ConfigureAwait(false);
                        batchBytes = checked(batchBytes + receivedBytes);
                    }
                    catch (Exception exception)
                    {
                        received?.Dispose();
                        if (routingId is { } sourceRoutingId)
                        {
                            if (_multipartRoutingId == sourceRoutingId)
                                _multipartRoutingId = null;
                            suppliedAdmission?.Dispose();
                            Interlocked.Exchange(
                                ref _multipartAdmission,
                                null)?.Dispose();
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

    private async ValueTask ProcessReceivedAsync(
        RoutingId routingId,
        ZLinkBackendStreamReceive received,
        ZLinkApplicationJobQueueLease? admission,
        bool drain,
        CancellationToken cancellationToken)
    {
        IDisposable? receivedOwner = received;
        try
        {
            var state = GetOrCreateReceiveState(routingId);
            if (state is null)
                return;
            lock (state.Gate)
            {
                if (state.Removed)
                    return;
                state.Buffer.Append(received.Parts, receivedOwner);
                receivedOwner = null;
            }

            if (!drain)
                return;

            var ownedAdmission = admission;
            admission = null;
            var result = await DrainReceiveStateAsync(
                    state,
                    ownedAdmission,
                    cancellationToken)
                .ConfigureAwait(false);
            if (result != ZLinkReceiveStateDrainResult.Empty)
                MarkReceiveStateBlocked(state);
        }
        finally
        {
            receivedOwner?.Dispose();
            admission?.Dispose();
        }
    }

    private void HandlePeerReceiveFailure(RoutingId routingId, Exception exception)
    {
        MarkDisconnectedRoutingId(routingId);
        RemoveReceiveState(routingId);
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

    private async ValueTask FlushReceiveStatesAsync(
        CancellationToken cancellationToken)
    {
        lock (_receiveStateGate)
        {
            _receiveStateSnapshot.Clear();
            _receiveStateSnapshot.AddRange(_blockedReceiveStates);
            _blockedReceiveStates.Clear();
            _receiveStateSnapshot.Sort(static (left, right) =>
                StringComparer.Ordinal.Compare(
                    left.RoutingId.ToHex(),
                    right.RoutingId.ToHex()));
            RotateReceiveStateSnapshot();
        }

        foreach (var state in _receiveStateSnapshot)
        {
            Exception? failure = null;
            var result = ZLinkReceiveStateDrainResult.Empty;
            try
            {
                result = await DrainReceiveStateAsync(
                        state,
                        suppliedAdmission: null,
                        cancellationToken)
                    .ConfigureAwait(false);
            }
            catch (OperationCanceledException)
                when (cancellationToken.IsCancellationRequested)
            {
                throw;
            }
            catch (Exception exception)
            {
                failure = exception;
            }

            if (failure is not null)
            {
                HandlePeerReceiveFailure(state.RoutingId, failure);
                continue;
            }

            _receiveStateCursor = state.RoutingId.ToHex();
            if (result != ZLinkReceiveStateDrainResult.Empty)
                MarkReceiveStateBlocked(state);
        }
    }

    private async ValueTask<ZLinkReceiveStateDrainResult> DrainReceiveStateAsync(
        ZLinkStreamReceiveState state,
        ZLinkApplicationJobQueueLease? suppliedAdmission,
        CancellationToken cancellationToken)
    {
        var startedAt = Stopwatch.GetTimestamp();
        var count = 0;
        long bytes = 0;
        try
        {
            while (true)
            {
                var acquireAdmission = false;
                lock (state.Gate)
                {
                    if (state.Removed)
                        return ZLinkReceiveStateDrainResult.Empty;

                    if (state.Pending is { } pending)
                    {
                        var pendingBytes = pending.ByteLength;
                        if (!TryAdmitFrame(state.RoutingId, pending))
                            return ZLinkReceiveStateDrainResult.AdmissionBlocked;
                        state.Pending = null;
                        count++;
                        bytes = checked(bytes + pendingBytes);
                        continue;
                    }

                    if (!state.Buffer.TryGetCompleteFrameSize(out var frameBytes))
                    {
                        // No application job materialized from this supply
                        // record. The Framework receive buffer owns the already
                        // dequeued bytes; this finite parse turn releases its Job
                        // Queue reservation without extending Core HWM charge.
                        suppliedAdmission?.Dispose();
                        suppliedAdmission = null;
                        return ZLinkReceiveStateDrainResult.Empty;
                    }
                    else if (ZLinkReceiveBatchBudget.WouldExceed(
                                 count,
                                 bytes,
                                 frameBytes,
                                 startedAt))
                    {
                        return ZLinkReceiveStateDrainResult.BatchExhausted;
                    }
                    else
                    {
                        var frameAdmission = suppliedAdmission;
                        suppliedAdmission = null;
                        if (frameAdmission is null)
                        {
                            acquireAdmission = true;
                        }
                        else
                        {
                            if (!state.Buffer.TryTakeFrame(out var frame))
                            {
                                suppliedAdmission = frameAdmission;
                                return ZLinkReceiveStateDrainResult.Empty;
                            }

                            frame!.ApplicationJobAdmission = frameAdmission;
                            var admittedBytes = frame.ByteLength;
                            if (!TryAdmitFrame(state.RoutingId, frame))
                            {
                                state.Pending = frame;
                                return ZLinkReceiveStateDrainResult.AdmissionBlocked;
                            }
                            count++;
                            bytes = checked(bytes + admittedBytes);
                        }
                    }
                }

                if (!acquireAdmission)
                    continue;

                var acquired = await _applicationJobQueue
                    .AcquireAsync(cancellationToken)
                    .ConfigureAwait(false);
                lock (state.Gate)
                {
                    if (state.Removed)
                    {
                        acquired.Dispose();
                        return ZLinkReceiveStateDrainResult.Empty;
                    }
                    suppliedAdmission = acquired;
                }
            }
        }
        finally
        {
            suppliedAdmission?.Dispose();
        }
    }

    private void RotateReceiveStateSnapshot()
    {
        if (_receiveStateCursor is null || _receiveStateSnapshot.Count < 2)
            return;
        var start = _receiveStateSnapshot.FindIndex(state =>
            StringComparer.Ordinal.Compare(
                state.RoutingId.ToHex(),
                _receiveStateCursor) > 0);
        if (start < 0) start = 0;
        if (start == 0) return;
        var rotated = _receiveStateSnapshot.Skip(start).Concat(
            _receiveStateSnapshot.Take(start)).ToArray();
        _receiveStateSnapshot.Clear();
        _receiveStateSnapshot.AddRange(rotated);
    }

    private enum ZLinkReceiveStateDrainResult
    {
        Empty = 0,
        BatchExhausted = 1,
        AdmissionBlocked = 2
    }

    private bool TryAdmitFrame(
        RoutingId routingId,
        ZLinkStreamInboundFrame frame)
    {
        var header = frame.Header
            ?? throw new InvalidOperationException("STREAM frame header ownership was lost.");
        var payload = frame.Payload
            ?? throw new InvalidOperationException("STREAM frame payload ownership was lost.");

        if (_stopSource.IsCancellationRequested || _sessions.IsStopping)
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
                    frame.ApplicationJobAdmission,
                    frame.PayloadOwner);
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
        var payloadOwner = frame.PayloadOwner;
        if (_sessions.TryGet(routingId, out var existing))
            {
                var admission = existing.TryEnqueuePacket(
                    header,
                    payload,
                    applicationJobAdmission,
                    payloadOwner);
                if (admission == ZLinkSerialPostAdmission.Accepted)
                {
                    return true;
                }
                throw new ZLinkStreamPeerAdmissionException(
                    "STREAM peer session queue is closed.");
            }

            var ingressAdmission = _sessionIngress.EnqueueApplication(
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
                                    applicationJobAdmission,
                                    payloadOwner)
                                == ZLinkSerialPostAdmission.Accepted;
                    }
                    finally
                    {
                        if (!ownershipTransferred)
                        {
                            DisposeRejectedPacket(header, payload);
                            applicationJobAdmission?.Dispose();
                            payloadOwner?.Dispose();
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
        ZLinkApplicationJobQueueLease? applicationJobAdmission,
        IDisposable? payloadOwner)
    {
        if (_sessions.TryGet(routingId, out var existing))
        {
            var admission = existing.TryEnqueueControlPacket(
                header,
                payload,
                applicationJobAdmission,
                payloadOwner);
            if (admission == ZLinkSerialPostAdmission.Accepted) return true;
            throw new ZLinkStreamPeerAdmissionException(
                "STREAM peer control queue is closed.");
        }

        if (_controlIngress.EnqueueControl(async cancellationToken =>
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
                            applicationJobAdmission,
                            payloadOwner)
                        == ZLinkSerialPostAdmission.Accepted;
            }
            finally
            {
                if (!ownershipTransferred)
                {
                    DisposeRejectedPacket(header, payload);
                    applicationJobAdmission?.Dispose();
                    payloadOwner?.Dispose();
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
        if (_sessions.IsStopping) return;

        switch (monitorEvent.NativeEvent)
        {
            case ZLinkSocketNativeEventType.ConnectionReady:
                if (monitorEvent.RoutingId is RoutingId readyRoutingId)
                {
                    ClearDisconnectedRoutingId(readyRoutingId);
                    var connectedAdmission = _controlIngress.EnqueueControl(async cancellationToken =>
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
                    RemoveReceiveState(disconnectedRoutingId);
                }
                var disconnectedAdmission = _controlIngress.EnqueueControl(_ =>
                {
                    if (_sessions.TryResolveMonitorSession(monitorEvent.RoutingId, out var disconnectedSession))
                        disconnectedSession.EnqueueDisconnected(
                            new ZLinkStreamError(
                                ZLinkStreamSessionError.TransportError,
                                monitorEvent.NativeEvent.ToString()));
                    return ValueTask.CompletedTask;
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
            && (_stopSource.IsCancellationRequested || _sessions.IsStopping))
            return;

        _errorSink.ReportRuntimeTaskException(
            operation,
            new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ShuttingDown,
                "The STREAM control queue closed before the monitor event was admitted."));
    }

    private ZLinkStreamReceiveState? GetOrCreateReceiveState(RoutingId routingId)
    {
        lock (_receiveStateGate)
        {
            if (_disconnectedRoutingIds.ContainsKey(routingId)) return null;
            if (_receiveStates.TryGetValue(routingId, out var existing))
                return existing;

            var created = new ZLinkStreamReceiveState(routingId, _maxMessageSize);
            _receiveStates.Add(routingId, created);
            return created;
        }
    }

    private void RemoveReceiveState(RoutingId routingId)
    {
        ZLinkStreamReceiveState? state;
        lock (_receiveStateGate)
        {
            if (!_receiveStates.Remove(routingId, out state)) return;
            _blockedReceiveStates.Remove(state);
        }

        state.Dispose();
    }

    private void DisposeReceiveStates()
    {
        ZLinkStreamReceiveState[] states;
        lock (_receiveStateGate)
        {
            states = _receiveStates.Values.ToArray();
            _receiveStates.Clear();
            _blockedReceiveStates.Clear();
            _receiveStateSnapshot.Clear();
        }

        foreach (var state in states) state.Dispose();
    }

    private void MarkReceiveStateBlocked(ZLinkStreamReceiveState state)
    {
        lock (_receiveStateGate)
            _blockedReceiveStates.Add(state);
    }

    private void MarkDisconnectedRoutingId(RoutingId routingId)
    {
        lock (_receiveStateGate)
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
        }
    }

    private void ClearDisconnectedRoutingId(RoutingId routingId)
    {
        lock (_receiveStateGate)
        {
            if (!_disconnectedRoutingIds.Remove(routingId, out var node)) return;
            _disconnectedRoutingIdOrder.Remove(node);
        }
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
}
