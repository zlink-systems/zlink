using System.Diagnostics;
using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.Runtime.Channels;

internal sealed class ZLinkClientServerServerIdentity(
    string channelName,
    RoutingId serverRid,
    ulong lifecycleGeneration,
    string securityIdentity,
    int weight,
    uint normalizedEffectiveMaxMessageBytes,
    string advertisedEndpoint)
{
    private static readonly TimeSpan ProbeInterval = TimeSpan.FromSeconds(5);
    private static readonly TimeSpan PeerDeadline = TimeSpan.FromSeconds(15);
    private readonly ZLinkStateLane _lane = new();
    private readonly Dictionary<RoutingId, Peer> _peers = [];
    private ulong _revision = 1;
    private ulong _nextProbeId = 1;
    private int _weight = weight;
    private int _servingWeight = weight;
    private ZLinkFrameworkRuntimeState _state =
        ZLinkFrameworkRuntimeState.Serving;
    private string _advertisedEndpoint = advertisedEndpoint;
    private IRouterSocket? _router;
    private long _livenessAckCount;
    private long _livenessProbeCount;
    private long _receivedLivenessProbeCount;
    internal event Action<Snapshot>? SnapshotChanged;

    internal ZLinkChannelName ChannelName { get; } =
        ZLinkChannelName.FromBoundary(channelName, nameof(channelName));
    internal RoutingId ServerRid { get; } = serverRid;
    internal ulong LifecycleGeneration { get; } = lifecycleGeneration;
    internal string SecurityIdentity { get; } = securityIdentity;
    internal uint NormalizedEffectiveMaxMessageBytes { get; } =
        normalizedEffectiveMaxMessageBytes;
    internal long LivenessAckCount =>
        Interlocked.Read(ref _livenessAckCount);
    internal long LivenessProbeCount =>
        Interlocked.Read(ref _livenessProbeCount);
    internal long ReceivedLivenessProbeCount =>
        Interlocked.Read(ref _receivedLivenessProbeCount);
    internal ValueTask<int> GetAdmittedPeerCountAsync() =>
        _lane.RunAsync(() => _peers.Count);

    internal ValueTask<Snapshot> ReadAsync() =>
        _lane.RunAsync(ReadOnLane);

    internal ValueTask SetAdvertisedEndpointAsync(string endpoint)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(endpoint);
        return _lane.RunAsync(() => { _advertisedEndpoint = endpoint; });
    }

    internal async ValueTask<Snapshot> MarkDrainingAsync()
    {
        var snapshot = await _lane.RunAsync(MarkDrainingOnLane).ConfigureAwait(false);
        PushUpdate(snapshot);
        SnapshotChanged?.Invoke(snapshot);
        return snapshot;
    }

    internal ValueTask<Snapshot> MarkRetiringAsync() => SetLifecycleStateAsync(
        ZLinkFrameworkRuntimeState.Relocating);

    internal ValueTask<Snapshot> MarkServingAsync() => SetLifecycleStateAsync(
        ZLinkFrameworkRuntimeState.Serving);

    private async ValueTask<Snapshot> SetLifecycleStateAsync(ZLinkFrameworkRuntimeState state)
    {
        var snapshot = await _lane.RunAsync(() => SetLifecycleStateOnLane(state))
            .ConfigureAwait(false);
        PushUpdate(snapshot);
        SnapshotChanged?.Invoke(snapshot);
        return snapshot;
    }

    internal async ValueTask<Snapshot> SetWeightAsync(int weight)
    {
        ZLinkSocketConfig.ValidatePeerWeight(weight);
        var snapshot = await _lane.RunAsync(() => SetWeightOnLane(weight))
            .ConfigureAwait(false);
        PushUpdate(snapshot);
        SnapshotChanged?.Invoke(snapshot);
        return snapshot;
    }

    internal ValueTask AttachRouterAsync(IRouterSocket router) =>
        _lane.RunAsync(() => { _router = router; });

    internal ValueTask DetachRouterAsync(IRouterSocket router) =>
        _lane.RunAsync(() =>
        {
            if (ReferenceEquals(_router, router))
                _router = null;
            _peers.Clear();
        });

    internal ValueTask AdmitPeerAsync(
        RoutingId routingId,
        uint normalizedEffectiveMaxMessageBytes)
    {
        var now = Stopwatch.GetElapsedTime(0);
        return _lane.RunAsync(() =>
        {
            _peers[routingId] = new Peer(
                routingId,
                normalizedEffectiveMaxMessageBytes,
                now + ProbeInterval,
                now + PeerDeadline);
        });
    }

    internal ValueTask<(bool Found, uint MaximumMessageBytes)>
        GetAdmittedMaximumMessageBytesAsync(RoutingId routingId) =>
        _lane.RunAsync(() => _peers.TryGetValue(routingId, out var peer)
            ? (true, peer.NormalizedEffectiveMaxMessageBytes)
            : (false, 0u));

    internal ValueTask AcceptLivenessAckAsync(
        RoutingId routingId,
        ulong probeId) =>
        _lane.RunAsync(() =>
        {
            if (!_peers.TryGetValue(routingId, out var peer)
                || peer.OutstandingProbeId != probeId)
                return;
            peer.OutstandingProbeId = null;
            peer.Deadline = Stopwatch.GetElapsedTime(0) + PeerDeadline;
            Interlocked.Increment(ref _livenessAckCount);
        });

    internal void RecordLivenessProbe(RoutingId routingId)
    {
        _ = routingId;
        Interlocked.Increment(ref _receivedLivenessProbeCount);
    }

    internal async ValueTask TickLivenessAsync(
        IRouterSocket router,
        CancellationToken cancellationToken)
    {
        var now = Stopwatch.GetElapsedTime(0);
        var (probes, expired) = await _lane.RunAsync(() => PrepareLivenessTick(now))
            .ConfigureAwait(false);
        foreach (var routingId in expired)
            try
            {
                router.DisconnectRid(routingId);
            }
            catch
            {
        }
        foreach (var probe in probes)
            if (await SendOwnedAsync(
                    router,
                    probe.RoutingId,
                    ZLinkClientServerControlProtocol.EncodeLivenessProbe(
                        probe.ProbeId),
                    cancellationToken).ConfigureAwait(false))
                Interlocked.Increment(ref _livenessProbeCount);
    }

    private void PushUpdate(Snapshot snapshot)
    {
        var (router, peers) = AwaitStateLane(_lane.RunAsync(GetPushUpdateTargets));
        if (router is null)
            return;
        foreach (var peer in peers)
            _ = SendOwnedAsync(
                router,
                peer.RoutingId,
                ZLinkClientServerControlProtocol.EncodeUpdate(
                    ToAdmission(snapshot) with
                    {
                        NormalizedEffectiveMaxMessageBytes =
                            peer.MaximumMessageBytes
                    }),
                CancellationToken.None);
    }

    private Snapshot ReadOnLane() => new(
        _revision,
        _weight,
        _state,
        _advertisedEndpoint);

    private Snapshot MarkDrainingOnLane()
    {
        _revision++;
        _weight = 0;
        _state = ZLinkFrameworkRuntimeState.Draining;
        return ReadOnLane();
    }

    private Snapshot SetLifecycleStateOnLane(ZLinkFrameworkRuntimeState state)
    {
        _revision++;
        _state = state;
        _weight = state == ZLinkFrameworkRuntimeState.Serving
            ? _servingWeight
            : 0;
        return ReadOnLane();
    }

    private Snapshot SetWeightOnLane(int weight)
    {
        if (_state != ZLinkFrameworkRuntimeState.Serving)
            throw new ZLinkConfigurationException(
                $"ClientServer Server '{ChannelName}' is not serving.");
        _revision = checked(_revision + 1);
        _weight = weight;
        _servingWeight = weight;
        return ReadOnLane();
    }

    private ((RoutingId RoutingId, ulong ProbeId)[] Probes, RoutingId[] Expired)
        PrepareLivenessTick(TimeSpan now)
    {
        List<(RoutingId RoutingId, ulong ProbeId)> probes = [];
        List<RoutingId> expired = [];
        foreach (var (key, peer) in _peers.ToArray())
        {
            if (now >= peer.Deadline)
            {
                _peers.Remove(key);
                expired.Add(peer.RoutingId);
                continue;
            }
            if (now < peer.NextProbe)
                continue;
            peer.NextProbe = now + ProbeInterval;
            peer.OutstandingProbeId ??= AllocateProbeId();
            probes.Add((peer.RoutingId, peer.OutstandingProbeId.Value));
        }
        return ([.. probes], [.. expired]);
    }

    private (IRouterSocket? Router,
        (RoutingId RoutingId, uint MaximumMessageBytes)[] Peers) GetPushUpdateTargets() =>
        (_router, _peers.Values
            .Select(static peer => (
                peer.RoutingId,
                peer.NormalizedEffectiveMaxMessageBytes))
            .ToArray());

    internal ZLinkClientServerControlProtocol.Admission ToAdmission(
        Snapshot snapshot) =>
        new(
            ChannelName.Value,
            ServerRid,
            LifecycleGeneration,
            snapshot.Revision,
            snapshot.Weight,
            snapshot.State,
            SecurityIdentity,
            NormalizedEffectiveMaxMessageBytes,
            snapshot.AdvertisedEndpoint);

    private ulong AllocateProbeId()
    {
        var result = _nextProbeId;
        _nextProbeId = result == long.MaxValue ? 1 : result + 1;
        return result;
    }

    private static T AwaitStateLane<T>(ValueTask<T> operation) =>
        operation.GetAwaiter().GetResult();

    private static async ValueTask<bool> SendOwnedAsync(
        IRouterSocket router,
        RoutingId routingId,
        Message message,
        CancellationToken cancellationToken)
    {
        try
        {
            await router.Send(routingId)
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


    internal readonly record struct Snapshot(
        ulong Revision,
        int Weight,
        ZLinkFrameworkRuntimeState State,
        string AdvertisedEndpoint);

    private sealed class Peer(
        RoutingId routingId,
        uint normalizedEffectiveMaxMessageBytes,
        TimeSpan nextProbe,
        TimeSpan deadline)
    {
        internal RoutingId RoutingId { get; } = routingId;
        internal uint NormalizedEffectiveMaxMessageBytes { get; } =
            normalizedEffectiveMaxMessageBytes;
        internal TimeSpan NextProbe { get; set; } = nextProbe;
        internal TimeSpan Deadline { get; set; } = deadline;
        internal ulong? OutstandingProbeId { get; set; }
    }
}
