using Zlink.Framework.Runtime.Backend.DotNet.Wrappers;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Messaging;
using Zlink.Framework.Runtime.Service;
using static Zlink.Framework.Runtime.Execution.ZLinkStateLaneWait;

namespace Zlink.Framework.Runtime.Channels;

internal sealed class ZLinkClientServerClientRuntime : IAsyncDisposable
{
    private const string ManualConnectionPrefix = "manual:";
    private const string LocalConnectionPrefix = "local:";
    private const string AutomaticConnectionPrefix = "auto:";
    private const string ProcessLocalOwnerId = "process-local";
    private static readonly TimeSpan ControlReceivePollInterval = TimeSpan.FromMilliseconds(100);
    private readonly ZLinkChannelName _channelName;
    private readonly IZLinkMonitoringBackendAdapter _monitoring;
    private readonly IZLinkBackendRuntimeContext _context;
    private readonly IZLinkSocketConfig _socketConfig;
    private readonly ZLinkApplicationJobQueue _applicationJobQueue;
    private readonly TimeSpan _sendTimeout;
    private readonly TimeProvider _time;
    private readonly CancellationToken _stopToken;
    private readonly ZLinkStateLane _lane = new();
    private readonly Dictionary<string, Connection> _connections = new(StringComparer.Ordinal);
    private readonly List<(Task Task, Connection? Connection)> _retired = [];
    private ZLinkWeightedSelectionPlan<ReadyTarget, string>? _readySelectionPlan;
    private long _readySelectionPlanBuildCount;
    private int _pendingRequests;
    private bool IsDisposing => Volatile.Read(ref _disposeTask) is not null;
    private IDisposable? _manualConnectionAttachment;
    private Task? _disposeTask;
    private readonly ZLinkMessageFlowTracer? _flow;
    private readonly IZLinkRuntimeFailureReporter _errorSink;

    // Monitoring subscribers use this edge notification to request a fresh
    // snapshot. The callback only signals a bounded channel; it never reads
    // connection state while a connection lock is held.
    internal event Action? StateChanged;

    internal ZLinkClientServerClientRuntime(
        string channelName,
        IZLinkMonitoringBackendAdapter monitoring,
        IZLinkBackendRuntimeContext context,
        IZLinkSocketConfig socketConfig,
        TimeSpan sendTimeout,
        CancellationToken stopToken,
        ZLinkApplicationJobQueue applicationJobQueue,
        IZLinkRuntimeFailureReporter errorSink,
        ZLinkMessageFlowTracer? flow = null,
        TimeProvider? timeProvider = null
    )
    {
        _channelName = ZLinkChannelName.FromBoundary(channelName, nameof(channelName));
        _monitoring = monitoring;
        _context = context;
        _socketConfig = socketConfig;
        _sendTimeout = sendTimeout;
        _stopToken = stopToken;
        _applicationJobQueue = applicationJobQueue;
        _errorSink = errorSink;
        _flow = flow;
        _time = timeProvider ?? TimeProvider.System;
    }

    internal void AddManual(string endpoint) =>
        AddOrReplaceAsync($"{ManualConnectionPrefix}{endpoint}", endpoint, expected: null)
            .AsTask()
            .GetAwaiter()
            .GetResult();

    internal void RemoveManual(string endpoint) => Remove($"{ManualConnectionPrefix}{endpoint}");

    internal async ValueTask AddLocalAsync(ZLinkClientServerServerIdentity identity)
    {
        var endpoint = identity.AdvertisedEndpoint;
        var snapshot = await identity.ReadAsync().ConfigureAwait(false);
        var key =
            $"{LocalConnectionPrefix}{identity.ServerRid.ToHex()}:{identity.LifecycleGeneration}";
        await AddOrReplaceAsync(key, endpoint, LocalDescriptor(identity, endpoint, snapshot))
            .ConfigureAwait(false);
        identity.SnapshotChanged += changed =>
        {
            RunState(() =>
            {
                if (_connections.TryGetValue(key, out var connection))
                    connection.Update(LocalDescriptor(identity, endpoint, changed));
            });
        };
    }

    private ZLinkClientServerServerDescriptor LocalDescriptor(
        ZLinkClientServerServerIdentity identity,
        string endpoint,
        ZLinkClientServerServerIdentity.Snapshot snapshot
    ) =>
        new(
            _channelName.Value,
            identity.ServerRid,
            identity.LifecycleGeneration,
            snapshot.Revision,
            endpoint,
            snapshot.Weight,
            snapshot.State,
            identity.SecurityIdentity,
            ProcessLocalOwnerId,
            1,
            default
        );

    internal async ValueTask ReplaceAutomaticAsync(
        IReadOnlyList<ZLinkClientServerServerDescriptor> descriptors
    )
    {
        var desired = descriptors.ToDictionary(
            static row =>
                $"{AutomaticConnectionPrefix}{row.ServerRid.ToHex()}:{row.LifecycleGeneration}",
            StringComparer.Ordinal
        );
        var successors = descriptors.ToDictionary(
            static row => row.ServerRid,
            static row =>
                $"{AutomaticConnectionPrefix}{row.ServerRid.ToHex()}:{row.LifecycleGeneration}"
        );
        string[] obsolete;
        obsolete = RunState(() =>
            _connections
                .Keys.Where(static key =>
                    key.StartsWith(AutomaticConnectionPrefix, StringComparison.Ordinal)
                )
                .Where(key => !desired.ContainsKey(key))
                .ToArray()
        );

        // Start successors before removing the previous lifecycle. Ready
        // publication is fenced inside each connection.
        foreach (var (key, descriptor) in desired)
            await AddOrReplaceAsync(key, descriptor.Endpoint, descriptor).ConfigureAwait(false);
        foreach (var key in obsolete)
        {
            var retainForSuccessor = RunState(() =>
            {
                if (
                    _connections.TryGetValue(key, out var obsoleteConnection)
                    && obsoleteConnection.ExpectedServerRid is { } rid
                    && successors.TryGetValue(rid, out var successorKey)
                    && _connections.TryGetValue(successorKey, out var successor)
                )
                    return !successor.AdmissionCompleted;
                return false;
            });
            if (!retainForSuccessor)
                Remove(key);
        }
    }

    internal async ValueTask<ZLinkOneWaySubmitResult> SendAsync(
        IReadOnlyList<Message> parts,
        CancellationToken cancellationToken
    )
    {
        var readiness = await WaitForReadyAsync(_sendTimeout, cancellationToken)
            .ConfigureAwait(false);
        var target = readiness.Target;
        if (target is null)
        {
            ZLinkMessageParts.DisposeAll(parts);
            return new ZLinkOneWaySubmitResult(readiness.Status);
        }
        if (!ZLinkClientServerMessageBound.Fits(parts, target.AdmittedMaximumMessageBytes))
        {
            ZLinkMessageParts.DisposeAll(parts);
            throw ZLinkClientServerMessageBound.CreateExceededException(
                target.AdmittedMaximumMessageBytes
            );
        }
        // Capture the trace fields before the transport takes ownership of the
        // parts; emit `sent` only after the local transport accepts them.
        var sentPacketName =
            _flow?.Enabled(ZLinkMessageFlowOutcome.Sent) == true
                ? ZLinkFrameworkRuntime.TryReadEnvelopePacketName(parts)
                : null;
        try
        {
            await target
                .Socket.Send()
                .Messages(parts)
                .Async(cancellationToken)
                .EnsureAcceptedAsync()
                .ConfigureAwait(false);
            if (sentPacketName is not null && _flow!.Enabled(ZLinkMessageFlowOutcome.Sent))
                _flow.Trace(
                    new ZLinkMessageFlowEvent(
                        ZLinkMessageFlowOutcome.Sent,
                        ZLinkDispatchErrorSurface.Channel,
                        ZLinkDispatchMessageKind.Send,
                        sentPacketName,
                        _channelName.Value,
                        ServerRid: target.SelectionServerRid.ToString()
                    )
                );
            return new ZLinkOneWaySubmitResult(ZLinkOneWaySubmitStatus.Submitted);
        }
        catch (ZlinkSubmitException error)
        {
            return new ZLinkOneWaySubmitResult(
                error.Result switch
                {
                    ZlinkSubmitException.ErrorCode.Backpressured =>
                        ZLinkOneWaySubmitStatus.TimedOut,
                    ZlinkSubmitException.ErrorCode.NotFound =>
                        ZLinkOneWaySubmitStatus.TargetNotFound,
                    ZlinkSubmitException.ErrorCode.NotConnected =>
                        ZLinkOneWaySubmitStatus.RouteNotConnected,
                    ZlinkSubmitException.ErrorCode.Terminated => ZLinkOneWaySubmitStatus.Shutdown,
                    _ => throw ZLinkRequestFailureMapper.CreateSubmitException(
                        error,
                        $"ClientServer send failed for '{_channelName}'"
                    ),
                }
            );
        }
        catch (ObjectDisposedException)
        {
            return new ZLinkOneWaySubmitResult(ZLinkOneWaySubmitStatus.Shutdown);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }
    }

    internal async ValueTask<IReadOnlyList<Message>> RequestAsync(
        IReadOnlyList<Message> parts,
        TimeSpan timeout,
        CancellationToken cancellationToken
    )
    {
        Interlocked.Increment(ref _pendingRequests);
        SignalStateChanged();
        using var readyWaitCancellation = CancellationTokenSource.CreateLinkedTokenSource(
            cancellationToken,
            _stopToken
        );
        try
        {
            var readiness = await WaitForReadyAsync(_sendTimeout, readyWaitCancellation.Token)
                .ConfigureAwait(false);
            var target = readiness.Target;
            if (target is null)
            {
                ZLinkMessageParts.DisposeAll(parts);
                ZLinkOneWaySubmitOutcome.EnsureAccepted(
                    new ZLinkOneWaySubmitResult(readiness.Status),
                    $"ClientServer channel '{_channelName}' request"
                );
                throw new InvalidOperationException("A failed ready wait was accepted.");
            }
            if (!ZLinkClientServerMessageBound.Fits(parts, target.AdmittedMaximumMessageBytes))
            {
                ZLinkMessageParts.DisposeAll(parts);
                throw ZLinkClientServerMessageBound.CreateExceededException(
                    target.AdmittedMaximumMessageBytes
                );
            }
            if (
                _flow?.Enabled(ZLinkMessageFlowOutcome.Sent) == true
                && ZLinkFrameworkRuntime.TryReadEnvelopeHeaderForTrace(parts) is { } sentHeader
            )
                _flow.Trace(
                    new ZLinkMessageFlowEvent(
                        ZLinkMessageFlowOutcome.Sent,
                        ZLinkDispatchErrorSurface.Channel,
                        ZLinkDispatchMessageKind.Request,
                        sentHeader.MessageName,
                        _channelName.Value,
                        CorrelationId: sentHeader.CorrelationId,
                        ServerRid: target.SelectionServerRid.ToString()
                    )
                );
            return await ZLinkRawRequestSubmitter
                .SubmitAsync(
                    parts,
                    (pending, nativeTimeout, token) =>
                        ZLinkRequestSubmissionOutcome.SubmitAndAwaitReplyAsync(
                            target.Socket.Request().Messages(pending).Timeout(nativeTimeout),
                            token
                        ),
                    timeout,
                    $"ClientServer request failed for '{_channelName}': {{0}}.",
                    cancellationToken
                )
                .ConfigureAwait(false);
        }
        catch (OperationCanceledException)
            when (_stopToken.IsCancellationRequested && !cancellationToken.IsCancellationRequested)
        {
            throw ZLinkRequestFailureMapper.CreateShutdownRequestException(
                $"ClientServer channel '{_channelName}' request was interrupted by runtime shutdown."
            );
        }
        finally
        {
            Interlocked.Decrement(ref _pendingRequests);
            SignalStateChanged();
        }
    }

    internal int PendingRequestCount => Volatile.Read(ref _pendingRequests);

    internal long ReadySelectionPlanBuildCount => RunState(() => _readySelectionPlanBuildCount);

    internal IReadOnlyList<ZLinkClientServerConnectionSnapshot> SnapshotConnections() =>
        RunState(() =>
            DistinctConnections()
                .Select(static connection => connection.Snapshot())
                .OrderBy(static value => value.ServerRid?.ToHex(), StringComparer.Ordinal)
                .ThenBy(static value => value.Endpoint, StringComparer.Ordinal)
                .ToArray()
        );

    internal int ReadyCount
    {
        get => RunState(() => DistinctConnections().Count(static value => value.Ready));
    }

    internal int AdmissionCompletedCount
    {
        get =>
            RunState(() => DistinctConnections().Count(static value => value.AdmissionCompleted));
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
        get =>
            RunState(() =>
                DistinctConnections().Sum(static connection => connection.LivenessAckCount)
            );
    }

    internal long ReceivedLivenessProbeCount
    {
        get =>
            RunState(() =>
                DistinctConnections()
                    .Sum(static connection => connection.ReceivedLivenessProbeCount)
            );
    }

    internal long SentLivenessProbeCount
    {
        get =>
            RunState(() =>
                DistinctConnections().Sum(static connection => connection.SentLivenessProbeCount)
            );
    }

    internal string AdmissionDiagnostics
    {
        get =>
            RunState(() =>
                string.Join(
                    "; ",
                    _connections.Select(static entry => $"{entry.Key}={entry.Value.Diagnostics}")
                )
            );
    }

    internal IAsyncDisposable GetMonitoringSocket()
    {
        return RunState(() =>
            _connections.Values.FirstOrDefault()?.Socket
            ?? throw new InvalidOperationException(
                $"ClientServer client '{_channelName}' has no connection intent to monitor."
            )
        );
    }

    public ValueTask DisposeAsync() =>
        new(
            ZLinkRuntimeTaskRunner.RunDisposal(
                ref _disposeTask,
                async () =>
                {
                    var attachment = RunState(() =>
                    {
                        var value = _manualConnectionAttachment;
                        _manualConnectionAttachment = null;
                        return value;
                    });
                    attachment?.Dispose();
                    await DisposeCoreAsync().ConfigureAwait(false);
                }
            )
        );

    internal void OwnManualConnectionAttachment(IDisposable attachment)
    {
        ArgumentNullException.ThrowIfNull(attachment);
        var dispose = false;
        IDisposable? previous = null;
        RunState(() =>
        {
            if (IsDisposing)
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

    private async Task DisposeCoreAsync()
    {
        Connection[] values = [];
        Task[] retired = [];
        RunState(() =>
        {
            retired = _retired
                .Where(value => !value.Task.IsFaulted)
                .Select(value => value.Task)
                .ToArray();
        });

        var failures = new ZLinkFailureCollector();
        foreach (var task in retired)
            await failures.CaptureAsync(() => new ValueTask(task)).ConfigureAwait(false);
        failures.ThrowIfAny();
        RunState(() =>
            values = DistinctConnections()
                .Concat(
                    _retired
                        .Where(value =>
                            !value.Task.IsCompletedSuccessfully && value.Connection is not null
                        )
                        .Select(value => value.Connection!)
                )
                .Distinct()
                .ToArray()
        );
        foreach (var value in values)
            await failures.CaptureAsync(value.DisposeAsync).ConfigureAwait(false);
        failures.ThrowIfAny();
        RunState(() =>
        {
            _connections.Clear();
            _retired.Clear();
        });
        await _lane.DisposeAsync().ConfigureAwait(false);
    }

    private async ValueTask AddOrReplaceAsync(
        string key,
        string endpoint,
        ZLinkClientServerServerDescriptor? expected
    )
    {
        Connection? previous = null;
        Connection? created = null;
        TaskCompletionSource? inFlight = null;
        RunState(() =>
        {
            if (IsDisposing)
                return;
            if (
                _connections.TryGetValue(key, out var existing)
                && existing.Matches(endpoint, expected)
            )
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
                _stopToken,
                OnAdmitted,
                ScheduleStateChanged,
                _time,
                _errorSink
            );
            inFlight = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            _retired.RemoveAll(static candidate => candidate.Task.IsCompletedSuccessfully);
            _retired.Add((inFlight.Task, created));
        });
        if (created is null)
            return;
        try
        {
            try
            {
                await created
                    .PrepareAsync(_applicationJobQueue, _monitoring, _socketConfig, _sendTimeout)
                    .ConfigureAwait(false);
                var committed = false;
                try
                {
                    committed = RunState(() =>
                    {
                        if (IsDisposing)
                            return false;
                        _connections.TryGetValue(key, out var current);
                        if (!ReferenceEquals(current, previous))
                            return false;
                        _connections[key] = created;
                        ScheduleStateChanged(selectionChanged: true);
                        return true;
                    });
                }
                catch (ObjectDisposedException) { }
                if (!committed)
                {
                    await created.DisposeAsync().ConfigureAwait(false);
                    return;
                }
                created.Start();
            }
            catch (Exception startFailure)
            {
                var failures = new ZLinkFailureCollector(startFailure);
                ValueTask previousDisposal = ValueTask.CompletedTask;
                failures.Capture(() =>
                {
                    try
                    {
                        previousDisposal = RunState(() =>
                        {
                            if (
                                _connections.TryGetValue(key, out var current)
                                && ReferenceEquals(current, created)
                            )
                            {
                                if (previous is null)
                                    _connections.Remove(key);
                                else
                                    _connections[key] = previous;
                                ScheduleStateChanged(selectionChanged: true);
                            }
                            return previous is not null && !IsReferenced(previous)
                                ? new ValueTask(RegisterRetirement(previous))
                                : ValueTask.CompletedTask;
                        });
                    }
                    catch (ObjectDisposedException)
                    {
                        previousDisposal = previous is null
                            ? ValueTask.CompletedTask
                            : new ValueTask(RegisterRetirement(previous));
                    }
                });
                await failures.CaptureAsync(created.DisposeAsync).ConfigureAwait(false);
                await failures.CaptureAsync(() => previousDisposal).ConfigureAwait(false);
                failures.ThrowIfAny();
                throw new InvalidOperationException(
                    "Unreachable after connection startup cleanup failure propagation."
                );
            }
            if (previous is not null)
            {
                ValueTask previousDisposal;
                try
                {
                    previousDisposal = RunState(() =>
                        !IsReferenced(previous)
                            ? new ValueTask(RegisterRetirement(previous))
                            : ValueTask.CompletedTask
                    );
                }
                catch (ObjectDisposedException)
                {
                    previousDisposal = new ValueTask(RegisterRetirement(previous));
                }
                await previousDisposal.ConfigureAwait(false);
            }
        }
        catch (Exception failure)
        {
            inFlight!.TrySetException(failure);
            throw;
        }
        finally
        {
            inFlight!.TrySetResult();
        }
    }

    private void Remove(string key)
    {
        Connection? removed;
        RunState(() =>
        {
            if (IsDisposing)
                return;
            if (!_connections.Remove(key, out removed))
                return;
            ScheduleStateChanged(selectionChanged: true);
            if (IsReferenced(removed))
                return;
            RegisterRetirement(removed);
        });
    }

    private Task RegisterRetirement(Connection connection)
    {
        var task = connection.DisposeAsync().AsTask();
        _retired.RemoveAll(static candidate => candidate.Task.IsCompletedSuccessfully);
        _retired.Add((task, connection));
        ZLinkUnawaitedSubmit.Observe(new ValueTask(task), nameof(RegisterRetirement), _errorSink);
        return task;
    }

    private ReadyWaitResult SelectReady() =>
        RunState(() =>
        {
            if (_readySelectionPlan?.Select() is { } ready)
                return new ReadyWaitResult(ready, ZLinkOneWaySubmitStatus.Submitted);
            return new ReadyWaitResult(
                null,
                DistinctConnections().Any(static connection => connection.AdmittedButIneligible)
                    ? ZLinkOneWaySubmitStatus.RouteNotConnected
                    : ZLinkOneWaySubmitStatus.TimedOut
            );
        });

    private void ScheduleStateChanged(bool selectionChanged)
    {
        ZLinkUnawaitedSubmit.Observe(
            _lane.RunAsync(() =>
            {
                if (!IsDisposing && selectionChanged)
                    RebuildReadySelectionPlanUnderLock();
                SignalStateChanged();
                return ValueTask.CompletedTask;
            }),
            nameof(ScheduleStateChanged),
            _errorSink
        );
    }

    private void RebuildReadySelectionPlanUnderLock()
    {
        var retainedCurrents = _readySelectionPlan?.CaptureCurrents();
        var candidates = DistinctConnections()
            .Select(static connection => connection.ReadyTarget)
            .Where(static target => target is not null)
            .Select(static target => target!)
            .OrderBy(static target => target.SelectionServerRid.ToHex(), StringComparer.Ordinal)
            .ToArray();
        _readySelectionPlan = new ZLinkWeightedSelectionPlan<ReadyTarget, string>(
            candidates,
            static target => target.Weight,
            static target => target.SelectionServerRid.ToHex(),
            retainedCurrents,
            StringComparer.Ordinal,
            StringComparer.Ordinal
        );
        _readySelectionPlanBuildCount++;
    }

    private void SignalStateChanged() => StateChanged?.Invoke();

    private sealed record ReadyTarget(
        IDealerSocket Socket,
        uint AdmittedMaximumMessageBytes,
        RoutingId SelectionServerRid,
        int Weight
    );

    private IEnumerable<Connection> DistinctConnections() =>
        _connections.Values.Distinct(
            (IEqualityComparer<Connection>)ReferenceEqualityComparer.Instance
        );

    private bool IsReferenced(Connection connection) =>
        _connections.Values.Any(candidate => ReferenceEquals(candidate, connection));

    private void OnAdmitted(Connection admitted, string identity)
    {
        Connection? duplicate = null;
        RunState(() =>
        {
            if (IsDisposing)
                return;
            if (!IsReferenced(admitted))
                return;
            var canonical = DistinctConnections()
                .FirstOrDefault(candidate =>
                    !ReferenceEquals(candidate, admitted)
                    && StringComparer.Ordinal.Equals(candidate.AdmittedIdentity, identity)
                );
            if (canonical is null)
                return;

            canonical.MergeExpected(admitted.Expected);
            foreach (
                var key in _connections
                    .Where(entry => ReferenceEquals(entry.Value, admitted))
                    .Select(static entry => entry.Key)
                    .ToArray()
            )
                _connections[key] = canonical;
            duplicate = admitted;
            RegisterRetirement(duplicate);
        });
    }

    private T RunState<T>(Func<T> work) => AwaitStateLane(_lane.RunAsync(work));

    private void RunState(Action work) => AwaitStateLane(_lane.RunAsync(work));

    private async ValueTask<ReadyWaitResult> WaitForReadyAsync(
        TimeSpan timeout,
        CancellationToken cancellationToken
    )
    {
        var started = _time.GetTimestamp();
        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var readiness = SelectReady();
            if (
                readiness.Target is not null
                || readiness.Status == ZLinkOneWaySubmitStatus.RouteNotConnected
            )
                return readiness;
            var changed = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            void OnStateChanged() => changed.TrySetResult();
            StateChanged += OnStateChanged;
            try
            {
                readiness = SelectReady();
                if (
                    readiness.Target is not null
                    || readiness.Status == ZLinkOneWaySubmitStatus.RouteNotConnected
                )
                    return readiness;
                var remaining = timeout - _time.GetElapsedTime(started);
                if (remaining <= TimeSpan.Zero)
                    return readiness;
                try
                {
                    await changed
                        .Task.WaitAsync(remaining, _time, cancellationToken)
                        .ConfigureAwait(false);
                }
                catch (TimeoutException)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    // Observe the current snapshot and monotonic deadline again after the timer wakes.
                }
            }
            finally
            {
                StateChanged -= OnStateChanged;
            }
        }
    }

    private readonly record struct ReadyWaitResult(
        ReadyTarget? Target,
        ZLinkOneWaySubmitStatus Status
    );

    private sealed class Connection : IAsyncDisposable
    {
        private readonly string _channelName;
        private readonly string _endpoint;
        private TimeSpan _admissionTimeout;
        private readonly CancellationToken _stopToken;
        private uint _normalizedEffectiveMaxMessageBytes;
        private readonly ZLinkStateLane _lane = new();
        private readonly object _socketLifecycleGate = new();
        private IZLinkBackendSocketMonitor _monitor = null!;
        private IZLinkBackendSocketPoller _receivePoller = null!;
        private ZLinkClientServerServerDescriptor? _expected;
        private bool IsDisposing => Volatile.Read(ref _disposeTask) is not null;
        private Task? _disposeTask;
        private bool _admissionStarted;
        private bool _admissionCompleted;
        private bool _ready;
        private bool _rejected;
        private int _weight;
        private string _diagnostics = "configured";
        private ZLinkClientServerControlProtocol.Admission? _admittedDescriptor;
        private CancellationTokenSource _admissionStop = null!;
        private readonly List<Task> _requestTasks = [];
        private Task? _controlTask;
        private Task? _livenessTask;
        private Task? _monitorTask;
        private readonly Action<Connection, string> _onAdmitted;
        private readonly Action<bool> _onStateChanged;
        private ReadyTarget? _readyTarget;
        private ulong _nextProbeId = 1;
        private ulong? _outstandingProbeId;
        private long _lastPeerActivity;
        private readonly TimeProvider _time;
        private readonly IZLinkRuntimeFailureReporter _errorSink;
        private long _livenessAckCount;
        private long _receivedLivenessProbeCount;
        private long _sentLivenessProbeCount;
        private ulong _physicalGeneration = 1;
        private ulong _admissionAttempt;
        private IAsyncDisposable? _receiveFlowRegistration;

        internal Connection(
            string channelName,
            string endpoint,
            ZLinkClientServerServerDescriptor? expected,
            IDealerSocket socket,
            CancellationToken stopToken,
            Action<Connection, string> onAdmitted,
            Action<bool> onStateChanged,
            TimeProvider timeProvider,
            IZLinkRuntimeFailureReporter errorSink
        )
        {
            _channelName = channelName;
            _endpoint = endpoint;
            _expected = expected;
            _stopToken = stopToken;
            _onAdmitted = onAdmitted;
            _onStateChanged = onStateChanged;
            _time = timeProvider;
            _errorSink = errorSink;
            Socket = socket;
        }

        internal IDealerSocket Socket { get; }
        internal ReadyTarget? ReadyTarget => Volatile.Read(ref _readyTarget);
        private ZLinkClientServerControlProtocol.Admission? CurrentAdmission =>
            _admissionCompleted && !_rejected ? _admittedDescriptor : null;
        internal bool AdmittedButIneligible =>
            RunState(() =>
                !IsDisposing
                && CurrentAdmission
                    is {
                        State: ZLinkFrameworkRuntimeState.Serving
                            or ZLinkFrameworkRuntimeState.Draining
                    } admission
                && (
                    _weight == 0
                    || admission.State == ZLinkFrameworkRuntimeState.Draining
                    || _expected?.State == ZLinkFrameworkRuntimeState.Draining
                )
                && _readyTarget is null
            );
        internal bool Ready => ReadyTarget is not null;
        internal bool AdmissionCompleted
        {
            get => RunState(() => _admissionCompleted);
        }
        internal string Diagnostics
        {
            get =>
                RunState(() =>
                    $"{_diagnostics};generation={_physicalGeneration};"
                    + $"attempt={_admissionAttempt};"
                    + $"admissionStarted={_admissionStarted};"
                    + $"admissionCompleted={_admissionCompleted};"
                    + $"current={CurrentAdmission is not null}"
                );
        }
        internal RoutingId? ExpectedServerRid
        {
            get => RunState(() => _expected?.ServerRid);
        }
        internal string? AdmittedIdentity
        {
            get =>
                RunState(() =>
                    _admittedDescriptor is { } descriptor
                        ? IdentityOf(descriptor.ServerRid, descriptor.LifecycleGeneration)
                        : null
                );
        }
        internal ZLinkClientServerServerDescriptor? Expected
        {
            get => RunState(() => _expected);
        }
        internal long LivenessAckCount => Interlocked.Read(ref _livenessAckCount);
        internal long ReceivedLivenessProbeCount =>
            Interlocked.Read(ref _receivedLivenessProbeCount);
        internal long SentLivenessProbeCount => Interlocked.Read(ref _sentLivenessProbeCount);

        internal ZLinkClientServerConnectionSnapshot Snapshot()
        {
            return RunState(() =>
            {
                var admission = _admittedDescriptor;
                var expected = _expected;
                var state =
                    admission?.State
                        is ZLinkFrameworkRuntimeState.Draining
                            or ZLinkFrameworkRuntimeState.Relocating
                            or ZLinkFrameworkRuntimeState.Relocated
                    || expected?.State
                        is ZLinkFrameworkRuntimeState.Draining
                            or ZLinkFrameworkRuntimeState.Relocating
                            or ZLinkFrameworkRuntimeState.Relocated
                        ? ZLinkClientServerServerState.Draining
                    : _rejected ? ZLinkClientServerServerState.Rejected
                    : _ready ? ZLinkClientServerServerState.Ready
                    : _admissionStarted ? ZLinkClientServerServerState.Connecting
                    : _admissionCompleted || admission is not null
                        ? ZLinkClientServerServerState.Disconnected
                    : ZLinkClientServerServerState.Configured;
                return new ZLinkClientServerConnectionSnapshot(
                    admission?.ServerRid ?? expected?.ServerRid,
                    admission?.LifecycleGeneration ?? expected?.LifecycleGeneration,
                    admission?.DescriptorRevision ?? expected?.DescriptorRevision,
                    admission?.AdvertisedEndpoint ?? expected?.Endpoint ?? _endpoint,
                    _weight,
                    _readyTarget is not null,
                    state,
                    expected is null || expected.OwnerId == ProcessLocalOwnerId
                        ? "manual"
                        : "redis",
                    _ready ? null : _diagnostics
                );
            });
        }

        internal bool Matches(string endpoint, ZLinkClientServerServerDescriptor? expected)
        {
            return RunState(() =>
                StringComparer.Ordinal.Equals(_endpoint, endpoint)
                && !_rejected
                && (
                    _expected is null && expected is null
                    || _expected is not null
                        && expected is not null
                        && _expected.ServerRid == expected.ServerRid
                        && _expected.LifecycleGeneration == expected.LifecycleGeneration
                        && StringComparer.Ordinal.Equals(
                            _expected.SecurityIdentity,
                            expected.SecurityIdentity
                        )
                )
            );
        }

        internal void Update(ZLinkClientServerServerDescriptor? expected)
        {
            RunState(() =>
            {
                _expected = expected;
                if (expected is not null)
                {
                    _weight = expected.Weight;
                    if (expected.State != ZLinkFrameworkRuntimeState.Serving)
                        _ready = false;
                    else if (CurrentAdmission is not null)
                        _ready = true;
                }
                PublishReadyTargetUnderLock();
            });
        }

        internal void MergeExpected(ZLinkClientServerServerDescriptor? expected)
        {
            if (expected is null)
                return;
            RunState(() =>
            {
                if (
                    _admittedDescriptor is { } descriptor
                    && (
                        descriptor.ServerRid != expected.ServerRid
                        || descriptor.LifecycleGeneration != expected.LifecycleGeneration
                    )
                )
                    return;
                _expected = expected;
                _weight = expected.Weight;
                _ready =
                    CurrentAdmission is not null
                    && expected.State == ZLinkFrameworkRuntimeState.Serving;
                PublishReadyTargetUnderLock();
            });
        }

        internal async ValueTask PrepareAsync(
            ZLinkApplicationJobQueue applicationJobQueue,
            IZLinkMonitoringBackendAdapter monitoring,
            IZLinkSocketConfig socketConfig,
            TimeSpan sendTimeout
        )
        {
            _admissionTimeout = socketConfig.ConnectTimeout ?? TimeSpan.FromSeconds(1);
            _admissionStop = CancellationTokenSource.CreateLinkedTokenSource(_stopToken);
            _normalizedEffectiveMaxMessageBytes =
                ZLinkClientServerControlProtocol.NormalizeMaximumMessageBytes(
                    socketConfig.MaxMessageSize
                );
            Socket.SetRoutingId(RoutingId.From($"csc-{Guid.NewGuid():N}"));
            ZLinkChannelBundleFactory.ApplySocketConfig(Socket.Options, socketConfig, sendTimeout);
            Socket.Options.Probe = false;
            _monitor = monitoring.OpenSocketMonitor(Socket);
            _receivePoller = ZLinkBackendSocketPoller.Create(Socket);
            _receiveFlowRegistration = await applicationJobQueue
                .RegisterReceiveFlowSocketAsync(Socket)
                .ConfigureAwait(false);
        }

        internal void Start()
        {
            RunState(() =>
            {
                ObjectDisposedException.ThrowIf(IsDisposing, this);
                if (_monitorTask is null)
                {
                    using (ExecutionContext.SuppressFlow())
                    {
                        _monitorTask = Task
                            .Factory.StartNew(
                                static state => ((Connection)state!).RunMonitorLoopAsync(),
                                this,
                                CancellationToken.None,
                                TaskCreationOptions.LongRunning,
                                TaskScheduler.Default
                            )
                            .Unwrap();
                        _controlTask = Task
                            .Factory.StartNew(
                                static state => ((Connection)state!).RunControlLoopAsync(),
                                this,
                                CancellationToken.None,
                                TaskCreationOptions.LongRunning,
                                TaskScheduler.Default
                            )
                            .Unwrap();
                    }
                }
            });
            lock (_socketLifecycleGate)
            {
                if (RunState(() => IsDisposing))
                    return;
                Socket.Connect(_endpoint);
            }
        }

        public ValueTask DisposeAsync() =>
            new(
                ZLinkRuntimeTaskRunner.RunDisposal(
                    ref _disposeTask,
                    async () =>
                    {
                        RunState(() =>
                        {
                            _ready = false;
                            PublishReadyTargetUnderLock();
                        });
                        await DisposeCoreAsync().ConfigureAwait(false);
                    }
                )
            );

        private async Task DisposeCoreAsync()
        {
            var failures = new ZLinkFailureCollector();
            if (_admissionStop is not null)
                await failures
                    .CaptureAsync(() => new ValueTask(_admissionStop.CancelAsync()))
                    .ConfigureAwait(false);
            var (requestTasks, controlTask, livenessTask, monitorTask) = RunState(() =>
                (_requestTasks.ToArray(), _controlTask, _livenessTask, _monitorTask)
            );
            foreach (var requestTask in requestTasks)
                await failures
                    .CaptureAsync(() => new ValueTask(IgnoreCancellationAsync(requestTask)))
                    .ConfigureAwait(false);
            if (controlTask is not null)
                await failures
                    .CaptureAsync(() => new ValueTask(IgnoreCancellationAsync(controlTask)))
                    .ConfigureAwait(false);
            if (livenessTask is not null)
                await failures
                    .CaptureAsync(() => new ValueTask(IgnoreCancellationAsync(livenessTask)))
                    .ConfigureAwait(false);
            if (monitorTask is not null)
                await failures
                    .CaptureAsync(() => new ValueTask(IgnoreCancellationAsync(monitorTask)))
                    .ConfigureAwait(false);
            await failures
                .CaptureAsync(() =>
                    ZLinkReceiveFlowController.DisposeRegistrationAsync(_receiveFlowRegistration)
                )
                .ConfigureAwait(false);
            if (_monitor is not null)
                await failures.CaptureAsync(_monitor.DisposeAsync).ConfigureAwait(false);
            if (_receivePoller is not null)
                failures.Capture(_receivePoller.Dispose);
            failures.ThrowIfAny();
            await failures.CaptureAsync(DisposeSocketAsync).ConfigureAwait(false);
            failures.ThrowIfAny();
            if (_admissionStop is not null)
                _admissionStop.Dispose();
            await _lane.DisposeAsync().ConfigureAwait(false);
        }

        private ValueTask DisposeSocketAsync()
        {
            lock (_socketLifecycleGate)
                return Socket.DisposeAsync();
        }

        private static async Task IgnoreCancellationAsync(Task task)
        {
            try
            {
                await task.ConfigureAwait(false);
            }
            catch (OperationCanceledException) { }
        }

        private void OnMonitorEvent(ZLinkBackendSocketMonitorEvent value)
        {
            switch (value.NativeEvent)
            {
                case ZLinkSocketNativeEventType.ConnectionReady:
                    // This DEALER owns one endpoint. Its disconnected count
                    // snapshot is zero and cannot start a new handshake.
                    if (value.Value == 0)
                        return;
                    var shouldStartAdmission = RunState(() =>
                    {
                        if (IsDisposing)
                            return false;
                        if (
                            _expected is { } expected
                            && (value.RoutingId is not { } actual || actual != expected.ServerRid)
                        )
                        {
                            _ready = false;
                            _rejected = true;
                            _admissionCompleted = true;
                            PublishReadyTargetUnderLock();
                            return false;
                        }
                        return true;
                    });
                    if (shouldStartAdmission)
                        TryStartAdmission();
                    break;
                case ZLinkSocketNativeEventType.Disconnected:
                case ZLinkSocketNativeEventType.Closed:
                    // Core owns the endpoint reconnect (transport liveness §6);
                    // the connect intent stays and the next READY re-admits.
                    RunState(() =>
                    {
                        if (!IsDisposing)
                            FencePhysicalConnection("transport:disconnected");
                    });
                    break;
                case ZLinkSocketNativeEventType.HandshakeFailedNoDetail:
                case ZLinkSocketNativeEventType.HandshakeFailedProtocol:
                case ZLinkSocketNativeEventType.HandshakeFailedAuth:
                    RunState(() =>
                    {
                        if (!IsDisposing)
                            FencePhysicalConnection("transport:handshake-failed");
                    });
                    break;
            }
        }

        private async Task RunMonitorLoopAsync()
        {
            var cancellationToken = _admissionStop.Token;
            while (!cancellationToken.IsCancellationRequested)
            {
                try
                {
                    if (!_monitor.Wait(ControlReceivePollInterval))
                        continue;
                    while (_monitor.TryRecv(out var monitorEvent))
                        OnMonitorEvent(monitorEvent);
                }
                catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
                {
                    return;
                }
                catch (ObjectDisposedException)
                {
                    return;
                }
                await Task.Yield();
            }
        }

        private void TryStartAdmission()
        {
            RunState(() =>
            {
                if (IsDisposing || _admissionStarted || _admissionCompleted)
                    return;
                _admissionStarted = true;
                var physicalGeneration = _physicalGeneration;
                var attempt = ++_admissionAttempt;
                Task admissionTask;
                using (ExecutionContext.SuppressFlow())
                    admissionTask = RunAdmissionAsync(
                        physicalGeneration,
                        attempt,
                        _admissionStop.Token
                    );
                _requestTasks.RemoveAll(static candidate =>
                    candidate.IsCompletedSuccessfully || candidate.IsCanceled
                );
                _requestTasks.Add(admissionTask);
            });
        }

        private async Task RunAdmissionAsync(
            ulong physicalGeneration,
            ulong attempt,
            CancellationToken cancellationToken
        )
        {
            // TryStartAdmission invokes this method while holding the
            // connection state lock. Keep a synchronously completing request
            // from re-entering ApplyAdmission and the parent runtime callback
            // before that lock has been released.
            await Task.Yield();
            var retryAdmission = false;
            try
            {
                var hello = ZLinkClientServerControlProtocol.EncodeHello(
                    new ZLinkClientServerControlProtocol.Hello(
                        _channelName,
                        ZLinkTransportSecurityIdentity.Plaintext,
                        _normalizedEffectiveMaxMessageBytes
                    )
                );
                IReadOnlyList<Message> reply;
                try
                {
                    reply = await ZLinkRequestSubmissionOutcome
                        .SubmitAndAwaitReplyAsync(
                            Socket.Request().Message(hello).Timeout(_admissionTimeout),
                            cancellationToken
                        )
                        .ConfigureAwait(false);
                }
                catch
                {
                    hello.Dispose();
                    throw;
                }
                try
                {
                    _ = ApplyAdmission(reply, physicalGeneration, attempt);
                }
                finally
                {
                    ZLinkMessageParts.DisposeAll(reply);
                }
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested) { }
            catch (ZlinkRequestException exception)
                when (exception.Result == ZlinkRequestException.ErrorCode.TimedOut)
            {
                // The request deadline itself paces the next service
                // handshake; the physical connect intent remains Core-owned.
                retryAdmission = RunState(() =>
                {
                    if (!IsCurrentAttempt(physicalGeneration, attempt))
                        return false;
                    _ready = false;
                    _diagnostics = $"request:{exception.GetType().Name}:{exception.Message}";
                    PublishReadyTargetUnderLock();
                    return true;
                });
            }
            catch (Exception exception)
            {
                RunState(() =>
                {
                    if (!IsCurrentAttempt(physicalGeneration, attempt))
                        return;
                    _ready = false;
                    _admissionCompleted = true;
                    _rejected = true;
                    _diagnostics = $"request:{exception.GetType().Name}:{exception.Message}";
                    PublishReadyTargetUnderLock();
                });
            }
            finally
            {
                var shouldRetry = RunState(() =>
                {
                    if (!IsCurrentAttempt(physicalGeneration, attempt))
                        return false;
                    _admissionStarted = false;
                    return retryAdmission;
                });
                if (shouldRetry)
                    TryStartAdmission();
            }
        }

        private bool ApplyAdmission(
            IReadOnlyList<Message> reply,
            ulong physicalGeneration,
            ulong attempt
        )
        {
            ZLinkClientServerServerDescriptor? expected;
            string? admittedIdentity = null;
            expected = RunState(() => _expected);
            try
            {
                if (
                    !RunState(() =>
                    {
                        if (!IsCurrentAttempt(physicalGeneration, attempt))
                            return false;
                        return true;
                    })
                )
                    return false;
                if (
                    ZLinkClientServerControlProtocol.TryDecodeReject(reply, out _)
                    || !ZLinkClientServerControlProtocol.TryDecodeAdmission(
                        reply,
                        out var admission
                    )
                    || admission is null
                    || !StringComparer.Ordinal.Equals(admission.ChannelName, _channelName)
                    || admission.LifecycleGeneration == 0
                    || admission.Weight is < 0 or > ZLinkSocketConfig.MaximumPeerWeight
                    || admission.NormalizedEffectiveMaxMessageBytes == 0
                    || admission.NormalizedEffectiveMaxMessageBytes
                        > _normalizedEffectiveMaxMessageBytes
                    || expected is not null
                        && (
                            admission.ServerRid != expected.ServerRid
                            || admission.LifecycleGeneration != expected.LifecycleGeneration
                            || admission.DescriptorRevision != expected.DescriptorRevision
                            || admission.Weight != expected.Weight
                            || admission.State != expected.State
                            || !StringComparer.Ordinal.Equals(
                                admission.AdvertisedEndpoint,
                                expected.Endpoint
                            )
                            || !ZLinkClientServerControlProtocol.SecurityIdentityMatches(
                                expected.SecurityIdentity,
                                admission.SecurityIdentity
                            )
                        )
                )
                {
                    return RunState(() =>
                    {
                        if (!IsCurrentAttempt(physicalGeneration, attempt))
                            return false;
                        _ready = false;
                        _rejected = true;
                        _admissionCompleted = true;
                        _diagnostics =
                            reply.Count == 0
                                ? "invalid:empty"
                                : $"invalid:{Convert.ToHexString(
                                reply[0].AsReadOnlyMemory().Span)}";
                        PublishReadyTargetUnderLock();
                        return false;
                    });
                }
                var accepted = RunState(() =>
                {
                    if (IsCurrentAttempt(physicalGeneration, attempt))
                    {
                        _admissionCompleted = true;
                        _weight = admission.Weight;
                        _ready = admission.State == ZLinkFrameworkRuntimeState.Serving;
                        admittedIdentity = IdentityOf(
                            admission.ServerRid,
                            admission.LifecycleGeneration
                        );
                        _admittedDescriptor = admission;
                        _lastPeerActivity = _time.GetTimestamp();
                        _diagnostics = "ready";
                        using (ExecutionContext.SuppressFlow())
                        {
                            _livenessTask ??= RunLivenessLoopAsync(_admissionStop.Token);
                        }
                        PublishReadyTargetUnderLock();
                        return true;
                    }
                    return false;
                });
                if (!accepted)
                    return false;
            }
            catch (Exception exception)
            {
                _errorSink.ReportRuntimeTaskException(nameof(RunAdmissionAsync), exception);
                return RunState(() =>
                {
                    if (!IsCurrentAttempt(physicalGeneration, attempt))
                        return false;
                    _ready = false;
                    _rejected = true;
                    _admissionCompleted = true;
                    _diagnostics = $"invalid:{exception.GetType().Name}:{exception.Message}";
                    PublishReadyTargetUnderLock();
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
            using var received = Received.Create();
            while (!cancellationToken.IsCancellationRequested)
            {
                try
                {
                    var readiness = _receivePoller.Wait(ControlReceivePollInterval);
                    var admissionEstablished = RunState(() =>
                        !IsDisposing && CurrentAdmission is not null
                    );
                    if (!admissionEstablished)
                        continue;
                    if (
                        (
                            readiness
                            & (
                                ZLinkBackendSocketReadiness.Readable
                                | ZLinkBackendSocketReadiness.Error
                                | ZLinkBackendSocketReadiness.Priority
                            )
                        ) == 0
                    )
                        continue;
                    if (!Socket.Recv(received, RecvFlags.DontWait))
                        continue;
                    if (
                        ZLinkClientServerControlProtocol.TryDecodeLivenessProbe(
                            received.Parts,
                            out var probeId
                        )
                    )
                    {
                        Interlocked.Increment(ref _receivedLivenessProbeCount);
                        var ack = ZLinkClientServerControlProtocol.EncodeLivenessAck(probeId);
                        if (received.ReplyToken is not null)
                            ReplyOwned(received, ack);
                        else
                            await SendOwnedAsync(ack, cancellationToken).ConfigureAwait(false);
                        continue;
                    }
                    if (
                        ZLinkClientServerControlProtocol.TryDecodeUpdate(
                            received.Parts,
                            out var update
                        ) && update is not null
                    )
                    {
                        ApplyUpdate(update);
                        continue;
                    }
                    if (
                        ZLinkClientServerControlProtocol.TryDecodeLivenessAck(
                            received.Parts,
                            out var ackId
                        )
                    )
                    {
                        AcceptLivenessAck(ackId);
                        continue;
                    }
                    var restartReason = ZLinkClientServerControlProtocol.IsControl(received.Parts)
                        ? "protocol:pushed-control"
                        : "protocol:unsolicited-application";
                    // Release the native receive parts before restarting the
                    // admission on the same socket.
                    received.Dispose();
                    RestartAdmission(restartReason);
                }
                catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
                {
                    break;
                }
                catch (Exception exception)
                {
                    RunState(() =>
                    {
                        _ready = false;
                        _diagnostics = $"control:{exception.GetType().Name}:{exception.Message}";
                        PublishReadyTargetUnderLock();
                    });
                    throw;
                }
            }
        }

        private async Task RunLivenessLoopAsync(CancellationToken cancellationToken)
        {
            while (!cancellationToken.IsCancellationRequested)
            {
                await Task.Delay(ZLinkServiceLiveness.ProbeInterval, _time, cancellationToken)
                    .ConfigureAwait(false);
                var timedOut = RunState(() =>
                {
                    if (IsDisposing || CurrentAdmission is null)
                        return false;
                    if (_time.GetElapsedTime(_lastPeerActivity) >= ZLinkServiceLiveness.PeerTimeout)
                        return true;

                    _outstandingProbeId ??= AllocateProbeId();
                    Task requestTask;
                    using (ExecutionContext.SuppressFlow())
                        requestTask = RequestLivenessProbeAsync(
                            _outstandingProbeId.Value,
                            _physicalGeneration,
                            cancellationToken
                        );
                    _requestTasks.RemoveAll(static candidate =>
                        candidate.IsCompletedSuccessfully || candidate.IsCanceled
                    );
                    _requestTasks.Add(requestTask);
                    return false;
                });
                if (timedOut)
                    RestartAdmission("liveness:timeout");
            }
        }

        private async Task RequestLivenessProbeAsync(
            ulong probeId,
            ulong physicalGeneration,
            CancellationToken cancellationToken
        )
        {
            using var probe = ZLinkClientServerControlProtocol.EncodeLivenessProbe(probeId);
            IReadOnlyList<Message> reply;
            try
            {
                // Keep the Core request/reply envelope; reply completion must
                // not delay the next probe or the connection's deadline check.
                var request = ZLinkRequestSubmissionOutcome.SubmitAndAwaitReplyAsync(
                    Socket.Request().Message(probe).Timeout(ZLinkServiceLiveness.PeerTimeout),
                    cancellationToken
                );
                Interlocked.Increment(ref _sentLivenessProbeCount);
                reply = await request.ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                return;
            }
            catch (ZlinkRequestException exception)
                when (exception.Result
                        is ZlinkRequestException.ErrorCode.TimedOut
                            or ZlinkRequestException.ErrorCode.NotConnected
                            or ZlinkRequestException.ErrorCode.NotFound
                            or ZlinkRequestException.ErrorCode.Terminated
                )
            {
                // Only a matching ACK supplies liveness evidence. The single
                // periodic loop owns expiry when no evidence arrives.
                return;
            }
            try
            {
                if (ZLinkClientServerControlProtocol.TryDecodeLivenessAck(reply, out var ackId))
                    AcceptLivenessAck(ackId, physicalGeneration);
            }
            finally
            {
                ZLinkMessageParts.DisposeAll(reply);
            }
        }

        private void AcceptLivenessAck(ulong ackId, ulong? physicalGeneration = null)
        {
            var accepted = RunState(() =>
            {
                if (
                    IsDisposing
                    || physicalGeneration is { } generation && _physicalGeneration != generation
                    || _outstandingProbeId != ackId
                )
                    return false;
                _outstandingProbeId = null;
                _lastPeerActivity = _time.GetTimestamp();
                if (CurrentAdmission is { State: ZLinkFrameworkRuntimeState.Serving })
                    _ready = true;
                PublishReadyTargetUnderLock();
                return true;
            });
            if (!accepted)
                return;
            Interlocked.Increment(ref _livenessAckCount);
        }

        private void PublishReadyTargetUnderLock()
        {
            var current = Volatile.Read(ref _readyTarget);
            if (!_ready || IsDisposing || _weight <= 0)
            {
                if (current is null)
                {
                    _onStateChanged(false);
                    return;
                }
                Volatile.Write(ref _readyTarget, null);
                _onStateChanged(true);
                return;
            }

            var serverRid =
                CurrentAdmission?.ServerRid
                ?? _expected?.ServerRid
                ?? throw new InvalidOperationException(
                    "A ready ClientServer connection has no Server RID."
                );
            var maximumMessageBytes =
                CurrentAdmission?.NormalizedEffectiveMaxMessageBytes
                ?? _normalizedEffectiveMaxMessageBytes;
            if (
                current is not null
                && ReferenceEquals(current.Socket, Socket)
                && current.AdmittedMaximumMessageBytes == maximumMessageBytes
                && current.SelectionServerRid == serverRid
                && current.Weight == _weight
            )
            {
                _onStateChanged(false);
                return;
            }

            Volatile.Write(
                ref _readyTarget,
                new ReadyTarget(Socket, maximumMessageBytes, serverRid, _weight)
            );
            _onStateChanged(true);
        }

        private bool IsCurrentAttempt(ulong physicalGeneration, ulong attempt) =>
            !IsDisposing
            && _physicalGeneration == physicalGeneration
            && _admissionAttempt == attempt;

        private void FencePhysicalConnection(string diagnostics)
        {
            _physicalGeneration++;
            _admissionAttempt++;
            _admissionStarted = false;
            _admissionCompleted = false;
            _rejected = false;
            _ready = false;
            _outstandingProbeId = null;
            _diagnostics = diagnostics;
            PublishReadyTargetUnderLock();
        }

        // A peer deadline or an invalid pushed control ends only the current
        // logical admission. The connect intent stays with Core, which owns
        // the endpoint reconnect (transport liveness §6); the next service
        // handshake starts on the existing admission path.
        private void RestartAdmission(string diagnostics)
        {
            var restart = RunState(() =>
            {
                if (IsDisposing || CurrentAdmission is null)
                    return false;
                FencePhysicalConnection(diagnostics);
                return true;
            });
            if (restart)
                TryStartAdmission();
        }

        private void ApplyUpdate(ZLinkClientServerControlProtocol.Admission update)
        {
            RunState(() =>
            {
                var current = CurrentAdmission;
                if (
                    current is null
                    || update.ChannelName != current.ChannelName
                    || update.ServerRid != current.ServerRid
                    || update.LifecycleGeneration != current.LifecycleGeneration
                    || update.SecurityIdentity != current.SecurityIdentity
                    || update.AdvertisedEndpoint != current.AdvertisedEndpoint
                    || update.NormalizedEffectiveMaxMessageBytes
                        != current.NormalizedEffectiveMaxMessageBytes
                )
                {
                    _ready = false;
                    _diagnostics = "invalid:update-identity";
                    PublishReadyTargetUnderLock();
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
                        PublishReadyTargetUnderLock();
                    }
                    return;
                }
                _admittedDescriptor = update;
                _weight = update.Weight;
                _ready = update.State == ZLinkFrameworkRuntimeState.Serving;
                _diagnostics = _ready ? "ready" : "update:not-ready";
                PublishReadyTargetUnderLock();
            });
        }

        private async ValueTask SendOwnedAsync(Message message, CancellationToken cancellationToken)
        {
            try
            {
                await Socket
                    .Send()
                    .Message(message)
                    .Async(cancellationToken)
                    .EnsureAcceptedAsync()
                    .ConfigureAwait(false);
            }
            finally
            {
                message.Dispose();
            }
        }

        private static void ReplyOwned(Received received, Message message)
        {
            try
            {
                // A request envelope needs the correlated raw reply terminal.
                // Binding admission owns DEALER reply backpressure. This loop
                // observes a refused submission through its failure path.
                received.Reply().Message(message).Submit();
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

        private T RunState<T>(Func<T> work) => AwaitStateLane(_lane.RunAsync(work));

        private void RunState(Action work) => AwaitStateLane(_lane.RunAsync(work));

        private static string IdentityOf(RoutingId serverRid, ulong lifecycleGeneration) =>
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
    string? LastFailure
);
