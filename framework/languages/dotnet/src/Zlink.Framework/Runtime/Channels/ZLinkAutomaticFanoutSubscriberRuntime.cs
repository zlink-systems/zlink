namespace Zlink.Framework.Runtime.Channels;

internal sealed record ZLinkFanoutConnectionPlan(
    ZLinkFanoutPublisherDescriptor Descriptor,
    bool Connect,
    ZLinkFanoutPublisherConnectionState State,
    string? LastFailure = null);

/// <summary>
/// Owns every physical automatic subscriber connection for one ChannelName.
/// Each publisher identity gets one SUB socket, receive loop and liveness
/// deadline so activity from another publisher cannot keep it ready.
/// </summary>
internal sealed class ZLinkAutomaticFanoutSubscriberRuntime
    : IAsyncDisposable
{
    private static readonly TimeSpan ReconnectDelay =
        TimeSpan.FromSeconds(1);
    private readonly ZLinkChannelName _channelName;
    private readonly IZLinkBackendRuntimeContext _context;
    private readonly IZLinkSocketConfig _socketConfig;
    private readonly ZLinkChannelReceiveLoop _receiveLoop;
    private readonly ZLinkFanoutRuntimeService _monitoring;
    private readonly ZLinkApplicationJobQueue _applicationJobQueue;
    private readonly bool _ownsApplicationJobQueue;
    private readonly IZLinkRuntimeFailureReporter _errorSink;
    private readonly CancellationToken _runtimeStopToken;
    private readonly TimeProvider _time;
    private readonly ZLinkStateLane _lane = new();
    private readonly Dictionary<
        (RoutingId PublisherRid, ulong LifecycleGeneration),
        Connection> _connections = [];
    private IReadOnlyList<ZLinkFanoutPublisherConnectionSnapshot> _excluded =
        Array.Empty<ZLinkFanoutPublisherConnectionSnapshot>();
    private ZLinkLocationRuntimeSnapshot _location =
        new("unknown", null, null);
    private int _disposed;
    private long _socketCreationCount;

    internal long SocketCreationCount =>
        Volatile.Read(ref _socketCreationCount);

    internal ZLinkAutomaticFanoutSubscriberRuntime(
        string channelName,
        IZLinkBackendRuntimeContext context,
        IZLinkSocketConfig socketConfig,
        ZLinkChannelReceiveLoop receiveLoop,
        ZLinkFanoutRuntimeService monitoring,
        IZLinkRuntimeFailureReporter errorSink,
        CancellationToken runtimeStopToken,
        TimeProvider? timeProvider = null,
        ZLinkApplicationJobQueue? applicationJobQueue = null)
    {
        _channelName = ZLinkChannelName.FromBoundary(
            channelName,
            nameof(channelName));
        _context = context;
        _socketConfig = socketConfig;
        _receiveLoop = receiveLoop;
        _monitoring = monitoring;
        _ownsApplicationJobQueue = applicationJobQueue is null;
        _applicationJobQueue = applicationJobQueue
            ?? new ZLinkApplicationJobQueue(
                ZLinkApplicationJobQueueCapacityResolver.Resolve(
                    ZLinkApplicationJobQueueProfile.Balanced,
                    int.MaxValue,
                    1));
        _errorSink = errorSink;
        _runtimeStopToken = runtimeStopToken;
        _time = timeProvider ?? TimeProvider.System;
    }

    internal async ValueTask ReplaceAsync(
        IReadOnlyList<ZLinkFanoutConnectionPlan> plans,
        ZLinkLocationRuntimeSnapshot location)
    {
        ObjectDisposedException.ThrowIf(
            Volatile.Read(ref _disposed) != 0,
            this);
        var desired = plans
            .Where(static plan => plan.Connect)
            .ToDictionary(
                static plan => IdentityKey(plan.Descriptor));
        var removed = await _lane.RunAsync(() =>
        {
            List<Connection> removed = [];
            _location = location;
            _excluded = plans
                .Where(static plan => !plan.Connect)
                .Select(static plan => Snapshot(
                    plan.Descriptor,
                    false,
                    false,
                    plan.State,
                    plan.LastFailure))
                .ToArray();

            foreach (var (key, connection) in _connections.ToArray())
            {
                if (desired.Remove(key, out var plan)
                    && string.Equals(
                        connection.Endpoint,
                        plan.Descriptor.Endpoint,
                        StringComparison.Ordinal))
                {
                    connection.UpdateDescriptor(plan.Descriptor);
                    continue;
                }

                _connections.Remove(key);
                removed.Add(connection);
            }

            foreach (var (key, plan) in desired)
            {
                var connection = new Connection(this, plan.Descriptor);
                _connections.Add(key, connection);
                StartConnection(connection);
            }
            PublishSnapshotNoLock();
            return removed;
        }).ConfigureAwait(false);

        foreach (var connection in removed)
            await connection.DisposeAsync().ConfigureAwait(false);
    }

    internal async ValueTask ClearAsync(
        ZLinkLocationRuntimeSnapshot location)
    {
        var removed = await _lane.RunAsync(() =>
        {
            if (Volatile.Read(ref _disposed) != 0)
                return Array.Empty<Connection>();
            _location = location;
            _excluded = Array.Empty<ZLinkFanoutPublisherConnectionSnapshot>();
            var removed = _connections.Values.ToArray();
            _connections.Clear();
            PublishSnapshotNoLock();
            return removed;
        }).ConfigureAwait(false);

        foreach (var connection in removed)
            await connection.DisposeAsync().ConfigureAwait(false);
    }

    internal void RecordLocationFailure(
        DateTimeOffset? lastSuccessAt,
        DateTimeOffset failureAt)
    {
        AwaitStateLane(_lane.RunAsync(() =>
        {
            _location = new ZLinkLocationRuntimeSnapshot(
                "degraded",
                lastSuccessAt,
                failureAt);
            PublishSnapshotNoLock();
        }));
    }

    public async ValueTask DisposeAsync()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0)
            return;
        var connections = await _lane.RunAsync(() =>
        {
            var connections = _connections.Values.ToArray();
            _connections.Clear();
            _excluded = Array.Empty<ZLinkFanoutPublisherConnectionSnapshot>();
            PublishSnapshotNoLock();
            return connections;
        }).ConfigureAwait(false);
        foreach (var connection in connections)
            await connection.DisposeAsync().ConfigureAwait(false);
        if (_ownsApplicationJobQueue)
            _applicationJobQueue.Dispose();
    }

    private void ConnectionChanged()
    {
        if (_lane.IsOnLane)
        {
            PublishSnapshotNoLock();
            return;
        }

        AwaitStateLane(_lane.RunAsync(PublishSnapshotNoLock));
    }

    private static void StartConnection(Connection connection)
    {
        if (ExecutionContext.IsFlowSuppressed())
        {
            connection.Start();
            return;
        }

        using (ExecutionContext.SuppressFlow())
            connection.Start();
    }

    private void PublishSnapshotNoLock()
    {
        var publishers = _connections.Values
            .Select(static connection => connection.Read())
            .Concat(_excluded)
            .ToArray();
        _monitoring.RecordSnapshot(_channelName.Value, publishers, _location);
    }

    private static (RoutingId PublisherRid, ulong LifecycleGeneration)
        IdentityKey(
        ZLinkFanoutPublisherDescriptor descriptor) =>
        (descriptor.PublisherRid, descriptor.LifecycleGeneration);

    private static ZLinkFanoutPublisherConnectionSnapshot Snapshot(
        ZLinkFanoutPublisherDescriptor descriptor,
        bool connectionIntent,
        bool ready,
        ZLinkFanoutPublisherConnectionState state,
        string? lastFailure) =>
        new(
            descriptor.PublisherRid,
            descriptor.LifecycleGeneration,
            descriptor.DescriptorRevision,
            descriptor.Endpoint,
            connectionIntent,
            ready,
            state,
            lastFailure);

    private static void AwaitStateLane(ValueTask operation) =>
        operation.GetAwaiter().GetResult();

    private sealed class Connection(
        ZLinkAutomaticFanoutSubscriberRuntime owner,
        ZLinkFanoutPublisherDescriptor descriptor) : IAsyncDisposable
    {
        private readonly ZLinkStateLane _lane = new();
        private readonly CancellationTokenSource _stop =
            CancellationTokenSource.CreateLinkedTokenSource(
                owner._runtimeStopToken);
        private ZLinkFanoutPublisherDescriptor _descriptor = descriptor;
        private ZLinkFanoutPublisherConnectionState _state =
            ZLinkFanoutPublisherConnectionState.Connecting;
        private long _lastActivity = owner._time.GetTimestamp();
        private string? _lastFailure;
        private bool _ready;
        private Task? _loop;

        internal string Endpoint
        {
            get => AwaitStateLane(_lane.RunAsync(() => _descriptor.Endpoint));
        }

        internal void Start()
        {
            AwaitStateLane(_lane.RunAsync(StartCore));
        }

        internal void UpdateDescriptor(
            ZLinkFanoutPublisherDescriptor value)
        {
            AwaitStateLane(_lane.RunAsync(() => _descriptor = value));
            owner.ConnectionChanged();
        }

        internal ZLinkFanoutPublisherConnectionSnapshot Read()
        {
            return AwaitStateLane(_lane.RunAsync(() =>
                Snapshot(
                    _descriptor,
                    true,
                    _ready,
                    _state,
                    _lastFailure)));
        }

        private void StartCore()
        {
            if (ExecutionContext.IsFlowSuppressed())
            {
                _loop = Task.Run(RunAsync, CancellationToken.None);
                return;
            }

            using (ExecutionContext.SuppressFlow())
                _loop = Task.Run(RunAsync, CancellationToken.None);
        }

        private async Task RunAsync()
        {
            var reconnecting = false;
            while (!_stop.IsCancellationRequested)
            {
                await SetStateAsync(
                    reconnecting
                        ? ZLinkFanoutPublisherConnectionState.Reconnecting
                        : ZLinkFanoutPublisherConnectionState.Connecting,
                    ready: false,
                    failure: await ReadLastFailureAsync().ConfigureAwait(false))
                    .ConfigureAwait(false);
                reconnecting = true;
                await RunAttemptAsync(_stop.Token).ConfigureAwait(false);
                if (_stop.IsCancellationRequested)
                    break;
                var lastFailure = await ReadLastFailureAsync().ConfigureAwait(false);
                await SetStateAsync(
                    ZLinkFanoutPublisherConnectionState.Disconnected,
                    ready: false,
                    failure: lastFailure ?? "subscriber connection ended")
                    .ConfigureAwait(false);
                try
                {
                    await Task.Delay(
                            ReconnectDelay,
                            owner._time,
                            _stop.Token)
                        .ConfigureAwait(false);
                }
                catch (OperationCanceledException)
                {
                    break;
                }
            }
        }

        private async Task RunAttemptAsync(CancellationToken cancellationToken)
        {
            ISubSocket? socket = null;
            using var attempt =
                CancellationTokenSource.CreateLinkedTokenSource(
                    cancellationToken);
            try
            {
                socket = owner._context.CreateSubscriberSocket();
                Interlocked.Increment(ref owner._socketCreationCount);
                ZLinkChannelBundleFactory.ApplySocketConfig(
                    socket.Options,
                    owner._socketConfig);
                socket.SetSubscription(string.Empty);
                socket.Connect(await _lane.RunAsync(
                    () => _descriptor.Endpoint).ConfigureAwait(false));
                await _lane.RunAsync(() =>
                {
                    _lastActivity = owner._time.GetTimestamp();
                    _lastFailure = null;
                }).ConfigureAwait(false);

                var receive = owner._receiveLoop.RunFanoutConnectionLoopAsync(
                    owner._channelName.Value,
                    socket,
                    OnActivity,
                    () => SetFailure("invalid fanout liveness beacon"),
                    owner._applicationJobQueue,
                    owner._errorSink,
                    attempt.Token);
                var watchdog = WatchInboundAsync(attempt);
                _ = await Task.WhenAny(receive, watchdog)
                    .ConfigureAwait(false);
                await attempt.CancelAsync().ConfigureAwait(false);
                await AwaitAttemptTaskAsync(receive).ConfigureAwait(false);
                await AwaitAttemptTaskAsync(watchdog).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
                when (cancellationToken.IsCancellationRequested)
            {
            }
            catch (Exception exception)
            {
                await _lane.RunAsync(() => _lastFailure = exception.Message)
                    .ConfigureAwait(false);
            }
            finally
            {
                if (socket is not null)
                    try
                    {
                        await socket.DisposeAsync().ConfigureAwait(false);
                    }
                    catch (Exception exception)
                    {
                        await _lane.RunAsync(
                            () => _lastFailure ??= exception.Message)
                            .ConfigureAwait(false);
                    }
            }
        }

        private async Task WatchInboundAsync(
            CancellationTokenSource attempt)
        {
            while (!attempt.IsCancellationRequested)
            {
                await Task.Delay(
                        TimeSpan.FromMilliseconds(250),
                        owner._time,
                        attempt.Token)
                    .ConfigureAwait(false);
                var lastActivity = await _lane.RunAsync(
                    () => _lastActivity).ConfigureAwait(false);
                if (!ZLinkFanoutLivenessProtocol.IsInboundTimedOut(
                        owner._time.GetElapsedTime(lastActivity)))
                    continue;
                await _lane.RunAsync(
                    () => _lastFailure = "fanout publisher inbound timeout")
                    .ConfigureAwait(false);
                await attempt.CancelAsync().ConfigureAwait(false);
                return;
            }
        }

        private void OnActivity()
        {
            AwaitStateLane(_lane.RunAsync(() =>
            {
                _lastActivity = owner._time.GetTimestamp();
                _ready = true;
                _state = ZLinkFanoutPublisherConnectionState.Ready;
                _lastFailure = null;
            }));
            owner.ConnectionChanged();
        }

        private async ValueTask SetStateAsync(
            ZLinkFanoutPublisherConnectionState state,
            bool ready,
            string? failure)
        {
            await _lane.RunAsync(() =>
            {
                _state = state;
                _ready = ready;
                _lastFailure = failure;
            }).ConfigureAwait(false);
            owner.ConnectionChanged();
        }

        private ValueTask<string?> ReadLastFailureAsync() =>
            _lane.RunAsync(() => _lastFailure);

        private void SetFailure(string failure) =>
            AwaitStateLane(_lane.RunAsync(() => _lastFailure = failure));

        public async ValueTask DisposeAsync()
        {
            await _stop.CancelAsync().ConfigureAwait(false);
            if (_loop is not null)
                try
                {
                    await _loop.ConfigureAwait(false);
                }
                catch (OperationCanceledException)
                {
            }
            _stop.Dispose();
            await _lane.DisposeAsync().ConfigureAwait(false);
        }

        private static async Task AwaitAttemptTaskAsync(Task task)
        {
            try
            {
                await task.ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
            }
        }

        private static T AwaitStateLane<T>(ValueTask<T> operation) =>
            operation.GetAwaiter().GetResult();

        private static void AwaitStateLane(ValueTask operation) =>
            operation.GetAwaiter().GetResult();
    }
}
