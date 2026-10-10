using System.Diagnostics;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Messaging;
using Zlink.Framework.Runtime.Service;
using static Zlink.Framework.Runtime.Execution.ZLinkStateLaneWait;

namespace Zlink.Framework.Runtime.Channels;

internal sealed class ZLinkClientServerServerIdentity(
    string channelName,
    RoutingId serverRid,
    ulong lifecycleGeneration,
    string securityIdentity,
    int weight,
    uint normalizedEffectiveMaxMessageBytes,
    string advertisedEndpoint,
    IZLinkRuntimeFailureReporter errorSink
)
{
    private readonly ZLinkStateLane _lane = new();
    private readonly Dictionary<RoutingId, Peer> _peers = [];
    private ulong _revision = 1;
    private int _weight = weight;
    private int _servingWeight = weight;
    private ZLinkFrameworkRuntimeState _state = ZLinkFrameworkRuntimeState.Serving;
    private IRouterSocket? _router;
    private long _livenessAckCount;
    private long _livenessProbeCount;
    private long _receivedLivenessProbeCount;
    internal event Action<Snapshot>? SnapshotChanged;

    internal ZLinkChannelName ChannelName { get; } =
        ZLinkChannelName.FromBoundary(channelName, nameof(channelName));
    internal RoutingId ServerRid { get; } = serverRid;

    // Fixed at bind from the resolver (spec 04 §3.1); the listener record and every descriptor
    // read this one value.
    internal string AdvertisedEndpoint { get; } = advertisedEndpoint;
    internal ulong LifecycleGeneration { get; } = lifecycleGeneration;
    internal string SecurityIdentity { get; } = securityIdentity;
    internal uint NormalizedEffectiveMaxMessageBytes { get; } = normalizedEffectiveMaxMessageBytes;
    internal long LivenessAckCount => Interlocked.Read(ref _livenessAckCount);
    internal long LivenessProbeCount => Interlocked.Read(ref _livenessProbeCount);
    internal long ReceivedLivenessProbeCount => Interlocked.Read(ref _receivedLivenessProbeCount);

    internal ValueTask<int> GetAdmittedPeerCountAsync() => _lane.RunAsync(() => _peers.Count);

    internal ValueTask<Snapshot> ReadAsync() => _lane.RunAsync(ReadOnLane);

    internal async ValueTask<Snapshot> MarkDrainingAsync()
    {
        var snapshot = await _lane.RunAsync(MarkDrainingOnLane).ConfigureAwait(false);
        PushUpdate(snapshot);
        SnapshotChanged?.Invoke(snapshot);
        return snapshot;
    }

    internal ValueTask<Snapshot> MarkRetiringAsync() =>
        SetLifecycleStateAsync(ZLinkFrameworkRuntimeState.Relocating);

    internal ValueTask<Snapshot> MarkServingAsync() =>
        SetLifecycleStateAsync(ZLinkFrameworkRuntimeState.Serving);

    private async ValueTask<Snapshot> SetLifecycleStateAsync(ZLinkFrameworkRuntimeState state)
    {
        var snapshot = await _lane
            .RunAsync(() => SetLifecycleStateOnLane(state))
            .ConfigureAwait(false);
        PushUpdate(snapshot);
        SnapshotChanged?.Invoke(snapshot);
        return snapshot;
    }

    internal async ValueTask<Snapshot> SetWeightAsync(int weight)
    {
        ZLinkSocketConfig.ValidatePeerWeight(weight);
        var snapshot = await _lane.RunAsync(() => SetWeightOnLane(weight)).ConfigureAwait(false);
        PushUpdate(snapshot);
        SnapshotChanged?.Invoke(snapshot);
        return snapshot;
    }

    internal ValueTask AttachRouterAsync(IRouterSocket router) =>
        _lane.RunAsync(() =>
        {
            _router = router;
        });

    internal ValueTask DetachRouterAsync(IRouterSocket router) =>
        _lane.RunAsync(() =>
        {
            if (ReferenceEquals(_router, router))
                _router = null;
            _peers.Clear();
        });

    internal ValueTask AdmitPeerAsync(
        RoutingId routingId,
        uint normalizedEffectiveMaxMessageBytes,
        ulong connectionId
    )
    {
        var now = Stopwatch.GetTimestamp();
        return _lane.RunAsync(() =>
        {
            var liveness =
                _peers.TryGetValue(routingId, out var current)
                && current.ConnectionId == connectionId
                    ? current.Liveness
                    : new ZLinkServiceLiveness(now);
            liveness.RecordReceived(now);
            _peers[routingId] = new Peer(
                routingId,
                normalizedEffectiveMaxMessageBytes,
                liveness,
                connectionId
            );
        });
    }

    internal ValueTask<(bool Found, uint MaximumMessageBytes)> ObserveReceivedRecordAsync(
        RoutingId routingId,
        ulong connectionId,
        long receivedTimestamp
    ) =>
        _lane.RunAsync(() =>
        {
            if (!_peers.TryGetValue(routingId, out var peer))
                return (false, 0u);
            if (peer.ConnectionId == connectionId)
                peer.Liveness.RecordReceived(receivedTimestamp);
            return (true, peer.NormalizedEffectiveMaxMessageBytes);
        });

    internal ValueTask AcceptLivenessAckAsync(
        RoutingId routingId,
        ulong connectionId,
        ulong probeId
    ) =>
        _lane.RunAsync(() =>
        {
            if (
                !_peers.TryGetValue(routingId, out var peer)
                || peer.ConnectionId != connectionId
                || !peer.Liveness.Acknowledge(probeId, Stopwatch.GetTimestamp())
            )
                return;
            Interlocked.Increment(ref _livenessAckCount);
        });

    internal void RecordLivenessProbe(RoutingId routingId)
    {
        _ = routingId;
        Interlocked.Increment(ref _receivedLivenessProbeCount);
    }

    internal async ValueTask TickLivenessAsync(
        IRouterSocket router,
        CancellationToken cancellationToken
    )
    {
        var now = Stopwatch.GetTimestamp();
        var (probes, expired) = await _lane
            .RunAsync(() => PrepareLivenessTick(now))
            .ConfigureAwait(false);
        var failures = new ZLinkFailureCollector();
        foreach (var routingId in expired)
            failures.Capture(() => router.DisconnectRid(routingId));
        foreach (var probe in probes)
            await failures
                .CaptureAsync(async () =>
                {
                    await SendOwnedAsync(
                            router,
                            probe.RoutingId,
                            ZLinkClientServerControlProtocol.EncodeLivenessProbe(probe.ProbeId),
                            cancellationToken
                        )
                        .ConfigureAwait(false);
                    Interlocked.Increment(ref _livenessProbeCount);
                })
                .ConfigureAwait(false);
        failures.ThrowIfAny();
    }

    private void PushUpdate(Snapshot snapshot)
    {
        var (router, peers) = AwaitStateLane(_lane.RunAsync(GetPushUpdateTargets));
        if (router is null)
            return;
        foreach (var peer in peers)
            ZLinkUnawaitedSubmit.Observe(
                SendOwnedAsync(
                    router,
                    peer.RoutingId,
                    ZLinkClientServerControlProtocol.EncodeUpdate(
                        ToAdmission(snapshot) with
                        {
                            NormalizedEffectiveMaxMessageBytes = peer.MaximumMessageBytes,
                        }
                    ),
                    CancellationToken.None
                ),
                nameof(PushUpdate),
                errorSink
            );
    }

    private Snapshot ReadOnLane() => new(_revision, _weight, _state, AdvertisedEndpoint);

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
        _weight = state == ZLinkFrameworkRuntimeState.Serving ? _servingWeight : 0;
        return ReadOnLane();
    }

    private Snapshot SetWeightOnLane(int weight)
    {
        if (_state != ZLinkFrameworkRuntimeState.Serving)
            throw new ZLinkConfigurationException(
                $"ClientServer Server '{ChannelName}' is not serving."
            );
        _revision = checked(_revision + 1);
        _weight = weight;
        _servingWeight = weight;
        return ReadOnLane();
    }

    private (
        (RoutingId RoutingId, ulong ProbeId)[] Probes,
        RoutingId[] Expired
    ) PrepareLivenessTick(long now)
    {
        List<(RoutingId RoutingId, ulong ProbeId)> probes = [];
        List<RoutingId> expired = [];
        foreach (var (key, peer) in _peers.ToArray())
        {
            if (peer.Liveness.IsExpired(now))
            {
                _peers.Remove(key);
                expired.Add(peer.RoutingId);
                continue;
            }
            if (peer.Liveness.TryGetProbe(now, out var probeId))
                probes.Add((peer.RoutingId, probeId));
        }
        return ([.. probes], [.. expired]);
    }

    private (
        IRouterSocket? Router,
        (RoutingId RoutingId, uint MaximumMessageBytes)[] Peers
    ) GetPushUpdateTargets() =>
        (
            _router,
            _peers
                .Values.Select(static peer =>
                    (peer.RoutingId, peer.NormalizedEffectiveMaxMessageBytes)
                )
                .ToArray()
        );

    internal ZLinkClientServerControlProtocol.Admission ToAdmission(Snapshot snapshot) =>
        new(
            ChannelName.Value,
            ServerRid,
            LifecycleGeneration,
            snapshot.Revision,
            snapshot.Weight,
            snapshot.State,
            SecurityIdentity,
            NormalizedEffectiveMaxMessageBytes,
            snapshot.AdvertisedEndpoint
        );

    private static async ValueTask SendOwnedAsync(
        IRouterSocket router,
        RoutingId routingId,
        Message message,
        CancellationToken cancellationToken
    )
    {
        try
        {
            await router
                .Send(routingId)
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

    internal readonly record struct Snapshot(
        ulong Revision,
        int Weight,
        ZLinkFrameworkRuntimeState State,
        string AdvertisedEndpoint
    );

    private sealed class Peer(
        RoutingId routingId,
        uint normalizedEffectiveMaxMessageBytes,
        ZLinkServiceLiveness liveness,
        ulong connectionId
    )
    {
        internal RoutingId RoutingId { get; } = routingId;
        internal uint NormalizedEffectiveMaxMessageBytes { get; } =
            normalizedEffectiveMaxMessageBytes;
        internal ZLinkServiceLiveness Liveness { get; } = liveness;
        internal ulong ConnectionId { get; } = connectionId;
    }
}
