using Zlink.Framework.Runtime.Dispatch;

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
    private readonly string _channelName;
    private readonly IZLinkBackendAdapterFactory _backendAdapterFactory;
    private readonly IZLinkBackendContext _context;
    private readonly IZLinkSocketConfig _socketConfig;
    private readonly ZLinkChannelReceiveLoop _receiveLoop;
    private readonly ZLinkFanoutRuntimeService _monitoring;
    private readonly IZLinkRuntimeFailureReporter _errorSink;
    private readonly ZLinkInboundDispatchBudget _inboundDispatchBudget;
    private readonly CancellationToken _runtimeStopToken;
    private readonly TimeProvider _time;
    private readonly object _gate = new();
    private readonly Dictionary<string, Connection> _connections =
        new(StringComparer.Ordinal);
    private IReadOnlyList<ZLinkFanoutPublisherConnectionSnapshot> _excluded =
        Array.Empty<ZLinkFanoutPublisherConnectionSnapshot>();
    private ZLinkLocationRuntimeSnapshot _location =
        new("unknown", null, null);
    private int _disposed;

    internal ZLinkAutomaticFanoutSubscriberRuntime(
        string channelName,
        IZLinkBackendAdapterFactory backendAdapterFactory,
        IZLinkBackendContext context,
        IZLinkSocketConfig socketConfig,
        ZLinkChannelReceiveLoop receiveLoop,
        ZLinkFanoutRuntimeService monitoring,
        IZLinkRuntimeFailureReporter errorSink,
        CancellationToken runtimeStopToken,
        TimeProvider? timeProvider = null,
        ZLinkInboundDispatchBudget? inboundDispatchBudget = null)
    {
        _channelName = channelName;
        _backendAdapterFactory = backendAdapterFactory;
        _context = context;
        _socketConfig = socketConfig;
        _receiveLoop = receiveLoop;
        _monitoring = monitoring;
        _errorSink = errorSink;
        _runtimeStopToken = runtimeStopToken;
        _inboundDispatchBudget = inboundDispatchBudget
                                 ?? new ZLinkInboundDispatchBudget(0);
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
                static plan => IdentityKey(plan.Descriptor),
                StringComparer.Ordinal);
        List<Connection> removed = [];
        lock (_gate)
        {
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
                connection.Start();
            }
            PublishSnapshotNoLock();
        }

        foreach (var connection in removed)
            await connection.DisposeAsync().ConfigureAwait(false);
    }

    internal void RecordLocationFailure(
        DateTimeOffset? lastSuccessAt,
        DateTimeOffset failureAt)
    {
        lock (_gate)
        {
            _location = new ZLinkLocationRuntimeSnapshot(
                "degraded",
                lastSuccessAt,
                failureAt);
            PublishSnapshotNoLock();
        }
    }

    public async ValueTask DisposeAsync()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0)
            return;
        Connection[] connections;
        lock (_gate)
        {
            connections = _connections.Values.ToArray();
            _connections.Clear();
            _excluded = Array.Empty<ZLinkFanoutPublisherConnectionSnapshot>();
            PublishSnapshotNoLock();
        }
        foreach (var connection in connections)
            await connection.DisposeAsync().ConfigureAwait(false);
    }

    private void ConnectionChanged()
    {
        lock (_gate)
            PublishSnapshotNoLock();
    }

    private void PublishSnapshotNoLock()
    {
        var publishers = _connections.Values
            .Select(static connection => connection.Read())
            .Concat(_excluded)
            .ToArray();
        _monitoring.RecordSnapshot(_channelName, publishers, _location);
    }

    private static string IdentityKey(
        ZLinkFanoutPublisherDescriptor descriptor) =>
        $"{descriptor.PublisherRid.ToHex()}:{descriptor.LifecycleGeneration}";

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

    private sealed class Connection(
        ZLinkAutomaticFanoutSubscriberRuntime owner,
        ZLinkFanoutPublisherDescriptor descriptor) : IAsyncDisposable
    {
        private readonly object _gate = new();
        private readonly CancellationTokenSource _stop =
            CancellationTokenSource.CreateLinkedTokenSource(
                owner._runtimeStopToken);
        private ZLinkFanoutPublisherDescriptor _descriptor = descriptor;
        private ZLinkFanoutPublisherConnectionState _state =
            ZLinkFanoutPublisherConnectionState.Connecting;
        private DateTimeOffset _lastActivity = owner._time.GetUtcNow();
        private string? _lastFailure;
        private bool _ready;
        private Task? _loop;

        internal string Endpoint
        {
            get { lock (_gate) return _descriptor.Endpoint; }
        }

        internal void Start()
        {
            _loop = Task.Run(RunAsync, CancellationToken.None);
        }

        internal void UpdateDescriptor(
            ZLinkFanoutPublisherDescriptor value)
        {
            lock (_gate)
                _descriptor = value;
            owner.ConnectionChanged();
        }

        internal ZLinkFanoutPublisherConnectionSnapshot Read()
        {
            lock (_gate)
                return Snapshot(
                    _descriptor,
                    true,
                    _ready,
                    _state,
                    _lastFailure);
        }

        private async Task RunAsync()
        {
            var reconnecting = false;
            while (!_stop.IsCancellationRequested)
            {
                SetState(
                    reconnecting
                        ? ZLinkFanoutPublisherConnectionState.Reconnecting
                        : ZLinkFanoutPublisherConnectionState.Connecting,
                    ready: false,
                    failure: _lastFailure);
                reconnecting = true;
                await RunAttemptAsync(_stop.Token).ConfigureAwait(false);
                if (_stop.IsCancellationRequested)
                    break;
                SetState(
                    ZLinkFanoutPublisherConnectionState.Disconnected,
                    ready: false,
                    failure: _lastFailure ?? "subscriber connection ended");
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
            IZLinkBackendSubscriberSocket? socket = null;
            using var attempt =
                CancellationTokenSource.CreateLinkedTokenSource(
                    cancellationToken);
            try
            {
                socket = owner._backendAdapterFactory
                    .CreateChannelAdapter()
                    .CreateSubscriberSocket(owner._context);
                socket.SetChannelName(owner._channelName);
                ZLinkChannelBundleFactory.ApplySocketConfig(
                    socket,
                    owner._socketConfig);
                socket.SetSubscription(string.Empty);
                socket.Connect(Endpoint);
                lock (_gate)
                {
                    _lastActivity = owner._time.GetUtcNow();
                    _lastFailure = null;
                }

                var receive = owner._receiveLoop.RunFanoutConnectionLoopAsync(
                    owner._channelName,
                    socket,
                    OnActivity,
                    () =>
                    {
                        lock (_gate)
                            _lastFailure = "invalid fanout liveness beacon";
                    },
                    owner._errorSink,
                    owner._inboundDispatchBudget,
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
                lock (_gate)
                    _lastFailure = exception.Message;
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
                        lock (_gate)
                            _lastFailure ??= exception.Message;
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
                DateTimeOffset lastActivity;
                lock (_gate)
                    lastActivity = _lastActivity;
                if (!ZLinkFanoutLivenessProtocol.IsInboundTimedOut(
                        lastActivity,
                        owner._time.GetUtcNow()))
                    continue;
                lock (_gate)
                    _lastFailure = "fanout publisher inbound timeout";
                await attempt.CancelAsync().ConfigureAwait(false);
                return;
            }
        }

        private void OnActivity()
        {
            lock (_gate)
            {
                _lastActivity = owner._time.GetUtcNow();
                _ready = true;
                _state = ZLinkFanoutPublisherConnectionState.Ready;
                _lastFailure = null;
            }
            owner.ConnectionChanged();
        }

        private void SetState(
            ZLinkFanoutPublisherConnectionState state,
            bool ready,
            string? failure)
        {
            lock (_gate)
            {
                _state = state;
                _ready = ready;
                _lastFailure = failure;
            }
            owner.ConnectionChanged();
        }

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
    }
}
