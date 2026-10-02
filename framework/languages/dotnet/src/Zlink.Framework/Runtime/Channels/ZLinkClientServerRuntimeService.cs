using System.Runtime.CompilerServices;
using System.Threading.Channels;
using Zlink.Framework.Runtime.Diagnostics;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.Runtime.Channels;

internal sealed class ZLinkClientServerRuntimeService(
    ZLinkFrameworkRuntime runtime,
    ZLinkFrameworkHostLifecycleState hostLifecycle,
    ZLinkLocationStoreHealth? storeHealth
) : IZLinkClientServerRuntime, IAsyncDisposable
{
    private readonly object _gate = new();
    private readonly ZLinkFrameworkRuntime _runtime = runtime;
    private readonly ZLinkFrameworkHostLifecycleState _hostLifecycle = hostLifecycle;
    private readonly ZLinkLocationStoreHealth? _storeHealth = storeHealth;
    private readonly Dictionary<ZLinkChannelName, ZLinkClientServerStatus> _statuses = [];
    private readonly Dictionary<ZLinkChannelName, MonitorHub> _monitorHubs = [];

    private ZLinkClientServerChannelSnapshot SnapshotInternal(string channelName)
    {
        ArgumentException.ThrowIfNullOrEmpty(channelName);
        var channel = ZLinkChannelName.FromBoundary(channelName, nameof(channelName));
        var state = _runtime.ClientServerMonitoringState(channelName);
        if (_monitorHubs.TryGetValue(channel, out var hub))
            hub.RefreshClientSubscription(state.Client);
        var servers =
            state
                .Client?.SnapshotConnections()
                .Where(static entry => entry.ServerRid is not null)
                .Select(static entry => new ZLinkClientServerServerSnapshot(
                    entry.ServerRid!.Value,
                    entry.LifecycleGeneration!.Value,
                    entry.DescriptorRevision!.Value,
                    entry.Endpoint,
                    entry.Weight,
                    entry.Ready,
                    entry.State,
                    entry.DescriptorSource,
                    entry.LastFailure
                ))
                .ToList()
            ?? [];

        if (state.Server is { } localServer)
        {
            var local = AwaitStateLane(localServer.ReadAsync());
            var existing = servers.FindIndex(entry =>
                entry.ServerRid == localServer.ServerRid
                && entry.LifecycleGeneration == localServer.LifecycleGeneration
            );
            if (existing < 0)
                servers.Add(
                    new ZLinkClientServerServerSnapshot(
                        localServer.ServerRid,
                        localServer.LifecycleGeneration,
                        local.Revision,
                        local.AdvertisedEndpoint,
                        local.Weight,
                        local.State == ZLinkFrameworkRuntimeState.Serving && local.Weight > 0,
                        Map(local.State),
                        "manual",
                        LastFailure: null
                    )
                );
        }

        servers.Sort(
            static (left, right) =>
            {
                var rid = StringComparer.Ordinal.Compare(
                    left.ServerRid.ToHex(),
                    right.ServerRid.ToHex()
                );
                return rid != 0
                    ? rid
                    : left.LifecycleGeneration.CompareTo(right.LifecycleGeneration);
            }
        );
        var location = ZLinkLocationStoreHealth.ProjectSnapshot(_storeHealth?.GetSnapshot());
        var role =
            state.HasClient && state.HasServer
                ? Zlink.Framework.Contracts.Configuration.ZLinkClientServerRole.ClientAndServer
            : state.HasClient ? Zlink.Framework.Contracts.Configuration.ZLinkClientServerRole.Client
            : Zlink.Framework.Contracts.Configuration.ZLinkClientServerRole.Server;
        var readyCount = servers.Count(static entry => entry.Ready);
        ulong sequence;
        lock (_gate)
            sequence = _statuses.GetValueOrDefault(channel)?.Sequence ?? 0;
        return new ZLinkClientServerChannelSnapshot(
            channelName,
            role,
            IsReady: _hostLifecycle.State == ZLinkFrameworkRuntimeState.Serving
                && _runtime.IsStarted
                && readyCount > 0,
            readyCount,
            state.Client?.ConnectionIntentCount ?? 0,
            state.Client?.PendingRequestCount ?? 0,
            sequence,
            DateTimeOffset.UtcNow,
            servers,
            location
        );
    }

    public ZLinkClientServerStatus GetStatus(string channelName)
    {
        lock (_gate)
        {
            var channel = ZLinkChannelName.FromBoundary(channelName, nameof(channelName));
            if (_statuses.GetValueOrDefault(channel) is { } terminal && terminal.State.IsTerminal())
                return terminal;
            _ = GetOrCreateHubOnGate(channel);
            var snapshot = SnapshotInternal(channelName);
            return PublishStatusOnGate(Project(snapshot, _hostLifecycle.State, _runtime.IsStarted));
        }
    }

    private MonitorHub GetOrCreateHubOnGate(ZLinkChannelName channel)
    {
        if (_monitorHubs.TryGetValue(channel, out var hub))
            return hub;
        hub = new MonitorHub(this, channel);
        _monitorHubs.Add(channel, hub);
        hub.Start();
        if (_runtime.ClientServerMonitoringState(channel.Value).Server is { } server)
            server.SnapshotChanged += hub.SignalServer;
        return hub;
    }

    internal async Task StopAsync()
    {
        MonitorHub[] hubs;
        lock (_gate)
        {
            hubs = _monitorHubs.Values.ToArray();
            foreach (var channel in _monitorHubs.Keys)
            {
                if (_runtime.ClientServerMonitoringState(channel.Value).Server is { } server)
                    server.SnapshotChanged -= _monitorHubs[channel].SignalServer;
                var current = _statuses.GetValueOrDefault(channel);
                if (current is not null)
                    PublishStatusOnGate(
                        current with
                        {
                            State = ZLinkTopologyState.Stopped,
                            IsReady = false,
                        }
                    );
            }
            _monitorHubs.Clear();
        }
        foreach (var hub in hubs)
            await hub.StopAsync().ConfigureAwait(false);
    }

    public ValueTask DisposeAsync() => new(StopAsync());

    private static ZLinkClientServerStatus Project(
        ZLinkClientServerChannelSnapshot snapshot,
        ZLinkFrameworkRuntimeState hostState,
        bool runtimeStarted
    )
    {
        var targets = snapshot
            .Servers.Select(static server =>
            {
                var (state, reason) = MapPeer(server.State);
                return new ZLinkClientServerTargetStatus(
                    server.ServerRid,
                    server.Weight,
                    state,
                    reason
                );
            })
            .ToArray();
        var topologyState = snapshot.IsReady
            ? ZLinkTopologyState.Ready
            : HostTopologyState(hostState, runtimeStarted);
        return new ZLinkClientServerStatus(
            snapshot.ChannelName,
            snapshot.LocalRole,
            topologyState,
            snapshot.IsReady,
            snapshot.ReadyServerCount,
            targets,
            snapshot.Sequence,
            snapshot.ObservedAt
        );
    }

    public async IAsyncEnumerable<ZLinkObservedStatus<ZLinkClientServerStatus>> ObserveAsync(
        string channelName,
        [EnumeratorCancellation] CancellationToken cancellationToken = default
    )
    {
        var channel = ZLinkChannelName.FromBoundary(channelName, nameof(channelName));
        ZLinkObservationQueue<ZLinkClientServerStatus> observer;
        MonitorHub? hub;
        lock (_gate)
        {
            var initial = GetStatus(channelName);
            observer = new ZLinkObservationQueue<ZLinkClientServerStatus>(
                initial,
                initial.State.IsTerminal(),
                static status => status.ChannelName,
                eventName: "client_server"
            );
            _monitorHubs.TryGetValue(channel, out hub);
            hub?.Add(observer);
        }
        try
        {
            await foreach (
                var item in observer.ReadAllAsync(cancellationToken).ConfigureAwait(false)
            )
                yield return item;
        }
        finally
        {
            observer.Complete();
            lock (_gate)
                hub?.Remove(observer);
        }
    }

    private sealed class MonitorHub
    {
        private readonly ZLinkClientServerRuntimeService _owner;
        private readonly ZLinkChannelName _channel;
        private readonly StateChangeSignal _signal = new();
        private readonly CancellationTokenSource _stop = new();
        private readonly List<ZLinkObservationQueue<ZLinkClientServerStatus>> _observers = [];
        private readonly Action _signalClient;
        private readonly Action<ZLinkFrameworkRuntimeState> _signalHost;
        private ZLinkClientServerClientRuntime? _client;
        private Task _producer = Task.CompletedTask;

        internal MonitorHub(ZLinkClientServerRuntimeService owner, ZLinkChannelName channel)
        {
            _owner = owner;
            _channel = channel;
            _signalClient = _signal.Signal;
            _signalHost = _ => _signal.Signal();
        }

        // Subscription changes and publication share the owner's gate.
        internal void Add(ZLinkObservationQueue<ZLinkClientServerStatus> observer) =>
            _observers.Add(observer);

        internal void Remove(ZLinkObservationQueue<ZLinkClientServerStatus> observer) =>
            _observers.Remove(observer);

        internal void Publish(ZLinkClientServerStatus status)
        {
            foreach (var observer in _observers)
                observer.Publish(status, status.State.IsTerminal());
        }

        internal void SignalServer(ZLinkClientServerServerIdentity.Snapshot snapshot) =>
            _signal.Signal();

        internal void Start()
        {
            _owner._hostLifecycle.Changed += _signalHost;
            if (_owner._storeHealth is not null)
                _owner._storeHealth.Changed += _signalClient;
            _signal.Signal();
            _producer = Task.Run(ProduceAsync);
        }

        internal async ValueTask StopAsync()
        {
            _owner._hostLifecycle.Changed -= _signalHost;
            if (_owner._storeHealth is not null)
                _owner._storeHealth.Changed -= _signalClient;
            _stop.Cancel();
            _signal.Complete();
            try
            {
                await _producer.ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (_stop.IsCancellationRequested) { }
            finally
            {
                if (_client is not null)
                    _client.StateChanged -= _signalClient;
                _stop.Dispose();
                _signal.Dispose();
            }
        }

        private async Task ProduceAsync()
        {
            while (await _signal.WaitToReadAsync(_stop.Token).ConfigureAwait(false))
            {
                while (_signal.TryRead()) { }
                _ = _owner.GetStatus(_channel.Value);
            }
        }

        internal void RefreshClientSubscription(ZLinkClientServerClientRuntime? client)
        {
            if (ReferenceEquals(client, _client))
                return;
            if (_client is not null)
                _client.StateChanged -= _signalClient;
            _client = client;
            if (_client is not null)
                _client.StateChanged += _signalClient;
        }
    }

    private sealed class StateChangeSignal : IDisposable
    {
        private readonly Channel<bool> _channel = Channel.CreateBounded<bool>(
            new BoundedChannelOptions(1)
            {
                SingleReader = true,
                SingleWriter = false,
                AllowSynchronousContinuations = false,
                FullMode = BoundedChannelFullMode.DropWrite,
            }
        );

        internal void Signal() => _channel.Writer.TryWrite(true);

        internal ValueTask<bool> WaitToReadAsync(CancellationToken cancellationToken) =>
            _channel.Reader.WaitToReadAsync(cancellationToken);

        internal bool TryRead() => _channel.Reader.TryRead(out _);

        internal void Complete() => _channel.Writer.TryComplete();

        public void Dispose()
        {
            Complete();
        }
    }

    private ZLinkClientServerStatus PublishStatusOnGate(ZLinkClientServerStatus status)
    {
        var channel = ZLinkChannelName.FromBoundary(status.ChannelName, nameof(status.ChannelName));
        var previous = _statuses.GetValueOrDefault(channel);
        if (
            previous is not null
            && (previous.State.IsTerminal() || SamePublicStatus(previous, status))
        )
            return previous;
        status = status with { Sequence = checked((previous?.Sequence ?? 0) + 1) };
        _statuses[channel] = status;
        if (_monitorHubs.TryGetValue(channel, out var hub))
            hub.Publish(status);
        return status;
    }

    private static bool SamePublicStatus(
        ZLinkClientServerStatus left,
        ZLinkClientServerStatus right
    ) =>
        left.ChannelName == right.ChannelName
        && left.LocalRole == right.LocalRole
        && left.State == right.State
        && left.IsReady == right.IsReady
        && left.ReadyTargetCount == right.ReadyTargetCount
        && left.Targets.SequenceEqual(right.Targets);

    private static ZLinkClientServerServerState Map(ZLinkFrameworkRuntimeState state) =>
        state switch
        {
            ZLinkFrameworkRuntimeState.Serving => ZLinkClientServerServerState.Ready,
            ZLinkFrameworkRuntimeState.Draining
            or ZLinkFrameworkRuntimeState.Relocating
            or ZLinkFrameworkRuntimeState.Relocated => ZLinkClientServerServerState.Draining,
            ZLinkFrameworkRuntimeState.Stopped => ZLinkClientServerServerState.Disconnected,
            ZLinkFrameworkRuntimeState.Error => ZLinkClientServerServerState.Rejected,
            _ => ZLinkClientServerServerState.Configured,
        };

    private static (ZLinkPeerState State, ZLinkTopologyReason? Reason) MapPeer(
        ZLinkClientServerServerState state
    ) =>
        state switch
        {
            ZLinkClientServerServerState.Ready => (ZLinkPeerState.Ready, null),
            ZLinkClientServerServerState.Draining => (
                ZLinkPeerState.Draining,
                ZLinkTopologyReason.Draining
            ),
            ZLinkClientServerServerState.Configured or ZLinkClientServerServerState.Connecting => (
                ZLinkPeerState.Connecting,
                ZLinkTopologyReason.NoReadyTarget
            ),
            _ => (ZLinkPeerState.NotConnected, ZLinkTopologyReason.NoReadyTarget),
        };

    private static ZLinkTopologyState HostTopologyState(
        ZLinkFrameworkRuntimeState state,
        bool runtimeStarted
    ) =>
        state switch
        {
            ZLinkFrameworkRuntimeState.Preparing => ZLinkTopologyState.Starting,
            ZLinkFrameworkRuntimeState.Relocating
            or ZLinkFrameworkRuntimeState.Relocated
            or ZLinkFrameworkRuntimeState.Draining => ZLinkTopologyState.Stopping,
            ZLinkFrameworkRuntimeState.Stopped => ZLinkTopologyState.Stopped,
            ZLinkFrameworkRuntimeState.Error => ZLinkTopologyState.Failed,
            _ => runtimeStarted ? ZLinkTopologyState.Degraded : ZLinkTopologyState.Starting,
        };

    private static T AwaitStateLane<T>(ValueTask<T> operation) =>
        operation.GetAwaiter().GetResult();
}
