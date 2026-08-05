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
    private readonly Dictionary<string, SequenceState> _sequences =
        new(StringComparer.Ordinal);

    private ZLinkClientServerChannelSnapshot SnapshotInternal(string channelName)
    {
        ArgumentException.ThrowIfNullOrEmpty(channelName);
        var state = runtime.ClientServerMonitoringState(channelName);
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
            var local = localServer.Read();
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
            hostLifecycle.State,
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
        var sequence = Sequence(channelName, fingerprint);
        return new ZLinkClientServerChannelSnapshot(
            channelName,
            role,
            Selectable: state.HasClient
                        && runtime.IsStarted
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
        var targets = snapshot.Servers
            .Select(static server => new ZLinkClientServerTargetStatus(
                server.ServerRid,
                server.Weight,
                MapPeerState(server.State),
                MapUnavailableReason(server.State)))
            .ToArray();
        var hostState = hostLifecycle.State;
        var isReady = hostState == ZLinkFrameworkRuntimeState.Serving
                      && runtime.IsStarted
                      && snapshot.Selectable;
        var topologyState = isReady
            ? ZLinkTopologyState.Ready
            : HostTopologyState(hostState, runtime.IsStarted);
        return new ZLinkClientServerStatus(
            snapshot.ChannelName,
            snapshot.LocalRole,
            topologyState,
            isReady,
            targets.Count(static target =>
                target.Weight > 0
                && target.State == ZLinkPeerState.Ready),
            targets,
            snapshot.Sequence,
            snapshot.ObservedAt);
    }

    public async IAsyncEnumerable<ZLinkObservedStatus<ZLinkClientServerStatus>> ObserveAsync(
        string channelName,
        [EnumeratorCancellation] CancellationToken cancellationToken = default)
    {
        const int capacity = 1024;
        _ = SnapshotInternal(channelName);
        var observer = new ZLinkObservationQueue<ZLinkClientServerRuntimeEvent>(
            capacity,
            static item => item.Sequence,
            "client_server");
        using var stop =
            CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        using var signal = new StateChangeSignal();
        var client = runtime.ClientServerMonitoringState(channelName).Client;
        Action signalClient = signal.Signal;
        Action<ZLinkFrameworkRuntimeState> signalHost = _ => signal.Signal();
        if (client is not null)
            client.StateChanged += signalClient;
        hostLifecycle.Changed += signalHost;
        if (storeHealth is not null)
            storeHealth.Changed += signalClient;
        var producer = ProduceAsync(channelName, observer, signal, stop.Token);
        try
        {
            await foreach (var item in observer.ReadAllAsync(cancellationToken)
                               .ConfigureAwait(false))
                yield return new ZLinkObservedStatus<ZLinkClientServerStatus>(
                    GetStatus(channelName),
                    item.Loss);
        }
        finally
        {
            stop.Cancel();
            if (client is not null)
                client.StateChanged -= signalClient;
            hostLifecycle.Changed -= signalHost;
            if (storeHealth is not null)
                storeHealth.Changed -= signalClient;
            signal.Complete();
            try
            {
                await producer.ConfigureAwait(false);
            }
            catch (OperationCanceledException)
                when (stop.IsCancellationRequested)
            {
            }
            observer.Complete();
        }
    }

    private async Task ProduceAsync(
        string channelName,
        ZLinkObservationQueue<ZLinkClientServerRuntimeEvent> observer,
        StateChangeSignal signal,
        CancellationToken cancellationToken)
    {
        var previous = SnapshotInternal(channelName);
        while (await signal.WaitToReadAsync(cancellationToken)
                   .ConfigureAwait(false))
        {
            while (signal.TryRead())
            {
            }
            var current = SnapshotInternal(channelName);
            if (current.Sequence == previous.Sequence) continue;
            foreach (var change in Changes(previous, current))
                observer.Publish(
                    change,
                    hostLifecycle.State is ZLinkFrameworkRuntimeState.Stopped
                        or ZLinkFrameworkRuntimeState.Error);
            previous = current;
        }
    }

    private sealed class StateChangeSignal : IDisposable
    {
        private readonly Channel<bool> _channel =
            Channel.CreateBounded<bool>(new BoundedChannelOptions(1)
            {
                SingleReader = true,
                SingleWriter = false,
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

    private static IEnumerable<ZLinkClientServerRuntimeEvent> Changes(
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
                "removed");
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
                Ready: current.Selectable,
                State: null,
                Reason: null);
    }

    private static ZLinkClientServerRuntimeEvent Event(
        ZLinkClientServerChannelSnapshot current,
        ZLinkClientServerServerSnapshot server,
        string? reason) =>
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
            reason);

    private ulong Sequence(string channelName, Fingerprint fingerprint)
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
        if (storeHealth is null)
            return new ZLinkLocationRuntimeSnapshot(
                "not_configured",
                null,
                null);
        var snapshot = storeHealth.GetSnapshot();
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
}
