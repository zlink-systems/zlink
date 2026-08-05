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
    private readonly object _gate = new();
    private readonly Dictionary<string, Peer> _peers = new(StringComparer.Ordinal);
    private ulong _revision = 1;
    private ulong _nextProbeId = 1;
    private int _weight = weight;
    private int _servingWeight = weight;
    private ZLinkFrameworkRuntimeState _state =
        ZLinkFrameworkRuntimeState.Serving;
    private string _advertisedEndpoint = advertisedEndpoint;
    private IZLinkBackendRouterSocket? _router;
    private long _livenessAckCount;
    private long _livenessProbeCount;
    private long _receivedLivenessProbeCount;
    internal event Action<Snapshot>? SnapshotChanged;

    internal string ChannelName { get; } = channelName;
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
    internal int AdmittedPeerCount
    {
        get { lock (_gate) return _peers.Count; }
    }
    internal Snapshot Read()
    {
        lock (_gate)
            return new Snapshot(
                _revision,
                _weight,
                _state,
                _advertisedEndpoint);
    }

    internal void SetAdvertisedEndpoint(string endpoint)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(endpoint);
        lock (_gate)
            _advertisedEndpoint = endpoint;
    }

    internal Snapshot MarkDraining()
    {
        Snapshot snapshot;
        lock (_gate)
        {
            _revision++;
            _weight = 0;
            _state = ZLinkFrameworkRuntimeState.Draining;
            snapshot = new Snapshot(
                _revision,
                _weight,
                _state,
                _advertisedEndpoint);
        }
        PushUpdate(snapshot);
        SnapshotChanged?.Invoke(snapshot);
        return snapshot;
    }

    internal Snapshot MarkRetiring() => SetLifecycleState(
        ZLinkFrameworkRuntimeState.Relocating);

    internal Snapshot MarkServing() => SetLifecycleState(
        ZLinkFrameworkRuntimeState.Serving);

    private Snapshot SetLifecycleState(ZLinkFrameworkRuntimeState state)
    {
        Snapshot snapshot;
        lock (_gate)
        {
            _revision++;
            _state = state;
            _weight = state == ZLinkFrameworkRuntimeState.Serving
                ? _servingWeight
                : 0;
            snapshot = new Snapshot(
                _revision,
                _weight,
                _state,
                _advertisedEndpoint);
        }
        PushUpdate(snapshot);
        SnapshotChanged?.Invoke(snapshot);
        return snapshot;
    }

    internal Snapshot SetWeight(int weight)
    {
        ZLinkSocketConfig.ValidatePeerWeight(weight);
        Snapshot snapshot;
        lock (_gate)
        {
            if (_state != ZLinkFrameworkRuntimeState.Serving)
                throw new ZLinkConfigurationException(
                    $"ClientServer Server '{ChannelName}' is not serving.");
            _revision = checked(_revision + 1);
            _weight = weight;
            _servingWeight = weight;
            snapshot = new Snapshot(
                _revision,
                _weight,
                _state,
                _advertisedEndpoint);
        }
        PushUpdate(snapshot);
        SnapshotChanged?.Invoke(snapshot);
        return snapshot;
    }

    internal void AttachRouter(IZLinkBackendRouterSocket router)
    {
        lock (_gate) _router = router;
    }

    internal void DetachRouter(IZLinkBackendRouterSocket router)
    {
        lock (_gate)
        {
            if (ReferenceEquals(_router, router))
                _router = null;
            _peers.Clear();
        }
    }

    internal void AdmitPeer(RoutingId routingId)
    {
        var now = DateTimeOffset.UtcNow;
        lock (_gate)
            _peers[routingId.ToHex()] = new Peer(
                routingId,
                now + ProbeInterval,
                now + PeerDeadline);
    }

    internal void AcceptLivenessAck(
        RoutingId routingId,
        ulong probeId)
    {
        lock (_gate)
        {
            if (!_peers.TryGetValue(routingId.ToHex(), out var peer)
                || peer.OutstandingProbeId != probeId)
                return;
            peer.OutstandingProbeId = null;
            peer.Deadline = DateTimeOffset.UtcNow + PeerDeadline;
            Interlocked.Increment(ref _livenessAckCount);
        }
    }

    internal void RecordLivenessProbe(RoutingId routingId)
    {
        _ = routingId;
        Interlocked.Increment(ref _receivedLivenessProbeCount);
    }

    internal void TickLiveness(IZLinkBackendRouterSocket router)
    {
        List<(RoutingId RoutingId, ulong ProbeId)> probes = [];
        List<RoutingId> expired = [];
        var now = DateTimeOffset.UtcNow;
        lock (_gate)
        {
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
        }
        foreach (var routingId in expired)
            try
            {
                router.DisconnectPeer(routingId);
            }
            catch
            {
        }
        foreach (var probe in probes)
            if (TrySend(
                    router,
                    probe.RoutingId,
                    ZLinkClientServerControlProtocol.EncodeLivenessProbe(
                        probe.ProbeId)))
                Interlocked.Increment(ref _livenessProbeCount);
    }

    private void PushUpdate(Snapshot snapshot)
    {
        IZLinkBackendRouterSocket? router;
        RoutingId[] peers;
        lock (_gate)
        {
            router = _router;
            peers = _peers.Values
                .Select(static peer => peer.RoutingId)
                .ToArray();
        }
        if (router is null)
            return;
        foreach (var peer in peers)
            _ = TrySend(
                router,
                peer,
                ZLinkClientServerControlProtocol.EncodeUpdate(
                    ToAdmission(snapshot)));
    }

    internal ZLinkClientServerControlProtocol.Admission ToAdmission(
        Snapshot snapshot) =>
        new(
            ChannelName,
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

    private static bool TrySend(
        IZLinkBackendRouterSocket router,
        RoutingId routingId,
        Message message)
    {
        try
        {
            if (router.Send(routingId, message, SendFlags.DontWait))
                return true;
        }
        catch
        {
        }
        message.Dispose();
        return false;
    }


    internal readonly record struct Snapshot(
        ulong Revision,
        int Weight,
        ZLinkFrameworkRuntimeState State,
        string AdvertisedEndpoint);

    private sealed class Peer(
        RoutingId routingId,
        DateTimeOffset nextProbe,
        DateTimeOffset deadline)
    {
        internal RoutingId RoutingId { get; } = routingId;
        internal DateTimeOffset NextProbe { get; set; } = nextProbe;
        internal DateTimeOffset Deadline { get; set; } = deadline;
        internal ulong? OutstandingProbeId { get; set; }
    }
}
