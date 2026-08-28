using Zlink.Framework.Runtime.Backend.DotNet.Wrappers;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Messaging;

namespace Zlink.Framework.Runtime.Channels;

internal sealed class ZLinkClientServerClientRuntime : IAsyncDisposable
{
    private static readonly TimeSpan ControlReceivePollInterval =
        TimeSpan.FromMilliseconds(100);
    private readonly ZLinkChannelName _channelName;
    private readonly IZLinkMonitoringBackendAdapter _monitoring;
    private readonly IZLinkBackendRuntimeContext _context;
    private readonly IZLinkSocketConfig _socketConfig;
    private readonly ZLinkApplicationJobQueue _applicationJobQueue;
    private readonly TimeSpan _requestTimeout;
    private readonly CancellationToken _stopToken;
    private readonly ZLinkStateLane _lane = new();
    private readonly Dictionary<string, Connection> _connections =
        new(StringComparer.Ordinal);
    private readonly List<Task> _retired = [];
    private ZLinkWeightedSelectionPlan<Connection, string>?
        _readySelectionPlan;
    private long _selectionRevision;
    private long _readySelectionPlanRevision = -1;
    private int _pendingRequests;
    private bool _disposed;
    private IDisposable? _manualConnectionAttachment;
    private Task? _disposeTask;
    private readonly ZLinkMessageFlowTracer? _flow;

    // Monitoring subscribers use this edge notification to request a fresh
    // snapshot. The callback only signals a bounded channel; it never reads
    // connection state while a connection lock is held.
    internal event Action? StateChanged;

    internal ZLinkClientServerClientRuntime(
        string channelName,
        IZLinkMonitoringBackendAdapter monitoring,
        IZLinkBackendRuntimeContext context,
        IZLinkSocketConfig socketConfig,
        TimeSpan requestTimeout,
        CancellationToken stopToken,
        ZLinkApplicationJobQueue applicationJobQueue,
        ZLinkMessageFlowTracer? flow = null)
    {
        _channelName = ZLinkChannelName.FromBoundary(
            channelName,
            nameof(channelName));
        _monitoring = monitoring;
        _context = context;
        _socketConfig = socketConfig;
        _requestTimeout = requestTimeout;
        _stopToken = stopToken;
        _applicationJobQueue = applicationJobQueue;
        _flow = flow;
    }

    internal void AddManual(string endpoint) =>
        AddOrReplace(
            $"manual:{endpoint}",
            endpoint,
            expected: null);

    internal void RemoveManual(string endpoint) =>
        Remove($"manual:{endpoint}");

    internal void AddLocal(
        string endpoint,
        ZLinkClientServerServerIdentity identity)
    {
        var snapshot = AwaitStateLane(identity.ReadAsync());
        var key =
            $"local:{identity.ServerRid.ToHex()}:{identity.LifecycleGeneration}";
        AddOrReplace(
            key,
            endpoint,
            LocalDescriptor(identity, endpoint, snapshot));
        identity.SnapshotChanged += changed =>
        {
            RunState(() =>
            {
                if (_connections.TryGetValue(key, out var connection))
                    connection.Update(LocalDescriptor(
                        identity,
                        endpoint,
                        changed));
            });
        };
    }

    private ZLinkClientServerServerDescriptor LocalDescriptor(
        ZLinkClientServerServerIdentity identity,
        string endpoint,
        ZLinkClientServerServerIdentity.Snapshot snapshot) =>
        new(
                _channelName.Value,
                identity.ServerRid,
                identity.LifecycleGeneration,
                snapshot.Revision,
                endpoint,
                snapshot.Weight,
                snapshot.State,
                identity.SecurityIdentity,
                "process-local",
                1,
                default);

    internal void ReplaceAutomatic(
        IReadOnlyList<ZLinkClientServerServerDescriptor> descriptors)
    {
        var desired = descriptors.ToDictionary(
            static row => $"auto:{row.ServerRid.ToHex()}:{row.LifecycleGeneration}",
            StringComparer.Ordinal);
        var successors = descriptors.ToDictionary(
            static row => row.ServerRid,
            static row => $"auto:{row.ServerRid.ToHex()}:{row.LifecycleGeneration}");
        string[] obsolete;
        obsolete = RunState(() => _connections.Keys
                .Where(static key => key.StartsWith("auto:", StringComparison.Ordinal))
                .Where(key => !desired.ContainsKey(key))
                .ToArray());

        // Start successors before removing the previous lifecycle. Ready
        // publication is fenced inside each connection.
        foreach (var (key, descriptor) in desired)
            AddOrReplace(key, descriptor.Endpoint, descriptor);
        foreach (var key in obsolete)
        {
            var retainForSuccessor = RunState(() =>
            {
                if (_connections.TryGetValue(key, out var obsoleteConnection)
                    && obsoleteConnection.ExpectedServerRid is { } rid
                    && successors.TryGetValue(rid, out var successorKey)
                    && _connections.TryGetValue(
                        successorKey,
                        out var successor))
                    return !successor.AdmissionCompleted;
                return false;
            });
            if (!retainForSuccessor)
                Remove(key);
        }
    }

    internal async ValueTask<ZLinkOneWaySubmitResult> SendAsync(
        IReadOnlyList<Message> parts,
        CancellationToken cancellationToken)
    {
        var target = await WaitForReadyAsync(cancellationToken)
            .ConfigureAwait(false);
        if (target is null)
        {
            ZLinkMessageParts.DisposeAll(parts);
            return new ZLinkOneWaySubmitResult(ZLinkOneWaySubmitStatus.TargetNotFound);
        }
        if (!ZLinkClientServerMessageBound.Fits(
                parts,
                target.AdmittedMaximumMessageBytes))
        {
            ZLinkMessageParts.DisposeAll(parts);
            throw ZLinkClientServerMessageBound.CreateExceededException(
                target.AdmittedMaximumMessageBytes);
        }
        // Capture the trace fields before the transport takes ownership of the
        // parts; emit `sent` only after the local transport accepts them.
        var sentPacketName = _flow?.Enabled(ZLinkMessageFlowOutcome.Sent) == true
            ? ZLinkFrameworkRuntime.TryReadEnvelopePacketName(parts)
            : null;
        try
        {
            await target.Socket.Send()
                .Messages(parts)
                .Async(cancellationToken)
                .ConfigureAwait(false);
            if (sentPacketName is not null
                && _flow!.Enabled(ZLinkMessageFlowOutcome.Sent))
                _flow.Trace(new ZLinkMessageFlowEvent(
                    ZLinkMessageFlowOutcome.Sent,
                    ZLinkDispatchErrorSurface.Channel,
                    ZLinkDispatchMessageKind.Send,
                    sentPacketName,
                    _channelName.Value,
                    ServerRid: target.SelectionServerRid?.ToString()));
            return new ZLinkOneWaySubmitResult(
                ZLinkOneWaySubmitStatus.Submitted);
        }
        catch (ZlinkSubmitException error)
        {
            return new ZLinkOneWaySubmitResult(error.Result switch
            {
                ZlinkSubmitException.ErrorCode.Backpressured =>
                    ZLinkOneWaySubmitStatus.TimedOut,
                ZlinkSubmitException.ErrorCode.NotFound =>
                    ZLinkOneWaySubmitStatus.TargetNotFound,
                ZlinkSubmitException.ErrorCode.NotConnected =>
                    ZLinkOneWaySubmitStatus.RouteNotConnected,
                ZlinkSubmitException.ErrorCode.Terminated =>
                    ZLinkOneWaySubmitStatus.Shutdown,
                _ => throw ZLinkRequestFailureMapper.CreateSubmitException(
                    error,
                    $"ClientServer send failed for '{_channelName}'")
            });
        }
        catch (ObjectDisposedException)
        {
            return new ZLinkOneWaySubmitResult(
                ZLinkOneWaySubmitStatus.Shutdown);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }
    }

    internal async ValueTask<IReadOnlyList<Message>> RequestAsync(
        IReadOnlyList<Message> parts,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        Interlocked.Increment(ref _pendingRequests);
        SignalStateChanged();
        using var readyWaitCancellation =
            CancellationTokenSource.CreateLinkedTokenSource(
                cancellationToken,
                _stopToken);
        try
        {
            var deadline = DateTime.UtcNow + timeout;
            var target = await WaitForReadyAsync(
                    timeout,
                    readyWaitCancellation.Token)
                .ConfigureAwait(false);
            if (target is null)
            {
                ZLinkMessageParts.DisposeAll(parts);
                throw ZLinkRequestFailureMapper.CreateTimedOutRequestException(
                    $"ClientServer channel '{_channelName}' had no ready server before the request deadline.");
            }
            if (!ZLinkClientServerMessageBound.Fits(
                    parts,
                    target.AdmittedMaximumMessageBytes))
            {
                ZLinkMessageParts.DisposeAll(parts);
                throw ZLinkClientServerMessageBound.CreateExceededException(
                    target.AdmittedMaximumMessageBytes);
            }
            var remaining = deadline - DateTime.UtcNow;
            if (remaining <= TimeSpan.Zero)
            {
                ZLinkMessageParts.DisposeAll(parts);
                throw ZLinkRequestFailureMapper.CreateTimedOutRequestException(
                    $"ClientServer channel '{_channelName}' had no request time remaining after route admission.");
            }
            if (_flow?.Enabled(ZLinkMessageFlowOutcome.Sent) == true
                && ZLinkFrameworkRuntime.TryReadEnvelopeHeaderForTrace(parts)
                    is { } sentHeader)
                _flow.Trace(new ZLinkMessageFlowEvent(
                    ZLinkMessageFlowOutcome.Sent,
                    ZLinkDispatchErrorSurface.Channel,
                    ZLinkDispatchMessageKind.Request,
                    sentHeader.MessageName,
                    _channelName.Value,
                    CorrelationId: sentHeader.CorrelationId,
                    ServerRid: target.SelectionServerRid?.ToString()));
            return await ZLinkRawRequestSubmitter.SubmitAsync(
                    parts,
                    (pending, nativeTimeout, token) => target.Socket.Request()
                        .Messages(pending)
                        .Timeout(nativeTimeout)
                        .Async(token),
                    remaining,
                    $"ClientServer request failed for '{_channelName}': {{0}}.",
                    cancellationToken)
                .ConfigureAwait(false);
        }
        catch (OperationCanceledException)
            when (_stopToken.IsCancellationRequested
                  && !cancellationToken.IsCancellationRequested)
        {
            throw ZLinkRequestFailureMapper.CreateShutdownRequestException(
                $"ClientServer channel '{_channelName}' request was interrupted by runtime shutdown.");
        }
        finally
        {
            Interlocked.Decrement(ref _pendingRequests);
            SignalStateChanged();
        }
    }

    internal int PendingRequestCount => Volatile.Read(ref _pendingRequests);

    internal IReadOnlyList<ZLinkClientServerConnectionSnapshot>
        SnapshotConnections() =>
        RunState(() => DistinctConnections()
                .Select(static connection => connection.Snapshot())
                .OrderBy(
                    static value => value.ServerRid?.ToHex(),
                    StringComparer.Ordinal)
                .ThenBy(static value => value.Endpoint, StringComparer.Ordinal)
                .ToArray());

    internal int ReadyCount
    {
        get => RunState(
            () => DistinctConnections().Count(static value => value.Ready));
    }

    internal int AdmissionCompletedCount
    {
        get => RunState(() => DistinctConnections().Count(
            static value => value.AdmissionCompleted));
    }

    internal int ConnectionIntentCount
    {
        get => RunState(() => _connections.Count);
    }

    internal int PhysicalConnectionCount
    {
        get => RunState(() => DistinctConnections().Count());
    }

    internal long LivenessAckCount
    {
        get => RunState(() => DistinctConnections().Sum(
            static connection => connection.LivenessAckCount));
    }

    internal long ReceivedLivenessProbeCount
    {
        get => RunState(() => DistinctConnections().Sum(
            static connection => connection.ReceivedLivenessProbeCount));
    }

    internal long SentLivenessProbeCount
    {
        get => RunState(() => DistinctConnections().Sum(
            static connection => connection.SentLivenessProbeCount));
    }

    internal string AdmissionDiagnostics
    {
        get => RunState(() => string.Join(
                    "; ",
                    _connections.Select(
                        static entry =>
                            $"{entry.Key}={entry.Value.Diagnostics}")));
    }

    internal IAsyncDisposable GetMonitoringSocket()
    {
        return RunState(() => _connections.Values.FirstOrDefault()?.Socket
                   ?? throw new InvalidOperationException(
                       $"ClientServer client '{_channelName}' has no connection intent to monitor."));
    }

    public ValueTask DisposeAsync()
    {
        Task task = Task.CompletedTask;
        TaskCompletionSource? start = null;
        IDisposable? attachment = null;
        RunState(() =>
        {
            if (_disposeTask is null)
            {
                _disposed = true;
                attachment = _manualConnectionAttachment;
                _manualConnectionAttachment = null;
                start = new TaskCompletionSource(
                    TaskCreationOptions.RunContinuationsAsynchronously);
                using (ExecutionContext.SuppressFlow())
                    _disposeTask = DisposeCoreAsync(start.Task);
            }
            task = _disposeTask;
        });
        attachment?.Dispose();
        start?.TrySetResult();
        return new ValueTask(task);
    }

    internal void OwnManualConnectionAttachment(IDisposable attachment)
    {
        ArgumentNullException.ThrowIfNull(attachment);
        var dispose = false;
        IDisposable? previous = null;
        RunState(() =>
        {
            if (_disposed)
                dispose = true;
            else
            {
                previous = _manualConnectionAttachment;
                _manualConnectionAttachment = attachment;
            }
        });
        previous?.Dispose();
        if (dispose)
        {
            attachment.Dispose();
            throw new ObjectDisposedException(nameof(ZLinkClientServerClientRuntime));
        }
    }

    private async Task DisposeCoreAsync(Task started)
    {
        await started.ConfigureAwait(false);
        Connection[] values = [];
        Task[] retired = [];
        RunState(() =>
        {
            values = DistinctConnections().ToArray();
            _connections.Clear();
            retired = _retired.ToArray();
            _retired.Clear();
        });

        var failures = new ZLinkFailureCollector();
        foreach (var value in values)
            await failures.CaptureAsync(value.DisposeAsync).ConfigureAwait(false);
        foreach (var task in retired)
            await failures.CaptureAsync(
                    () => new ValueTask(task))
                .ConfigureAwait(false);
        failures.ThrowIfAny();
        await _lane.DisposeAsync().ConfigureAwait(false);
    }

    private void AddOrReplace(
        string key,
        string endpoint,
        ZLinkClientServerServerDescriptor? expected)
    {
        Connection? previous = null;
        var retirePrevious = false;
        Connection? created = null;
        RunState(() =>
        {
            if (_disposed)
                return;
            if (_connections.TryGetValue(key, out var existing)
                && existing.Matches(endpoint, expected))
            {
                existing.Update(expected);
                return;
            }
            previous = existing;
            created = new Connection(
                _channelName.Value,
                endpoint,
                expected,
                _context.CreateDealerSocket(),
                _monitoring,
                _socketConfig,
                _stopToken,
                _applicationJobQueue,
                OnAdmitted,
                InvalidateSelectionCache);
            _connections[key] = created;
            InvalidateSelectionCache();
            retirePrevious = previous is not null
                && !IsReferenced(previous);
            if (retirePrevious)
                RegisterRetirement(previous!);
        });
        created?.Start();
    }

    private void Remove(string key)
    {
        Connection? removed;
        RunState(() =>
        {
            if (_disposed)
                return;
            if (!_connections.Remove(key, out removed))
                return;
            InvalidateSelectionCache();
            if (IsReferenced(removed))
                return;
            RegisterRetirement(removed);
        });
    }

    private void RegisterRetirement(Connection connection)
    {
        var task = connection.DisposeAsync().AsTask();
        _retired.RemoveAll(static candidate => candidate.IsCompleted);
        _retired.Add(task);
    }

    private Connection? SelectReady()
    {
        return RunState(() =>
        {
            var revision = Volatile.Read(ref _selectionRevision);
            if (_readySelectionPlanRevision != revision)
            {
                var retainedCurrents = _readySelectionPlan?.CaptureCurrents();
                var candidates = DistinctConnections()
                    .Where(static value => value.Ready && value.Weight > 0)
                    .OrderBy(
                        static value => value.SelectionServerRid?.ToHex(),
                        StringComparer.Ordinal)
                    .ToArray();
                _readySelectionPlan = new ZLinkWeightedSelectionPlan<
                    Connection,
                    string>(
                    candidates,
                    static value => value.Weight,
                    static value => value.SelectionServerRid?.ToHex()
                        ?? throw new InvalidOperationException(
                            "A ready ClientServer connection has no Server RID."),
                    retainedCurrents,
                    StringComparer.Ordinal,
                    StringComparer.Ordinal);
                _readySelectionPlanRevision = revision;
            }
            return _readySelectionPlan!.Select();
        });
    }

    private void InvalidateSelectionCache()
    {
        Interlocked.Increment(ref _selectionRevision);
        SignalStateChanged();
    }

    private void SignalStateChanged() => StateChanged?.Invoke();

    private IEnumerable<Connection> DistinctConnections() =>
        _connections.Values.Distinct(
            (IEqualityComparer<Connection>)ReferenceEqualityComparer.Instance);

    private bool IsReferenced(Connection connection) =>
        _connections.Values.Any(
            candidate => ReferenceEquals(candidate, connection));

    private void OnAdmitted(
        Connection admitted,
        string identity)
    {
        Connection? duplicate = null;
        RunState(() =>
        {
            if (_disposed)
                return;
            if (!IsReferenced(admitted))
                return;
            var canonical = DistinctConnections().FirstOrDefault(
                candidate => !ReferenceEquals(candidate, admitted)
                    && StringComparer.Ordinal.Equals(
                        candidate.AdmittedIdentity,
                        identity));
            if (canonical is null)
                return;

            canonical.MergeExpected(admitted.Expected);
            foreach (var key in _connections
                         .Where(entry => ReferenceEquals(entry.Value, admitted))
                         .Select(static entry => entry.Key)
                         .ToArray())
                _connections[key] = canonical;
            duplicate = admitted;
            RegisterRetirement(duplicate);
        });
    }

    private T RunState<T>(Func<T> work) =>
        AwaitStateLane(_lane.RunAsync(work));

    private void RunState(Action work) =>
        AwaitStateLane(_lane.RunAsync(work));

    private static T AwaitStateLane<T>(ValueTask<T> operation) =>
        operation.GetAwaiter().GetResult();

    private static void AwaitStateLane(ValueTask operation) =>
        operation.GetAwaiter().GetResult();

    private async ValueTask<Connection?> WaitForReadyAsync(
        CancellationToken cancellationToken) =>
        await WaitForReadyAsync(
                _requestTimeout < TimeSpan.FromSeconds(5)
                    ? _requestTimeout
                    : TimeSpan.FromSeconds(5),
                cancellationToken)
            .ConfigureAwait(false);

    private async ValueTask<Connection?> WaitForReadyAsync(
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        var deadline = DateTime.UtcNow + timeout;
        while (true)
        {
            if (SelectReady() is { } ready)
                return ready;
            if (DateTime.UtcNow >= deadline)
                return null;
            await Task.Delay(
                    TimeSpan.FromMilliseconds(5),
                    cancellationToken)
                .ConfigureAwait(false);
        }
    }

    private sealed class Connection : IAsyncDisposable
    {
        private readonly string _channelName;
        private readonly string _endpoint;
        private readonly TimeSpan _admissionTimeout;
        private readonly CancellationToken _stopToken;
        private readonly uint _normalizedEffectiveMaxMessageBytes;
        private readonly ZLinkStateLane _lane = new();
        private readonly object _socketLifecycleGate = new();
        private readonly IZLinkBackendSocketMonitor _monitor;
        private ZLinkClientServerServerDescriptor? _expected;
        private bool _disposed;
        private Task? _disposeTask;
        private bool _admissionStarted;
        private bool _admissionCompleted;
        private bool _ready;
        private bool _rejected;
        private int _weight;
        private string _diagnostics = "configured";
        private string? _admittedIdentity;
        private ZLinkClientServerControlProtocol.Admission? _currentAdmission;
        private readonly CancellationTokenSource _admissionStop;
        private Task? _admissionTask;
        private readonly List<Task> _admissionTasks = [];
        private readonly List<Task> _retryTasks = [];
        private bool _retryScheduled;
        private Task? _controlTask;
        private Task? _livenessTask;
        private Task? _reconnectTask;
        private bool _reconnectInProgress;
        private readonly Action<Connection, string> _onAdmitted;
        private readonly Action _onSelectionChanged;
        private ulong _nextProbeId = 1;
        private ulong? _outstandingProbeId;
        private DateTimeOffset _peerDeadline;
        private long _livenessAckCount;
        private long _receivedLivenessProbeCount;
        private long _sentLivenessProbeCount;
        private ulong _physicalGeneration = 1;
        private ulong _admissionAttempt;
        private IDisposable? _receiveFlowRegistration;

        internal Connection(
            string channelName,
            string endpoint,
            ZLinkClientServerServerDescriptor? expected,
            IDealerSocket socket,
            IZLinkMonitoringBackendAdapter monitoring,
            IZLinkSocketConfig socketConfig,
            CancellationToken stopToken,
            ZLinkApplicationJobQueue applicationJobQueue,
            Action<Connection, string> onAdmitted,
            Action onSelectionChanged)
        {
            _channelName = channelName;
            _endpoint = endpoint;
            _expected = expected;
            _admissionTimeout = socketConfig.ConnectTimeout
                ?? TimeSpan.FromSeconds(1);
            _stopToken = stopToken;
            _onAdmitted = onAdmitted;
            _onSelectionChanged = onSelectionChanged;
            _admissionStop =
                CancellationTokenSource.CreateLinkedTokenSource(stopToken);
            _normalizedEffectiveMaxMessageBytes =
                ZLinkClientServerControlProtocol.NormalizeMaximumMessageBytes(
                    socketConfig.MaxMessageSize);
            Socket = socket;
            Socket.SetRoutingId(RoutingId.From($"csc-{Guid.NewGuid():N}"));
            ZLinkChannelBundleFactory.ApplySocketConfig(Socket.Options, socketConfig);
            Socket.Options.Probe = false;
            _monitor = monitoring.OpenSocketMonitor(Socket);
            _monitor.OnEvent(OnMonitorEvent);
            _receiveFlowRegistration =
                applicationJobQueue.RegisterReceiveFlowSocket(Socket);
        }

        internal IDealerSocket Socket { get; }
        internal bool Ready => RunState(() => _ready && !_disposed);
        internal uint AdmittedMaximumMessageBytes
        {
            get => RunState(() =>
                _currentAdmission?.NormalizedEffectiveMaxMessageBytes
                ?? _normalizedEffectiveMaxMessageBytes);
        }
        internal bool AdmissionCompleted
        {
            get => RunState(() => _admissionCompleted);
        }
        internal int Weight => RunState(() => _weight);
        internal RoutingId? SelectionServerRid
        {
            get => RunState(() =>
                _currentAdmission?.ServerRid ?? _expected?.ServerRid);
        }
        internal string Diagnostics
        {
            get => RunState(() =>
                $"{_diagnostics};generation={_physicalGeneration};"
                        + $"attempt={_admissionAttempt};"
                        + $"admissionStarted={_admissionStarted};"
                        + $"admissionCompleted={_admissionCompleted};"
                        + $"reconnect={_reconnectInProgress};"
                        + $"current={_currentAdmission is not null}");
        }
        internal RoutingId? ExpectedServerRid
        {
            get => RunState(() => _expected?.ServerRid);
        }
        internal string? AdmittedIdentity
        {
            get => RunState(() => _admittedIdentity);
        }
        internal ZLinkClientServerServerDescriptor? Expected
        {
            get => RunState(() => _expected);
        }
        internal long LivenessAckCount =>
            Interlocked.Read(ref _livenessAckCount);
        internal long ReceivedLivenessProbeCount =>
            Interlocked.Read(ref _receivedLivenessProbeCount);
        internal long SentLivenessProbeCount =>
            Interlocked.Read(ref _sentLivenessProbeCount);

        internal ZLinkClientServerConnectionSnapshot Snapshot()
        {
            return RunState(() =>
            {
                var admission = _currentAdmission;
                var expected = _expected;
                var state = _rejected
                    ? ZLinkClientServerServerState.Rejected
                    : _ready
                        ? ZLinkClientServerServerState.Ready
                        : admission?.State == ZLinkFrameworkRuntimeState.Draining
                            ? ZLinkClientServerServerState.Draining
                            : _admissionStarted
                                ? ZLinkClientServerServerState.Connecting
                                : _admissionCompleted
                                    ? ZLinkClientServerServerState.Disconnected
                                    : ZLinkClientServerServerState.Configured;
                return new ZLinkClientServerConnectionSnapshot(
                    admission?.ServerRid ?? expected?.ServerRid,
                    admission?.LifecycleGeneration
                    ?? expected?.LifecycleGeneration,
                    admission?.DescriptorRevision
                    ?? expected?.DescriptorRevision,
                    admission?.AdvertisedEndpoint
                    ?? expected?.Endpoint
                    ?? _endpoint,
                    _weight,
                    _ready,
                    state,
                    expected?.OwnerId == "process-local"
                        ? "manual"
                        : expected is null
                            ? "manual"
                            : "redis",
                    _ready ? null : _diagnostics);
            });
        }

        internal bool Matches(
            string endpoint,
            ZLinkClientServerServerDescriptor? expected)
        {
            return RunState(() => StringComparer.Ordinal.Equals(_endpoint, endpoint)
                    && !_rejected
                    && (_expected is null && expected is null
                        || _expected is not null
                        && expected is not null
                        && _expected.ServerRid == expected.ServerRid
                        && _expected.LifecycleGeneration
                        == expected.LifecycleGeneration
                        && StringComparer.Ordinal.Equals(
                            _expected.SecurityIdentity,
                            expected.SecurityIdentity)));
        }

        internal void Update(ZLinkClientServerServerDescriptor? expected)
        {
            RunState(() =>
            {
                _expected = expected;
                if (expected is not null)
                {
                    _weight = expected.Weight;
                    if (expected.State != ZLinkFrameworkRuntimeState.Serving
                        || expected.Weight <= 0)
                        _ready = false;
                    else if (_admissionCompleted)
                        _ready = true;
                }
            });
            _onSelectionChanged();
        }

        internal void MergeExpected(
            ZLinkClientServerServerDescriptor? expected)
        {
            if (expected is null)
                return;
            RunState(() =>
            {
                if (_admittedIdentity is not null
                    && !StringComparer.Ordinal.Equals(
                        _admittedIdentity,
                        IdentityOf(
                            expected.ServerRid,
                            expected.LifecycleGeneration)))
                    return;
                _expected = expected;
                _weight = expected.Weight;
                _ready = _admissionCompleted
                    && expected.State == ZLinkFrameworkRuntimeState.Serving
                    && expected.Weight > 0;
            });
            _onSelectionChanged();
        }

        internal void Start()
        {
            lock (_socketLifecycleGate)
            {
                if (RunState(() => _disposed))
                        return;
                Socket.Connect(_endpoint);
            }
            // Native monitor delivery can race the first connected edge. The
            // adapter schedules the same fenced admission attempt so readiness
            // does not depend on which binding notification arrives first.
            ScheduleAdmissionRetry();
        }

        public ValueTask DisposeAsync()
        {
            Task task = Task.CompletedTask;
            TaskCompletionSource? start = null;
            RunState(() =>
            {
                if (_disposeTask is null)
                {
                    _disposed = true;
                    _ready = false;
                    _onSelectionChanged();
                    start = new TaskCompletionSource(
                        TaskCreationOptions.RunContinuationsAsynchronously);
                    using (ExecutionContext.SuppressFlow())
                        _disposeTask = DisposeCoreAsync(start.Task);
                }
                task = _disposeTask;
            });
            start?.TrySetResult();
            return new ValueTask(task);
        }

        private async Task DisposeCoreAsync(Task started)
        {
            await started.ConfigureAwait(false);
            var failures = new ZLinkFailureCollector();
            await failures.CaptureAsync(
                    () => new ValueTask(_admissionStop.CancelAsync()))
                .ConfigureAwait(false);
            Task[] admissionTasks;
            Task[] retryTasks;
            Task? controlTask;
            Task? livenessTask;
            Task? reconnectTask;
            (admissionTasks, retryTasks, controlTask, livenessTask, reconnectTask) =
                RunState(() =>
                {
                admissionTasks = _admissionTasks.ToArray();
                retryTasks = _retryTasks.ToArray();
                return (
                    admissionTasks,
                    retryTasks,
                    _controlTask,
                    _livenessTask,
                    _reconnectTask);
                });
            foreach (var admissionTask in admissionTasks)
                await failures.CaptureAsync(
                        () => new ValueTask(
                            IgnoreCancellationAsync(admissionTask)))
                    .ConfigureAwait(false);
            foreach (var retryTask in retryTasks)
                await failures.CaptureAsync(
                        () => new ValueTask(
                            IgnoreCancellationAsync(retryTask)))
                    .ConfigureAwait(false);
            if (controlTask is not null)
                await failures.CaptureAsync(
                        () => new ValueTask(
                            IgnoreCancellationAsync(controlTask)))
                    .ConfigureAwait(false);
            if (livenessTask is not null)
                await failures.CaptureAsync(
                        () => new ValueTask(
                            IgnoreCancellationAsync(livenessTask)))
                    .ConfigureAwait(false);
            if (reconnectTask is not null)
                await failures.CaptureAsync(
                        () => new ValueTask(
                            IgnoreCancellationAsync(reconnectTask)))
                    .ConfigureAwait(false);
            if (_receiveFlowRegistration is not null)
                failures.Capture(_receiveFlowRegistration.Dispose);
            lock (_socketLifecycleGate)
            {
                try
                {
                    Socket.Disconnect(_endpoint);
                }
                catch
                {
                }
            }
            await failures.CaptureAsync(_monitor.DisposeAsync)
                .ConfigureAwait(false);
            await failures.CaptureAsync(DisposeSocketAsync)
                .ConfigureAwait(false);
            failures.Capture(_admissionStop.Dispose);
            failures.ThrowIfAny();
            await _lane.DisposeAsync().ConfigureAwait(false);
        }

        private async ValueTask DisposeSocketAsync()
        {
            while (true)
            {
                try
                {
                    await Socket.DisposeAsync().ConfigureAwait(false);
                    return;
                }
                catch (ZlinkCloseException exception)
                    when (exception.Result is ZlinkCloseException.ErrorCode.Busy
                        or ZlinkCloseException.ErrorCode.Ok)
                {
                    // Binding cancellation completes the managed request
                    // before Core has delivered its terminal callback. Core
                    // retains the socket until that bounded request completes.
                    // Older local Core packages report this close race with
                    // errno 0, which the binding projects as Ok despite the
                    // failed close. Join either form before disposing context.
                    await Task.Delay(TimeSpan.FromMilliseconds(10))
                        .ConfigureAwait(false);
                }
            }
        }

        private static async Task IgnoreCancellationAsync(Task task)
        {
            try
            {
                await task.ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
            }
        }

        private void OnMonitorEvent(ZLinkBackendSocketMonitorEvent value)
        {
            switch (value.NativeEvent)
            {
                case ZLinkSocketNativeEventType.ConnectionReady:
                    var shouldStartAdmission = RunState(() =>
                    {
                        if (_disposed)
                            return false;
                        if (_expected is { } expected
                            && (value.RoutingId is not { } actual
                                || actual != expected.ServerRid))
                        {
                            _ready = false;
                            _rejected = true;
                            _admissionCompleted = true;
                            _onSelectionChanged();
                            return false;
                        }
                        return true;
                    });
                    if (shouldStartAdmission)
                        TryStartAdmission();
                    break;
                case ZLinkSocketNativeEventType.Disconnected:
                case ZLinkSocketNativeEventType.Closed:
                case ZLinkSocketNativeEventType.HandshakeFailedNoDetail:
                case ZLinkSocketNativeEventType.HandshakeFailedProtocol:
                case ZLinkSocketNativeEventType.HandshakeFailedAuth:
                    RunState(() =>
                    {
                        if (_reconnectInProgress)
                        {
                            _ready = false;
                            _onSelectionChanged();
                        }
                        else
                            FencePhysicalConnection("transport:disconnected");
                    });
                    break;
            }
        }

        private void TryStartAdmission()
        {
            ulong physicalGeneration;
            ulong attempt;
            RunState(() =>
            {
                if (_disposed || _admissionStarted)
                    return;
                _admissionStarted = true;
                physicalGeneration = _physicalGeneration;
                attempt = ++_admissionAttempt;
                using (ExecutionContext.SuppressFlow())
                    _admissionTask = RunAdmissionAsync(
                        physicalGeneration,
                        attempt,
                        _admissionStop.Token);
                _admissionTasks.RemoveAll(
                    static candidate => candidate.IsCompleted);
                _admissionTasks.Add(_admissionTask);
            });
        }

        private async Task RunAdmissionAsync(
            ulong physicalGeneration,
            ulong attempt,
            CancellationToken cancellationToken)
        {
            // TryStartAdmission invokes this method while holding the
            // connection state lock. Keep a synchronously completing request
            // from re-entering ApplyAdmission and the parent runtime callback
            // before that lock has been released.
            await Task.Yield();
            var retry = false;
            try
            {
                var hello = ZLinkClientServerControlProtocol.EncodeHello(
                    new ZLinkClientServerControlProtocol.Hello(
                        _channelName,
                        ZLinkTransportSecurityIdentity.Plaintext,
                        _normalizedEffectiveMaxMessageBytes));
                IReadOnlyList<Message> reply;
                try
                {
                    reply = await Socket.Request()
                        .Message(hello)
                        .Timeout(_admissionTimeout)
                        .Async(cancellationToken)
                        .ConfigureAwait(false);
                }
                catch
                {
                    hello.Dispose();
                    throw;
                }
                try
                {
                    _ = ApplyAdmission(
                        reply,
                        physicalGeneration,
                        attempt);
                }
                finally
                {
                    ZLinkMessageParts.DisposeAll(reply);
                }
            }
            catch (OperationCanceledException)
                when (cancellationToken.IsCancellationRequested)
            {
            }
            catch (Exception exception)
            {
                RunState(() =>
                {
                    if (!IsCurrentAttempt(physicalGeneration, attempt))
                        return;
                    _ready = false;
                    _admissionCompleted = false;
                    _diagnostics =
                        $"request:{exception.GetType().Name}";
                    retry = !_disposed;
                    _onSelectionChanged();
                });
            }
            finally
            {
                RunState(() =>
                {
                    if (IsCurrentAttempt(physicalGeneration, attempt))
                        _admissionStarted = false;
                });
            }
            if (retry && RestartPhysicalConnection("admission:request-failed"))
                return;
            if (retry)
                ScheduleAdmissionRetry();
        }

        private void ScheduleAdmissionRetry()
        {
            RunState(() =>
            {
                if (_disposed || _retryScheduled)
                    return;
                _retryScheduled = true;
                Task retryTask;
                using (ExecutionContext.SuppressFlow())
                    retryTask = RetryAdmissionAsync();
                _retryTasks.RemoveAll(
                    static candidate => candidate.IsCompleted);
                _retryTasks.Add(retryTask);
            });
        }

        private async Task RetryAdmissionAsync()
        {
            try
            {
                await Task.Delay(
                        TimeSpan.FromMilliseconds(100),
                        _admissionStop.Token)
                    .ConfigureAwait(false);
                TryStartAdmission();
            }
            catch (OperationCanceledException)
            {
            }
            finally
            {
                RunState(() => _retryScheduled = false);
            }
        }

        private bool ApplyAdmission(
            IReadOnlyList<Message> reply,
            ulong physicalGeneration,
            ulong attempt)
        {
            ZLinkClientServerServerDescriptor? expected;
            string? admittedIdentity = null;
            expected = RunState(() => _expected);
            try
            {
                if (!RunState(() =>
                {
                    if (!IsCurrentAttempt(physicalGeneration, attempt))
                        return false;
                    _admissionCompleted = true;
                    return true;
                }))
                    return false;
                if (ZLinkClientServerControlProtocol.TryDecodeReject(
                        reply,
                        out _)
                    || !ZLinkClientServerControlProtocol.TryDecodeAdmission(
                        reply,
                        out var admission)
                    || admission is null
                    || !StringComparer.Ordinal.Equals(
                        admission.ChannelName,
                        _channelName)
                    || admission.LifecycleGeneration == 0
                    || admission.Weight is < 0 or > ZLinkSocketConfig.MaximumPeerWeight
                    || admission.NormalizedEffectiveMaxMessageBytes == 0
                    || admission.NormalizedEffectiveMaxMessageBytes
                    > _normalizedEffectiveMaxMessageBytes
                    || expected is not null
                    && (admission.ServerRid != expected.ServerRid
                        || admission.LifecycleGeneration
                        != expected.LifecycleGeneration
                        || admission.DescriptorRevision
                        != expected.DescriptorRevision
                        || admission.Weight != expected.Weight
                        || admission.State != expected.State
                        || !StringComparer.Ordinal.Equals(
                            admission.AdvertisedEndpoint,
                            expected.Endpoint)
                        || !ZLinkClientServerControlProtocol.SecurityIdentityMatches(
                            expected.SecurityIdentity,
                            admission.SecurityIdentity)))
                {
                    return RunState(() =>
                    {
                        if (!IsCurrentAttempt(physicalGeneration, attempt))
                            return false;
                        _ready = false;
                        _rejected = true;
                        _diagnostics = reply.Count == 0
                            ? "invalid:empty"
                            : $"invalid:{Convert.ToHexString(
                                reply[0].AsReadOnlyMemory().Span)}";
                        _onSelectionChanged();
                        return false;
                    });
                }
                var accepted = RunState(() =>
                {
                    if (IsCurrentAttempt(physicalGeneration, attempt))
                    {
                        _weight = admission.Weight;
                        _ready =
                            admission.State
                            == ZLinkFrameworkRuntimeState.Serving
                            && _weight > 0;
                        _admittedIdentity = IdentityOf(
                            admission.ServerRid,
                            admission.LifecycleGeneration);
                        _currentAdmission = admission;
                        _peerDeadline =
                            DateTimeOffset.UtcNow + TimeSpan.FromSeconds(15);
                        admittedIdentity = _admittedIdentity;
                        _diagnostics = "ready";
                        using (ExecutionContext.SuppressFlow())
                        {
                            _controlTask ??= Task.Factory.StartNew(
                                    static state =>
                                        ((Connection)state!).RunControlLoopAsync(),
                                    this,
                                    CancellationToken.None,
                                    TaskCreationOptions.LongRunning,
                                    TaskScheduler.Default)
                                .Unwrap();
                            _livenessTask ??=
                                RunLivenessLoopAsync(_admissionStop.Token);
                        }
                        return true;
                    }
                    return false;
                });
                if (!accepted)
                    return false;
                _onSelectionChanged();
            }
            catch
            {
                return RunState(() =>
                {
                    if (!IsCurrentAttempt(physicalGeneration, attempt))
                        return false;
                    _ready = false;
                    _diagnostics = "invalid:exception";
                    _onSelectionChanged();
                    return false;
                });
            }
            if (admittedIdentity is not null)
                _onAdmitted(this, admittedIdentity);
            return true;
        }

        private async Task RunControlLoopAsync()
        {
            var cancellationToken = _admissionStop.Token;
            using var receivePoller = ZLinkBackendSocketPoller.Create(Socket);
            using var received = Received.Create();
            while (!cancellationToken.IsCancellationRequested)
            {
                var admissionEstablished = RunState(() => !_disposed
                        && _admissionCompleted
                        && _currentAdmission is not null);
                if (!admissionEstablished)
                {
                    await Task.Delay(
                            ControlReceivePollInterval,
                            cancellationToken)
                        .ConfigureAwait(false);
                    continue;
                }

                try
                {
                    var readiness = receivePoller.Wait(ControlReceivePollInterval);
                    if ((readiness & (ZLinkBackendSocketReadiness.Readable
                                      | ZLinkBackendSocketReadiness.Error
                                      | ZLinkBackendSocketReadiness.Priority)) == 0)
                        continue;
                    if (!Socket.Recv(received, RecvFlags.DontWait))
                    {
                        await Task.Delay(
                                TimeSpan.FromMilliseconds(10),
                                cancellationToken)
                            .ConfigureAwait(false);
                        continue;
                    }
                    if (ZLinkClientServerControlProtocol.TryDecodeLivenessProbe(
                            received.Parts,
                            out var probeId))
                    {
                        Interlocked.Increment(
                            ref _receivedLivenessProbeCount);
                        var ack =
                            ZLinkClientServerControlProtocol.EncodeLivenessAck(
                                probeId);
                        if (received.RequestSeq is not null)
                            ReplyOwned(received, ack);
                        else
                            await SendOwnedAsync(ack, cancellationToken)
                                .ConfigureAwait(false);
                        continue;
                    }
                    if (ZLinkClientServerControlProtocol.TryDecodeUpdate(
                            received.Parts,
                            out var update)
                        && update is not null)
                    {
                        ApplyUpdate(update);
                        continue;
                    }
                    if (ZLinkClientServerControlProtocol.TryDecodeLivenessAck(
                            received.Parts,
                            out var ackId))
                    {
                        var accepted = RunState(() =>
                        {
                            if (_outstandingProbeId != ackId)
                                return false;
                            _outstandingProbeId = null;
                            _peerDeadline =
                                DateTimeOffset.UtcNow + TimeSpan.FromSeconds(15);
                            if (_currentAdmission is
                                {
                                    State: ZLinkFrameworkRuntimeState.Serving,
                                    Weight: > 0
                                })
                                _ready = true;
                            return true;
                        });
                        if (!accepted)
                            continue;
                        _onSelectionChanged();
                        Interlocked.Increment(ref _livenessAckCount);
                        continue;
                    }
                    var restartReason =
                        ZLinkClientServerControlProtocol.IsControl(received.Parts)
                            ? "protocol:pushed-control"
                            : "protocol:unsolicited-application";
                    // Release native receive parts before disconnecting the
                    // socket that produced them.
                    received.Dispose();
                    RestartPhysicalConnection(restartReason);
                }
                catch (OperationCanceledException)
                    when (cancellationToken.IsCancellationRequested)
                {
                    break;
                }
                catch (Exception exception)
                {
                    RunState(() =>
                        _diagnostics =
                            $"control:{exception.GetType().Name}:{exception.Message}");
                    RestartPhysicalConnection(
                        $"control:{exception.GetType().Name}");
                    await Task.Delay(
                            TimeSpan.FromMilliseconds(10),
                            cancellationToken)
                        .ConfigureAwait(false);
                }
            }
        }

        private async Task RunLivenessLoopAsync(
            CancellationToken cancellationToken)
        {
            while (!cancellationToken.IsCancellationRequested)
            {
                await Task.Delay(
                        TimeSpan.FromSeconds(5),
                        cancellationToken)
                    .ConfigureAwait(false);
                var probeState = RunState(() =>
                {
                    if (_disposed)
                        return (Disposed: true, TimedOut: false,
                            ProbeId: 0UL, PhysicalGeneration: 0UL);
                    var timedOut = false;
                    if (DateTimeOffset.UtcNow >= _peerDeadline)
                    {
                        timedOut = true;
                    }
                    _outstandingProbeId ??= AllocateProbeId();
                    return (Disposed: false, TimedOut: timedOut,
                        ProbeId: _outstandingProbeId.Value,
                        PhysicalGeneration: _physicalGeneration);
                });
                if (probeState.Disposed)
                    return;
                if (probeState.TimedOut)
                {
                    RestartPhysicalConnection("liveness:timeout");
                    continue;
                }
                var probe =
                    ZLinkClientServerControlProtocol.EncodeLivenessProbe(
                        probeState.ProbeId);
                IReadOnlyList<Message> reply;
                try
                {
                    reply = await Socket.Request()
                        .Message(probe)
                        .Timeout(TimeSpan.FromSeconds(15))
                        .Async(cancellationToken)
                        .ConfigureAwait(false);
                }
                catch (OperationCanceledException)
                    when (cancellationToken.IsCancellationRequested)
                {
                    probe.Dispose();
                    return;
                }
                catch
                {
                    probe.Dispose();
                    continue;
                }
                try
                {
                    Interlocked.Increment(ref _sentLivenessProbeCount);
                    if (!ZLinkClientServerControlProtocol.TryDecodeLivenessAck(
                            reply,
                            out var ackId))
                        continue;
                    var acknowledged = RunState(() =>
                    {
                        if (_disposed
                            || _physicalGeneration != probeState.PhysicalGeneration
                            || _outstandingProbeId != ackId)
                            return false;
                        _outstandingProbeId = null;
                        _peerDeadline =
                            DateTimeOffset.UtcNow + TimeSpan.FromSeconds(15);
                        if (_currentAdmission is
                            {
                                State: ZLinkFrameworkRuntimeState.Serving,
                                Weight: > 0
                                })
                            _ready = true;
                        return true;
                    });
                    if (!acknowledged)
                        continue;
                    _onSelectionChanged();
                    Interlocked.Increment(ref _livenessAckCount);
                }
                finally
                {
                    ZLinkMessageParts.DisposeAll(reply);
                }
            }
        }

        private bool IsCurrentAttempt(
            ulong physicalGeneration,
            ulong attempt) =>
            !_disposed
            && _physicalGeneration == physicalGeneration
            && _admissionAttempt == attempt;

        private void FencePhysicalConnection(string diagnostics)
        {
            _physicalGeneration++;
            _admissionAttempt++;
            _admissionStarted = false;
            _admissionCompleted = false;
            _ready = false;
            _currentAdmission = null;
            _outstandingProbeId = null;
            _diagnostics = diagnostics;
            _onSelectionChanged();
        }

        private bool RestartPhysicalConnection(string diagnostics)
        {
            var reconnectState = RunState(() =>
            {
                if (_disposed || _reconnectInProgress)
                    return (Started: false, PhysicalGeneration: 0UL,
                        AdmissionAttempt: 0UL);
                _reconnectInProgress = true;
                FencePhysicalConnection(diagnostics);
                return (Started: true, PhysicalGeneration: _physicalGeneration,
                    AdmissionAttempt: _admissionAttempt);
            });
            if (!reconnectState.Started)
                return false;
            try
            {
                lock (_socketLifecycleGate)
                {
                    if (RunState(() => _disposed))
                    {
                        RunState(() => _reconnectInProgress = false);
                        return false;
                    }
                    Socket.Disconnect(_endpoint);
                }
            }
            catch
            {
            }
            var scheduled = RunState(() =>
            {
                if (_disposed)
                {
                    _reconnectInProgress = false;
                    return false;
                }
                using (ExecutionContext.SuppressFlow())
                    _reconnectTask = ReconnectAsync(
                        reconnectState.PhysicalGeneration,
                        reconnectState.AdmissionAttempt);
                return true;
            });
            return scheduled;
        }

        private async Task ReconnectAsync(
            ulong physicalGeneration,
            ulong admissionAttempt)
        {
            try
            {
                while (!_admissionStop.IsCancellationRequested)
                {
                    try
                    {
                        // Disconnect completion is asynchronous at the
                        // transport layer. Give its monitor event a chance
                        // to retire the old pipe before registering the same
                        // endpoint again.
                        await Task.Delay(
                                TimeSpan.FromMilliseconds(100),
                                _admissionStop.Token)
                            .ConfigureAwait(false);
                        lock (_socketLifecycleGate)
                        {
                            if (RunState(() => _disposed
                                    || _physicalGeneration != physicalGeneration
                                    || _admissionAttempt != admissionAttempt))
                                    return;
                            Socket.Connect(_endpoint);
                        }
                        break;
                    }
                    catch (OperationCanceledException)
                        when (_admissionStop.IsCancellationRequested)
                    {
                        return;
                    }
                    catch (Exception exception)
                    {
                        // A native disconnect can still be completing when
                        // the first reconnect attempt runs. Keep the same
                        // physical generation fenced and retry the connect;
                        // losing this one transient failure would leave the
                        // connection permanently not-ready without another
                        // monitor event to trigger admission.
                        if (!RunState(() =>
                        {
                            if (_disposed
                                || _physicalGeneration != physicalGeneration
                                || _admissionAttempt != admissionAttempt)
                                return false;
                            _diagnostics =
                                $"reconnect:{exception.GetType().Name}";
                            return true;
                        }))
                            return;
                    }
                }
            }
            finally
            {
                RunState(() => _reconnectInProgress = false);
            }
            if (!_admissionStop.IsCancellationRequested)
                ScheduleAdmissionRetry();
        }

        private void ApplyUpdate(
            ZLinkClientServerControlProtocol.Admission update)
        {
            RunState(() =>
            {
                var current = _currentAdmission;
                if (current is null
                    || update.ChannelName != current.ChannelName
                    || update.ServerRid != current.ServerRid
                    || update.LifecycleGeneration
                    != current.LifecycleGeneration
                    || update.SecurityIdentity != current.SecurityIdentity
                    || update.AdvertisedEndpoint
                    != current.AdvertisedEndpoint
                    || update.NormalizedEffectiveMaxMessageBytes
                    != current.NormalizedEffectiveMaxMessageBytes)
                {
                    _ready = false;
                    _diagnostics = "invalid:update-identity";
                    _onSelectionChanged();
                    return;
                }
                if (update.DescriptorRevision < current.DescriptorRevision)
                    return;
                if (update.DescriptorRevision == current.DescriptorRevision)
                {
                    if (update != current)
                    {
                        _ready = false;
                        _diagnostics = "invalid:update-conflict";
                        _onSelectionChanged();
                    }
                    return;
                }
                _currentAdmission = update;
                _weight = update.Weight;
                _ready = update.State == ZLinkFrameworkRuntimeState.Serving
                    && update.Weight > 0;
                _diagnostics = _ready ? "ready" : "update:not-ready";
                _onSelectionChanged();
            });
        }

        private async ValueTask<bool> SendOwnedAsync(
            Message message,
            CancellationToken cancellationToken)
        {
            try
            {
                await Socket.Send()
                    .Message(message)
                    .Async(cancellationToken)
                    .ConfigureAwait(false);
                return true;
            }
            catch
            {
                return false;
            }
            finally
            {
                message.Dispose();
            }
        }

        private static void ReplyOwned(
            Received received,
            Message message)
        {
            try
            {
                // A request envelope needs the correlated raw reply terminal.
                // Raw replies are HWM-free, so Submit is canonical and cannot
                // suspend the sole control receive loop behind send admission.
                received.Reply()
                    .Message(message)
                    .Submit();
            }
            finally
            {
                message.Dispose();
            }
        }

        private ulong AllocateProbeId()
        {
            var result = _nextProbeId;
            _nextProbeId = result == long.MaxValue ? 1 : result + 1;
            return result;
        }

        private T RunState<T>(Func<T> work) =>
            AwaitStateLane(_lane.RunAsync(work));

        private void RunState(Action work) =>
            AwaitStateLane(_lane.RunAsync(work));

        private static T AwaitStateLane<T>(ValueTask<T> operation) =>
            operation.GetAwaiter().GetResult();

        private static void AwaitStateLane(ValueTask operation) =>
            operation.GetAwaiter().GetResult();

        private static string IdentityOf(
            RoutingId serverRid,
            ulong lifecycleGeneration) =>
            $"{serverRid.ToHex()}:{lifecycleGeneration}";
    }
}

internal sealed record ZLinkClientServerConnectionSnapshot(
    RoutingId? ServerRid,
    ulong? LifecycleGeneration,
    ulong? DescriptorRevision,
    string Endpoint,
    int Weight,
    bool Ready,
    ZLinkClientServerServerState State,
    string DescriptorSource,
    string? LastFailure);
