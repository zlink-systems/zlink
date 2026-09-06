using System.Runtime.CompilerServices;
using System.Threading.Channels;

using Zlink.Framework.Runtime.Diagnostics;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.Runtime.Channels;

internal sealed class ZLinkClientServerRuntimeService(
    ZLinkFrameworkRuntime runtime,
    ZLinkFrameworkHostLifecycleState hostLifecycle,
    ZLinkLocationStoreHealth? storeHealth) : IZLinkClientServerRuntime
{
    private readonly object _gate = new();
    private readonly ZLinkFrameworkRuntime _runtime = runtime;
    private readonly ZLinkFrameworkHostLifecycleState _hostLifecycle = hostLifecycle;
    private readonly ZLinkLocationStoreHealth? _storeHealth = storeHealth;
    private readonly Dictionary<ZLinkChannelName, SequenceState> _sequences = [];
    private readonly Dictionary<ZLinkChannelName, MonitorHub> _monitorHubs = [];

    private ZLinkClientServerChannelSnapshot SnapshotInternal(string channelName)
    {
        ArgumentException.ThrowIfNullOrEmpty(channelName);
        var channel = ZLinkChannelName.FromBoundary(channelName, nameof(channelName));
        var state = _runtime.ClientServerMonitoringState(channelName);
        var servers = state.Client?.SnapshotConnections()
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
                entry.LastFailure))
            .ToList() ?? [];

        if (state.Server is { } localServer)
        {
            var local = AwaitStateLane(localServer.ReadAsync());
            var existing = servers.FindIndex(
                entry => entry.ServerRid == localServer.ServerRid
                         && entry.LifecycleGeneration
                         == localServer.LifecycleGeneration);
            if (existing < 0)
                servers.Add(new ZLinkClientServerServerSnapshot(
                    localServer.ServerRid,
                    localServer.LifecycleGeneration,
                    local.Revision,
                    local.AdvertisedEndpoint,
                    local.Weight,
                    local.State == ZLinkFrameworkRuntimeState.Serving
                    && local.Weight > 0,
                    Map(local.State),
                    "manual",
                    LastFailure: null));
        }

        servers.Sort(static (left, right) =>
        {
            var rid = StringComparer.Ordinal.Compare(
                left.ServerRid.ToHex(),
                right.ServerRid.ToHex());
            return rid != 0
                ? rid
                : left.LifecycleGeneration.CompareTo(
                    right.LifecycleGeneration);
        });
        var location = LocationSnapshot();
        var role = state.HasClient && state.HasServer
            ? Zlink.Framework.Contracts.Configuration.ZLinkClientServerRole
                .ClientAndServer
            : state.HasClient
                ? Zlink.Framework.Contracts.Configuration.ZLinkClientServerRole
                    .Client
                : Zlink.Framework.Contracts.Configuration.ZLinkClientServerRole
                    .Server;
        var readyCount = servers.Count(static entry => entry.Ready);
        var fingerprint = new Fingerprint(
            _hostLifecycle.State,
            role,
            state.Client?.ConnectionIntentCount ?? 0,
            state.Client?.PendingRequestCount ?? 0,
            location,
            string.Join(
                "\n",
                servers.Select(static entry =>
                    $"{entry.ServerRid.ToHex()}|{entry.LifecycleGeneration}|"
                    + $"{entry.DescriptorRevision}|{entry.Endpoint}|"
                    + $"{entry.Weight}|{entry.Ready}|{entry.State}|"
                    + $"{entry.DescriptorSource}|{entry.LastFailure}")));
        var sequence = Sequence(channel, fingerprint);
        return new ZLinkClientServerChannelSnapshot(
            channelName,
            role,
            IsReady: fingerprint.HostState == ZLinkFrameworkRuntimeState.Serving
                     && _runtime.IsStarted
                     && readyCount > 0,
            readyCount,
            fingerprint.ConnectionIntentCount,
            fingerprint.PendingRequestCount,
            sequence,
            DateTimeOffset.UtcNow,
            servers,
            location);
    }

    public ZLinkClientServerStatus GetStatus(string channelName)
    {
        var snapshot = SnapshotInternal(channelName);
        return Project(
            snapshot,
            _hostLifecycle.State,
            _runtime.IsStarted);
    }

    private static ZLinkClientServerStatus Project(
        ZLinkClientServerChannelSnapshot snapshot,
        ZLinkFrameworkRuntimeState hostState,
        bool runtimeStarted)
    {
        var targets = snapshot.Servers
            .Select(static server => new ZLinkClientServerTargetStatus(
                server.ServerRid,
                server.Weight,
                MapPeerState(server.State),
                MapUnavailableReason(server.State)))
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
            snapshot.ObservedAt);
    }

    public async IAsyncEnumerable<ZLinkObservedStatus<ZLinkClientServerStatus>> ObserveAsync(
        string channelName,
        [EnumeratorCancellation] CancellationToken cancellationToken = default)
    {
        var channel = ZLinkChannelName.FromBoundary(
            channelName,
            nameof(channelName));
        var observer = new ZLinkObservationQueue<RetainedObservation>(
            static item => item.SourceKey,
            eventName: "client_server");
        MonitorHub hub;
        lock (_gate)
        {
            _ = SnapshotInternal(channelName);
            if (!_monitorHubs.TryGetValue(channel, out hub!))
            {
                hub = new MonitorHub(this, channel);
                _monitorHubs.Add(channel, hub);
                hub.Add(observer);
                hub.Start();
            }
            else
            {
                hub.Add(observer);
            }
        }
        try
        {
            await foreach (var item in observer.ReadAllAsync(cancellationToken)
                               .ConfigureAwait(false))
                yield return new ZLinkObservedStatus<ZLinkClientServerStatus>(
                    item.Status.Status,
                    item.Loss);
        }
        finally
        {
            observer.Complete();
            MonitorHub? stopped = null;
            lock (_gate)
            {
                hub.Remove(observer);
                if (hub.IsEmpty
                    && _monitorHubs.TryGetValue(channel, out var current)
                    && ReferenceEquals(current, hub))
                {
                    _monitorHubs.Remove(channel);
                    stopped = hub;
                }
            }
            if (stopped is not null)
                await stopped.StopAsync().ConfigureAwait(false);
        }
    }

    private sealed class MonitorHub
    {
        private readonly object _gate = new();
        private readonly ZLinkClientServerRuntimeService _owner;
        private readonly ZLinkChannelName _channel;
        private readonly StateChangeSignal _signal = new();
        private readonly CancellationTokenSource _stop = new();
        private readonly List<ZLinkObservationQueue<RetainedObservation>>
            _observers = [];
        private readonly Action _signalClient;
        private readonly Action<ZLinkFrameworkRuntimeState> _signalHost;
        private ZLinkClientServerChannelSnapshot _previous;
        private ZLinkClientServerClientRuntime? _client;
        private Task _producer = Task.CompletedTask;

        internal MonitorHub(
            ZLinkClientServerRuntimeService owner,
            ZLinkChannelName channel)
        {
            _owner = owner;
            _channel = channel;
            _previous = owner.SnapshotInternal(channel.Value);
            _signalClient = _signal.Signal;
            _signalHost = _ => _signal.Signal();
        }

        internal bool IsEmpty
        {
            get
            {
                lock (_gate) return _observers.Count == 0;
            }
        }

        internal void Add(ZLinkObservationQueue<RetainedObservation> observer)
        {
            lock (_gate) _observers.Add(observer);
        }

        internal void Remove(ZLinkObservationQueue<RetainedObservation> observer)
        {
            lock (_gate) _observers.Remove(observer);
        }

        internal void Start()
        {
            RefreshClientSubscription();
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
            catch (OperationCanceledException)
                when (_stop.IsCancellationRequested)
            {
            }
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
            while (await _signal.WaitToReadAsync(_stop.Token)
                       .ConfigureAwait(false))
            {
                while (_signal.TryRead())
                {
                }
                RefreshClientSubscription();
                var current = _owner.SnapshotInternal(_channel.Value);
                if (current.Sequence == _previous.Sequence) continue;
                var changes = Changes(_previous, current).ToArray();
                _previous = current;
                var hostState = _owner._hostLifecycle.State;
                var status = Project(
                    current,
                    hostState,
                    _owner._runtime.IsStarted);
                var hostTerminal = hostState is
                    ZLinkFrameworkRuntimeState.Stopped
                    or ZLinkFrameworkRuntimeState.Error;
                lock (_gate)
                    foreach (var change in changes)
                        foreach (var observer in _observers)
                            observer.Publish(
                                new RetainedObservation(
                                    change.SourceKey,
                                    status),
                                change.IsTerminal || hostTerminal);
            }
        }

        private void RefreshClientSubscription()
        {
            var client = _owner._runtime
                .ClientServerMonitoringState(_channel.Value)
                .Client;
            if (ReferenceEquals(client, _client)) return;
            if (_client is not null)
                _client.StateChanged -= _signalClient;
            _client = client;
            if (_client is not null)
                _client.StateChanged += _signalClient;
        }
    }

    private sealed class StateChangeSignal : IDisposable
    {
        private readonly Channel<bool> _channel =
            Channel.CreateBounded<bool>(new BoundedChannelOptions(1)
            {
                SingleReader = true,
                SingleWriter = false,
                AllowSynchronousContinuations = false,
                FullMode = BoundedChannelFullMode.DropWrite
            });

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

    internal static IEnumerable<ZLinkClientServerRuntimeEvent> Changes(
        ZLinkClientServerChannelSnapshot previous,
        ZLinkClientServerChannelSnapshot current)
    {
        var prior = previous.Servers.ToDictionary(
            static entry =>
                $"{entry.ServerRid.ToHex()}:{entry.LifecycleGeneration}",
            StringComparer.Ordinal);
        var changed = false;
        foreach (var server in current.Servers)
        {
            var key =
                $"{server.ServerRid.ToHex()}:{server.LifecycleGeneration}";
            if (prior.Remove(key, out var old) && old == server) continue;
            changed = true;
            yield return Event(current, server, server.LastFailure);
        }
        foreach (var removed in prior.Values)
        {
            changed = true;
            yield return Event(
                current,
                removed with
                {
                    Ready = false,
                    State = ZLinkClientServerServerState.Disconnected
                },
                "removed",
                terminal: true);
        }
        if (!changed)
            yield return new ZLinkClientServerRuntimeEvent(
                "zlink.runtime.client_server.state_changed",
                current.Sequence,
                current.ObservedAt,
                current.ChannelName,
                ServerRid: null,
                LifecycleGeneration: null,
                DescriptorRevision: null,
                Weight: null,
                Ready: current.IsReady,
                State: null,
                Reason: null);
    }

    private static ZLinkClientServerRuntimeEvent Event(
        ZLinkClientServerChannelSnapshot current,
        ZLinkClientServerServerSnapshot server,
        string? reason,
        bool terminal = false) =>
        new(
            "zlink.runtime.client_server.server_changed",
            current.Sequence,
            current.ObservedAt,
            current.ChannelName,
            server.ServerRid,
            server.LifecycleGeneration,
            server.DescriptorRevision,
            server.Weight,
            server.Ready,
            server.State,
            reason,
            terminal);

    private sealed record RetainedObservation(
        string SourceKey,
        ZLinkClientServerStatus Status);

    private ulong Sequence(ZLinkChannelName channelName, Fingerprint fingerprint)
    {
        lock (_gate)
        {
            if (!_sequences.TryGetValue(channelName, out var state))
            {
                _sequences[channelName] = new SequenceState(1, fingerprint);
                return 1;
            }
            if (state.Fingerprint == fingerprint) return state.Sequence;
            var next = checked(state.Sequence + 1);
            _sequences[channelName] = new SequenceState(next, fingerprint);
            return next;
        }
    }

    private ZLinkLocationRuntimeSnapshot LocationSnapshot()
    {
        if (_storeHealth is null)
            return new ZLinkLocationRuntimeSnapshot(
                "not_configured",
                null,
                null);
        var snapshot = _storeHealth.GetSnapshot();
        return new ZLinkLocationRuntimeSnapshot(
            snapshot.Healthy ? "ready" : "degraded",
            snapshot.LastSuccessAt,
            snapshot.LastFailureAt);
    }

    private static ZLinkClientServerServerState Map(
        ZLinkFrameworkRuntimeState state) =>
        state switch
        {
            ZLinkFrameworkRuntimeState.Serving =>
                ZLinkClientServerServerState.Ready,
            ZLinkFrameworkRuntimeState.Draining or
                ZLinkFrameworkRuntimeState.Relocating =>
                ZLinkClientServerServerState.Draining,
            ZLinkFrameworkRuntimeState.Stopped =>
                ZLinkClientServerServerState.Disconnected,
            ZLinkFrameworkRuntimeState.Error =>
                ZLinkClientServerServerState.Rejected,
            _ => ZLinkClientServerServerState.Configured
        };

    private static ZLinkPeerState MapPeerState(
        ZLinkClientServerServerState state) =>
        state switch
        {
            ZLinkClientServerServerState.Ready => ZLinkPeerState.Ready,
            ZLinkClientServerServerState.Draining => ZLinkPeerState.Draining,
            ZLinkClientServerServerState.Configured
                or ZLinkClientServerServerState.Connecting =>
                ZLinkPeerState.Connecting,
            _ => ZLinkPeerState.NotConnected
        };

    private static ZLinkTopologyReason? MapUnavailableReason(
        ZLinkClientServerServerState state) =>
        MapPeerState(state) switch
        {
            ZLinkPeerState.Ready => null,
            ZLinkPeerState.Draining => ZLinkTopologyReason.Draining,
            ZLinkPeerState.Connecting => ZLinkTopologyReason.NoReadyTarget,
            _ => ZLinkTopologyReason.InternalFailure
        };

    private static ZLinkTopologyState HostTopologyState(
        ZLinkFrameworkRuntimeState state,
        bool runtimeStarted) =>
        state switch
        {
            ZLinkFrameworkRuntimeState.Preparing => ZLinkTopologyState.Starting,
            ZLinkFrameworkRuntimeState.Relocating
                or ZLinkFrameworkRuntimeState.Relocated
                or ZLinkFrameworkRuntimeState.Draining =>
                ZLinkTopologyState.Stopping,
            ZLinkFrameworkRuntimeState.Stopped =>
                ZLinkTopologyState.Stopped,
            ZLinkFrameworkRuntimeState.Error => ZLinkTopologyState.Failed,
            _ => runtimeStarted
                ? ZLinkTopologyState.Degraded
                : ZLinkTopologyState.Starting
        };

    private sealed record Fingerprint(
        ZLinkFrameworkRuntimeState HostState,
        Zlink.Framework.Contracts.Configuration.ZLinkClientServerRole Role,
        int ConnectionIntentCount,
        int PendingRequestCount,
        ZLinkLocationRuntimeSnapshot Location,
        string ServerFingerprint);

    private sealed record SequenceState(
        ulong Sequence,
        Fingerprint Fingerprint);
    private static T AwaitStateLane<T>(ValueTask<T> operation) =>
        operation.GetAwaiter().GetResult();
}
