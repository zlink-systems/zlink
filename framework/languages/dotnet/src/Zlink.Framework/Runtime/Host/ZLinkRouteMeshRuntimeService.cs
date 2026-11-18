using System.Runtime.CompilerServices;
using Zlink.Framework.Runtime.Diagnostics;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Identifiers;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;
using static Zlink.Framework.Runtime.Execution.ZLinkStateLaneWait;

namespace Zlink.Framework.Runtime.Host;

/// <summary>
/// IZLinkRouteMeshRuntime over the registered MeshNodes (spec 50). Snapshots
/// read the Core status/peer tables directly; the event stream consumes the
/// independent Core MeshNode monitor, so event handlers never sit on the
/// dispatch path.
/// Peer ChannelName sets and channel readiness are derived from the Core peer
/// and peer-channel snapshots.
/// </summary>
internal sealed class ZLinkRouteMeshRuntimeService : IZLinkRouteMeshRuntime, IAsyncDisposable
{
    private static readonly TimeSpan DescriptorPollInterval = TimeSpan.FromMilliseconds(100);

    // The public RouteMesh stream projects topology changes, not per-message
    // operations. RawMeshMonitor is FIFO, so subscribing to data-plane events
    // can otherwise leave a peer transition behind unrelated message traffic.
    internal const MeshMonitorEventMask TopologyMonitorEvents =
        MeshMonitorEventMask.StateChanged
        | MeshMonitorEventMask.PeerConnecting
        | MeshMonitorEventMask.PeerAdmitted
        | MeshMonitorEventMask.PeerDraining
        | MeshMonitorEventMask.PeerClosed
        | MeshMonitorEventMask.PeerRejected
        | MeshMonitorEventMask.ChannelChanged
        | MeshMonitorEventMask.ProtocolError
        | MeshMonitorEventMask.ClaimRevoked
        | MeshMonitorEventMask.PeerNotRequired;

    private readonly ZLinkFrameworkRuntime _runtime;
    private readonly ZLinkFrameworkHostLifecycleState _hostLifecycle;
    private readonly ZLinkLocationStoreHealth? _storeHealth;
    private readonly IZLinkLocationDescriptorQuery? _locationQuery;
    private readonly ZLinkStateLane _lane = new();
    private readonly Dictionary<ZLinkMeshName, MonitorHub> _monitorHubs = [];
    private IDisposable? _metricRegistration;
    private bool _stopped;

    internal ZLinkRouteMeshRuntimeService(
        ZLinkFrameworkRuntime runtime,
        ZLinkFrameworkHostLifecycleState hostLifecycle,
        ZLinkLocationStoreHealth? storeHealth,
        IZLinkLocationDescriptorQuery? locationQuery
    )
    {
        _runtime = runtime;
        _hostLifecycle = hostLifecycle;
        _storeHealth = storeHealth;
        _locationQuery = locationQuery;
        _hostLifecycle.Changed += OnHostStateChanged;
    }

    public ZLinkRouteMeshStatus GetStatus(string meshName)
    {
        var nodeRuntime = _runtime.GetMeshNodeRuntime(meshName);
        var hub = GetOrCreateHub(meshName, nodeRuntime);
        return hub.GetStatus();
    }

    private ZLinkRouteMeshStatus BuildStatus(
        string meshName,
        ZLinkSpotNodeRuntime nodeRuntime,
        MonitorHub hub
    )
    {
        var status = nodeRuntime.Node.MeshTopology();
        var physicalPeers = status.Peers;
        var peerChannels = status.PeerChannels;
        var placement = hub.LocalDescriptor(status.RoutingId);
        var capacity = placement?.Capacity;
        var activeActors = _runtime.GetActiveActorCount(meshName);
        var activeSpots = nodeRuntime.ActiveSpotCount;
        var locationUnavailable = _storeHealth?.GetSnapshot() is { Healthy: false };
        var hostState = _hostLifecycle.State;
        var state = ApplyHostState(MapTopologyState(status.State), hostState);
        var localPlacementReady =
            state == ZLinkTopologyState.Ready
            && hostState == ZLinkFrameworkRuntimeState.Serving
            && !locationUnavailable;
        var previous = hub.LastStatus;
        var peers = ProjectPeers(nodeRuntime, hub, status.RoutingId, physicalPeers);
        var readyPeerCount = 0;
        var requiredPeerUnavailable = false;
        foreach (var peer in peers)
        {
            if (peer.State == ZLinkPeerState.Ready)
                readyPeerCount++;
            if (peer.State is ZLinkPeerState.Connecting or ZLinkPeerState.NotConnected)
                requiredPeerUnavailable = true;
        }
        if (state == ZLinkTopologyState.Ready && (locationUnavailable || requiredPeerUnavailable))
            state = ZLinkTopologyState.Degraded;
        var isReady =
            state == ZLinkTopologyState.Ready && hostState == ZLinkFrameworkRuntimeState.Serving;
        var projectedPeers = previous?.Peers;
        if (projectedPeers is null || !SamePeers(projectedPeers, peers, physicalPeers))
        {
            var changedPeers = peers.ToList();
            changedPeers.Sort(
                static (left, right) => ZLinkRoutingIdOrder.Instance.Compare(left.Rid, right.Rid)
            );
            var rebuiltPeers = changedPeers
                .Select(static peer => new ZLinkPeerStatus(
                    peer.Rid,
                    peer.State,
                    MapPeerReason(peer.State)
                ))
                .ToArray();
            projectedPeers =
                projectedPeers is not null && projectedPeers.SequenceEqual(rebuiltPeers)
                    ? projectedPeers
                    : rebuiltPeers;
        }
        var projectedChannels = previous?.Channels;
        var channelNames = ChannelNames(nodeRuntime, peerChannels);
        if (
            projectedChannels is null
            || !SameChannels(projectedChannels, channelNames, physicalPeers, peerChannels, isReady)
        )
            projectedChannels = ChannelReadyCounts(nodeRuntime, physicalPeers, peerChannels)
                .Select(channel => new ZLinkChannelStatus(
                    channel.Name,
                    isReady && channel.ReadyMembers > 0,
                    channel.ReadyMembers
                ))
                .ToArray();
        var placementAvailable =
            localPlacementReady
            && nodeRuntime.Registration.ObjectRole == ZLinkMeshNodeObjectRole.Server
            && nodeRuntime.Registration.PlacementWeight > 0
            && (
                HasRemainingCapacity(
                    activeActors,
                    capacity?.Actors.Reserved ?? 0,
                    capacity?.Actors.Limit ?? nodeRuntime.Registration.ActorLimit
                )
                || HasRemainingCapacity(
                    activeSpots,
                    capacity?.Spots.Reserved ?? 0,
                    capacity?.Spots.Limit ?? nodeRuntime.Registration.SpotLimit
                )
            )
            && HasRemainingCapacity(
                nodeRuntime.ActivationAdmission.Active,
                0,
                nodeRuntime.ActivationAdmission.Limit
            );
        var placementReason = PlacementUnavailableReason(
            placementAvailable,
            hostState,
            locationUnavailable
        );
        if (
            previous is not null
            && previous.State == state
            && previous.IsReady == isReady
            && previous.ReadyPeerCount == readyPeerCount
            && ReferenceEquals(previous.Peers, projectedPeers)
            && ReferenceEquals(previous.Channels, projectedChannels)
            && previous.Placement.IsAvailable == placementAvailable
            && previous.Placement.ActiveActorCount == activeActors
            && previous.Placement.ActiveSpotCount == activeSpots
            && previous.Placement.UnavailableReason == placementReason
        )
            return previous;
        return new ZLinkRouteMeshStatus(
            meshName,
            state,
            isReady,
            readyPeerCount,
            projectedChannels,
            projectedPeers,
            new ZLinkPlacementStatus(
                placementAvailable,
                activeActors,
                activeSpots,
                placementReason
            ),
            previous?.Sequence ?? 0,
            DateTimeOffset.UtcNow
        );
    }

    private static ZLinkTopologyReason? PlacementUnavailableReason(
        bool available,
        ZLinkFrameworkRuntimeState hostState,
        bool locationUnavailable
    ) =>
        available ? null
        : hostState
            is ZLinkFrameworkRuntimeState.Relocating
                or ZLinkFrameworkRuntimeState.Relocated
                or ZLinkFrameworkRuntimeState.Draining
            ? ZLinkTopologyReason.Draining
        : hostState != ZLinkFrameworkRuntimeState.Serving ? ZLinkTopologyReason.RuntimeNotReady
        : locationUnavailable ? ZLinkTopologyReason.LocationUnavailable
        : ZLinkTopologyReason.CapacityExceeded;

    internal static bool HasRemainingCapacity(ZLinkPopulationCapacity capacity) =>
        HasRemainingCapacity(capacity.Active, capacity.Reserved, capacity.Limit);

    internal static bool HasRemainingCapacity(ZLinkActivationConcurrency concurrency) =>
        HasRemainingCapacity(concurrency.Active, 0, concurrency.Limit);

    private static bool HasRemainingCapacity(int active, int reserved, int limit) =>
        limit == 0 || (long)active + reserved < limit;

    internal void Start()
    {
        foreach (var meshName in _runtime.Registration.SpotNodes.Keys)
        {
            var nodeRuntime = _runtime.GetMeshNodeRuntime(meshName);
            _ = GetOrCreateHub(meshName, nodeRuntime);
        }
        _metricRegistration ??= ZLinkRuntimeMetrics.RegisterMeshSnapshots(BuildMetricSnapshots);
    }

    internal async Task StopAsync()
    {
        var hubs = await _lane.RunAsync(StopOnLane).ConfigureAwait(false);
        if (hubs is null)
            return;
        _hostLifecycle.Changed -= OnHostStateChanged;
        Interlocked.Exchange(ref _metricRegistration, null)?.Dispose();
        foreach (var hub in hubs)
            await hub.StopAsync().ConfigureAwait(false);
        await _lane.RunAsync(() => _monitorHubs.Clear()).ConfigureAwait(false);
    }

    private MonitorHub[]? StopOnLane()
    {
        if (_stopped)
            return null;
        _stopped = true;
        return [.. _monitorHubs.Values];
    }

    public ValueTask DisposeAsync() => new(StopAsync());

    private IReadOnlyList<ZLinkRuntimeMetricMeshSnapshot> BuildMetricSnapshots() =>
        _runtime
            .Registration.SpotNodes.Keys.Select(meshName =>
            {
                var nodeRuntime = _runtime.GetMeshNodeRuntime(meshName);
                var topology = nodeRuntime.Node.MeshTopology();
                var peers = topology.Peers;
                var peerChannels = topology.PeerChannels;
                var source = DescriptorSources(nodeRuntime).FirstOrDefault() ?? "manual";
                var capacity = _runtime.ConfirmedCapacity(
                    new(meshName, nodeRuntime.Node.RoutingId),
                    BuildPopulationCapacity(nodeRuntime.Registration)
                );
                return new ZLinkRuntimeMetricMeshSnapshot(
                    meshName,
                    source,
                    peers.Count,
                    peers.Count(static peer =>
                        peer.State
                            is MeshPeerState.Connecting
                                or MeshPeerState.Admitted
                                or MeshPeerState.Draining
                    ),
                    peers.Count(static peer => peer.State == MeshPeerState.Admitted),
                    ChannelReadyCounts(nodeRuntime, peers, peerChannels)
                        .Select(static channel => new ZLinkRuntimeMetricChannel(
                            channel.Name,
                            channel.ReadyMembers
                        ))
                        .ToArray(),
                    capacity is null
                        ? null
                        : new ZLinkRuntimeMetricCapacity(
                            capacity.Actors.Active,
                            capacity.Actors.Reserved,
                            capacity.Actors.Limit
                        ),
                    capacity is null
                        ? null
                        : new ZLinkRuntimeMetricCapacity(
                            capacity.Spots.Active,
                            capacity.Spots.Reserved,
                            capacity.Spots.Limit
                        ),
                    capacity
                        ?.SpotTypes.Select(static entry => new ZLinkRuntimeMetricSpotTypeCapacity(
                            entry.ObjectKind == ZLinkPlacementObjectKind.InstanceSpot
                                ? "instance"
                                : "user",
                            entry.StableType,
                            entry.Active,
                            entry.Reserved,
                            entry.Limit
                        ))
                        .ToArray()
                        ?? [],
                    new ZLinkRuntimeMetricActivation(
                        nodeRuntime.ActivationAdmission.Active,
                        nodeRuntime.ActivationAdmission.Limit
                    ),
                    nodeRuntime
                        .GetInstanceSpotMonitoringSnapshots()
                        .Select(static entry => new ZLinkRuntimeMetricInstanceSpot(
                            entry.InstanceSpotType,
                            entry.PendingMessageCount,
                            entry.PendingByteCount
                        ))
                        .ToArray()
                );
            })
            .ToArray();

    private MonitorHub GetOrCreateHub(string meshName, ZLinkSpotNodeRuntime nodeRuntime)
    {
        if (_lane.IsOnLane)
            return GetOrCreateHubOnLane(meshName, nodeRuntime);
        return AwaitStateLane(_lane.RunAsync(() => GetOrCreateHubOnLane(meshName, nodeRuntime)));
    }

    private MonitorHub GetOrCreateHubOnLane(string meshName, ZLinkSpotNodeRuntime nodeRuntime)
    {
        var meshKey = ZLinkMeshName.FromBoundary(meshName, nameof(meshName));
        if (_monitorHubs.TryGetValue(meshKey, out var hub))
            return hub;
        if (_stopped)
            throw new ObjectDisposedException(nameof(ZLinkRouteMeshRuntimeService));
        hub = new MonitorHub(this, meshKey, nodeRuntime);
        _monitorHubs.Add(meshKey, hub);
        using (ExecutionContext.SuppressFlow())
            hub.Start();
        return hub;
    }

    public async IAsyncEnumerable<ZLinkObservedStatus<ZLinkRouteMeshStatus>> ObserveAsync(
        string meshName,
        [EnumeratorCancellation] CancellationToken cancellationToken = default
    )
    {
        var (hub, observer) = SubscribeMonitor(meshName);
        try
        {
            await foreach (
                var status in observer.ReadAllAsync(cancellationToken).ConfigureAwait(false)
            )
                yield return status;
        }
        finally
        {
            UnsubscribeMonitor(meshName, hub, observer);
        }
    }

    private (MonitorHub Hub, ZLinkObservationQueue<ZLinkRouteMeshStatus> Observer) SubscribeMonitor(
        string meshName
    )
    {
        return AwaitStateLane(
            _lane.RunAsync(() =>
            {
                if (_stopped)
                    throw new ObjectDisposedException(nameof(ZLinkRouteMeshRuntimeService));
                var nodeRuntime = _runtime.GetMeshNodeRuntime(meshName);
                var hub = GetOrCreateHub(meshName, nodeRuntime);
                return (hub, hub.Subscribe());
            })
        );
    }

    private void OnHostStateChanged(ZLinkFrameworkRuntimeState state)
    {
        var hubs = AwaitStateLane(_lane.RunAsync(() => _monitorHubs.Values.ToArray()));
        foreach (var hub in hubs)
            hub.PublishHostStateChanged(state);
    }

    private static ZLinkTopologyState ApplyHostState(
        ZLinkTopologyState topologyState,
        ZLinkFrameworkRuntimeState hostState
    ) =>
        hostState switch
        {
            ZLinkFrameworkRuntimeState.Serving => topologyState,
            ZLinkFrameworkRuntimeState.Preparing => ZLinkTopologyState.Starting,
            ZLinkFrameworkRuntimeState.Relocating
            or ZLinkFrameworkRuntimeState.Relocated
            or ZLinkFrameworkRuntimeState.Draining => ZLinkTopologyState.Stopping,
            ZLinkFrameworkRuntimeState.Stopped => ZLinkTopologyState.Stopped,
            ZLinkFrameworkRuntimeState.Error => ZLinkTopologyState.Failed,
            _ => topologyState,
        };

    private void UnsubscribeMonitor(
        string meshName,
        MonitorHub hub,
        ZLinkObservationQueue<ZLinkRouteMeshStatus> observer
    )
    {
        AwaitStateLane(
            _lane.RunAsync(() =>
            {
                _ = meshName;
                hub.Unsubscribe(observer);
            })
        );
    }

    private static bool UpdatesStatus(MeshMonitorEventKind kind) =>
        kind
            is MeshMonitorEventKind.StateChanged
                or MeshMonitorEventKind.ChannelChanged
                or MeshMonitorEventKind.ClaimRevoked
                or MeshMonitorEventKind.PeerConnecting
                or MeshMonitorEventKind.PeerAdmitted
                or MeshMonitorEventKind.PeerDraining
                or MeshMonitorEventKind.PeerClosed
                or MeshMonitorEventKind.PeerNotRequired
                or MeshMonitorEventKind.PeerRejected
                or MeshMonitorEventKind.ProtocolError;

    private static IReadOnlyList<string> DescriptorSources(ZLinkSpotNodeRuntime nodeRuntime)
    {
        var manual = nodeRuntime.Registration.Router?.ManualConnections.Count > 0;
        var redis =
            nodeRuntime.Registration.Router?.AcquisitionMode
            == ZLinkPeerAcquisitionMode.AutoConnect;
        return manual && redis ? ["manual_and_redis"]
            : manual ? ["manual"]
            : redis ? ["redis"]
            : [];
    }

    internal static ZLinkPlacementCapacity BuildPopulationCapacity(
        ZLinkSpotNodeRegistration registration
    )
    {
        var spotTypes = registration
            .SpotRelocations.Select(static entry => new ZLinkSpotTypeCapacity(
                ZLinkPlacementObjectKind.UserSpot,
                entry.Key,
                Active: 0,
                Reserved: 0,
                entry.Value.Placement.MaxActiveObjects ?? 0
            ))
            .Concat(
                registration.InstanceSpotRelocations.Select(
                    static entry => new ZLinkSpotTypeCapacity(
                        ZLinkPlacementObjectKind.InstanceSpot,
                        entry.Key,
                        Active: 0,
                        Reserved: 0,
                        entry.Value.Placement.MaxActiveObjects ?? 0
                    )
                )
            )
            .OrderBy(static entry => entry.ObjectKind)
            .ThenBy(static entry => entry.StableType, StringComparer.Ordinal)
            .ToArray();
        return new ZLinkPlacementCapacity(
            new ZLinkPopulationCapacity(0, 0, registration.ActorLimit),
            new ZLinkPopulationCapacity(0, 0, registration.SpotLimit),
            spotTypes
        );
    }

    private static ZLinkPeerState MapPeerState(MeshPeerState state)
    {
        return state switch
        {
            MeshPeerState.Configured or MeshPeerState.Connecting => ZLinkPeerState.Connecting,
            MeshPeerState.Admitted => ZLinkPeerState.Ready,
            MeshPeerState.Draining => ZLinkPeerState.Draining,
            MeshPeerState.NotRequired => ZLinkPeerState.NotRequired,
            _ => ZLinkPeerState.NotConnected,
        };
    }

    private static IEnumerable<(string Name, int ReadyMembers)> ChannelReadyCounts(
        ZLinkSpotNodeRuntime nodeRuntime,
        IReadOnlyList<MeshNodePeer> peers,
        IReadOnlyList<MeshPeerChannel>[] peerChannels
    )
    {
        foreach (
            var channelName in ChannelNames(nodeRuntime, peerChannels)
                .Distinct(StringComparer.Ordinal)
                .Order(StringComparer.Ordinal)
        )
            yield return (channelName, ReadyMembers(channelName, peers, peerChannels));
    }

    private static IEnumerable<string> ChannelNames(
        ZLinkSpotNodeRuntime nodeRuntime,
        IReadOnlyList<MeshPeerChannel>[] peerChannels
    )
    {
        foreach (var membership in nodeRuntime.Registration.ChannelMemberships)
            yield return membership.ChannelName;
        foreach (var channels in peerChannels)
        foreach (var channel in channels)
            yield return channel.Name;
    }

    private static int ReadyMembers(
        string channelName,
        IReadOnlyList<MeshNodePeer> peers,
        IReadOnlyList<MeshPeerChannel>[] peerChannels
    )
    {
        // The local membership is published to remote callers; it is not a peer target.
        var readyMembers = 0;
        for (var index = 0; index < peers.Count; index++)
        {
            if (peers[index].State != MeshPeerState.Admitted)
                continue;
            foreach (var channel in peerChannels[index])
            {
                if (
                    channel.Weight > 0
                    && string.Equals(channel.Name, channelName, StringComparison.Ordinal)
                )
                {
                    readyMembers++;
                    break;
                }
            }
        }
        return readyMembers;
    }

    private static bool SameChannels(
        IReadOnlyList<ZLinkChannelStatus> previous,
        IEnumerable<string> names,
        IReadOnlyList<MeshNodePeer> peers,
        IReadOnlyList<MeshPeerChannel>[] peerChannels,
        bool isReady
    )
    {
        foreach (var name in names)
            if (
                !previous.Any(channel =>
                    string.Equals(channel.ChannelName, name, StringComparison.Ordinal)
                )
            )
                return false;
        foreach (var channel in previous)
        {
            if (!names.Contains(channel.ChannelName, StringComparer.Ordinal))
                return false;
            var count = ReadyMembers(channel.ChannelName, peers, peerChannels);
            if (channel.ReadyTargetCount != count || channel.IsReady != (isReady && count > 0))
                return false;
        }
        return true;
    }

    private static IEnumerable<(RoutingId Rid, ZLinkPeerState State)> ProjectPeers(
        ZLinkSpotNodeRuntime nodeRuntime,
        MonitorHub hub,
        RoutingId localRid,
        IReadOnlyList<MeshNodePeer> physicalPeers
    )
    {
        foreach (var peer in physicalPeers)
            yield return (peer.RoutingId, MapPeerState(peer.State));
        foreach (var descriptor in hub.Descriptors())
        {
            if (
                descriptor.Rid == localRid
                || physicalPeers.Any(peer => peer.RoutingId == descriptor.Rid)
            )
                continue;
            var notRequired = ZLinkRouteMeshConnectionPolicy.IsNotRequired(
                nodeRuntime.Registration.ObjectRole,
                nodeRuntime.Registration.ChannelMemberships.Any(static membership =>
                    membership.IsServer
                ),
                descriptor.ObjectRole,
                descriptor.ChannelWeights.Count != 0
            );
            yield return (
                descriptor.Rid,
                notRequired ? ZLinkPeerState.NotRequired : ZLinkPeerState.NotConnected
            );
        }
    }

    private static bool SamePeers(
        IReadOnlyList<ZLinkPeerStatus> previous,
        IEnumerable<(RoutingId Rid, ZLinkPeerState State)> peers,
        IReadOnlyList<MeshNodePeer> physicalPeers
    )
    {
        // Multiple physical intents can project the same RID. Rebuild those
        // inputs with the existing sort before comparing their ordered values.
        for (var index = 0; index < physicalPeers.Count; index++)
        for (var other = 0; other < index; other++)
            if (physicalPeers[index].RoutingId == physicalPeers[other].RoutingId)
                return false;
        var count = 0;
        foreach (var peer in peers)
        {
            count++;
            var low = 0;
            var high = previous.Count - 1;
            while (low <= high)
            {
                var middle = low + (high - low) / 2;
                var comparison = ZLinkRoutingIdOrder.Instance.Compare(
                    previous[middle].NodeRid,
                    peer.Rid
                );
                if (comparison < 0)
                    low = middle + 1;
                else if (comparison > 0)
                    high = middle - 1;
                else
                {
                    low = middle;
                    break;
                }
            }
            if (
                low >= previous.Count
                || previous[low].NodeRid != peer.Rid
                || previous[low].State != peer.State
            )
                return false;
        }
        return count == previous.Count;
    }

    private static ZLinkTopologyState MapTopologyState(MeshNodeState state) =>
        state switch
        {
            MeshNodeState.Created => ZLinkTopologyState.Starting,
            MeshNodeState.Started or MeshNodeState.PartialReady or MeshNodeState.Ready =>
                ZLinkTopologyState.Ready,
            MeshNodeState.Draining => ZLinkTopologyState.Stopping,
            MeshNodeState.Stopped => ZLinkTopologyState.Stopped,
            _ => ZLinkTopologyState.Failed,
        };

    private static ZLinkTopologyReason? MapPeerReason(ZLinkPeerState state) =>
        state switch
        {
            ZLinkPeerState.Ready => null,
            ZLinkPeerState.NotRequired => null,
            ZLinkPeerState.Draining => ZLinkTopologyReason.Draining,
            ZLinkPeerState.Connecting or ZLinkPeerState.NotConnected =>
                ZLinkTopologyReason.NoReadyPeer,
            _ => ZLinkTopologyReason.InternalFailure,
        };

    /// <summary>
    /// Owns the managed topology monitor for one MeshNode and fans events out
    /// without putting observer backpressure on the receive loop.
    /// </summary>
    private sealed class MonitorHub
    {
        private readonly ZLinkRouteMeshRuntimeService _owner;
        private readonly ZLinkMeshName _meshName;
        private readonly ZLinkSpotNodeRuntime _nodeRuntime;
        private readonly IMeshNodeMonitor _monitor;
        private readonly CancellationTokenSource _stop = new();
        private readonly ZLinkStateLane _lane = new();
        private Task? _pump;
        private readonly List<ZLinkObservationQueue<ZLinkRouteMeshStatus>> _observers = [];
        private readonly Dictionary<RoutingId, ZLinkMeshNodeDescriptor> _descriptors = [];
        private ZLinkRouteMeshStatus? _lastStatus;

        public MonitorHub(
            ZLinkRouteMeshRuntimeService owner,
            ZLinkMeshName meshName,
            ZLinkSpotNodeRuntime nodeRuntime
        )
        {
            _owner = owner;
            _meshName = meshName;
            _nodeRuntime = nodeRuntime;
            _monitor = nodeRuntime.Node.OpenMeshMonitor(TopologyMonitorEvents);
        }

        public void Start()
        {
            if (_owner._storeHealth is not null)
                _owner._storeHealth.Changed += PublishCurrentStatus;
            AwaitStateLane(_lane.RunAsync(() => _pump ??= StartPump()));
        }

        private Task StartPump()
        {
            if (ExecutionContext.IsFlowSuppressed())
                return Task.Run(PumpAsync);
            using (ExecutionContext.SuppressFlow())
                return Task.Run(PumpAsync);
        }

        public ZLinkMeshNodeDescriptor? LocalDescriptor(RoutingId rid) =>
            _descriptors.GetValueOrDefault(rid);

        public IEnumerable<ZLinkMeshNodeDescriptor> Descriptors() => _descriptors.Values;

        public ZLinkRouteMeshStatus? LastStatus => _lastStatus;

        public ZLinkRouteMeshStatus GetStatus() =>
            AwaitStateLane(_lane.RunAsync(PublishCurrentStatusOnLane));

        public ZLinkObservationQueue<ZLinkRouteMeshStatus> Subscribe()
        {
            return AwaitStateLane(
                _lane.RunAsync(() =>
                {
                    var status = PublishCurrentStatusOnLane();
                    var observer = new ZLinkObservationQueue<ZLinkRouteMeshStatus>(
                        status,
                        status.State.IsTerminal(),
                        static status => status.MeshName,
                        eventName: "route_mesh"
                    );
                    _observers.Add(observer);
                    return observer;
                })
            );
        }

        public void Unsubscribe(ZLinkObservationQueue<ZLinkRouteMeshStatus> observer)
        {
            AwaitStateLane(_lane.RunAsync(() => _observers.Remove(observer)));
            observer.Complete();
        }

        public async Task StopAsync()
        {
            if (_owner._storeHealth is not null)
                _owner._storeHealth.Changed -= PublishCurrentStatus;
            var stopped = await _lane
                .RunAsync(() =>
                {
                    _stop.Cancel();
                    if (_lastStatus is { } current)
                        PublishStatusOnLane(
                            current with
                            {
                                State = ZLinkTopologyState.Stopped,
                                IsReady = false,
                                Channels = current
                                    .Channels.Select(static channel =>
                                        channel with
                                        {
                                            IsReady = false,
                                        }
                                    )
                                    .ToArray(),
                                Placement = current.Placement with
                                {
                                    IsAvailable = false,
                                    UnavailableReason = ZLinkTopologyReason.RuntimeNotReady,
                                },
                                ObservedAt = DateTimeOffset.UtcNow,
                            }
                        );
                    _observers.Clear();
                    return _pump;
                })
                .ConfigureAwait(false);
            if (stopped is not null)
                await stopped.ConfigureAwait(false);
            _stop.Dispose();
        }

        public void PublishHostStateChanged(ZLinkFrameworkRuntimeState state)
        {
            _ = state;
            PublishCurrentStatus();
        }

        private async Task PumpAsync()
        {
            var projectionPending = false;
            Task<bool>? monitorReady = null;
            Task? descriptorDue = null;
            try
            {
                while (!_stop.IsCancellationRequested)
                {
                    try
                    {
                        if (projectionPending)
                        {
                            PublishCurrentStatus();
                            projectionPending = false;
                        }

                        if (_owner._locationQuery is not null && descriptorDue is null)
                        {
                            descriptorDue = Task.Delay(DescriptorPollInterval, _stop.Token);
                            await PublishDescriptorChangesAsync(_stop.Token).ConfigureAwait(false);
                        }
                        while (_monitor.Recv(RecvFlags.DontWait) is { } monitorEvent)
                            if (UpdatesStatus(monitorEvent.Kind))
                                PublishCurrentStatus();
                    }
                    catch (OperationCanceledException) when (_stop.IsCancellationRequested)
                    {
                        throw;
                    }
                    catch (Exception error)
                    {
                        // Retain the observers and retry a full current status.
                        // If a wake event was already removed from the FIFO, the
                        // retry preserves its convergence signal.
                        projectionPending = true;
                        ZLinkFrameworkDebugLog.TaskFailure("route-mesh-monitor-pump", error);
                    }
                    monitorReady ??= _monitor.WaitAsync(_stop.Token).AsTask();
                    if (descriptorDue is null)
                        await monitorReady.ConfigureAwait(false);
                    else
                        await Task.WhenAny(monitorReady, descriptorDue).ConfigureAwait(false);
                    if (monitorReady.IsCompleted)
                    {
                        if (!await monitorReady.ConfigureAwait(false))
                            return;
                        monitorReady = null;
                    }
                    if (descriptorDue?.IsCompleted == true)
                    {
                        await descriptorDue.ConfigureAwait(false);
                        descriptorDue = null;
                    }
                }
            }
            catch (OperationCanceledException) when (_stop.IsCancellationRequested) { }
            finally
            {
                _monitor.Dispose();
            }
        }

        private async ValueTask PublishDescriptorChangesAsync(CancellationToken cancellationToken)
        {
            ZLinkLocationPage<ZLinkMeshNodeDescriptor> current;
            try
            {
                current = await _owner
                    ._locationQuery!.ListMeshNodeDescriptorsAsync(
                        _meshName.Value,
                        default,
                        cancellationToken
                    )
                    .ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                throw;
            }
            catch
            {
                // Location health owns store-failure reporting. Monitoring
                // remains subscribed and retries at the next polling interval.
                return;
            }
            if (await _lane.RunAsync(() => _descriptors.Count == 0).ConfigureAwait(false))
            {
                await _lane
                    .RunAsync(() =>
                    {
                        foreach (var descriptor in current.Items)
                            _descriptors[descriptor.Rid] = descriptor;
                    })
                    .ConfigureAwait(false);
                PublishCurrentStatus();
                return;
            }

            var sourceRid = _nodeRuntime.Node.RoutingId;
            foreach (var descriptor in current.Items)
            {
                var previous = await _lane
                    .RunAsync(() =>
                    {
                        _descriptors.TryGetValue(descriptor.Rid, out var value);
                        return value;
                    })
                    .ConfigureAwait(false);
                if (previous is null)
                {
                    await _lane
                        .RunAsync(() => _descriptors[descriptor.Rid] = descriptor)
                        .ConfigureAwait(false);
                    if (descriptor.Rid != sourceRid)
                        PublishCurrentStatus();
                    continue;
                }
                var capacityChanged =
                    !SameCapacity(previous.Capacity, descriptor.Capacity)
                    || previous.ActivationConcurrency != descriptor.ActivationConcurrency;
                var placementChanged =
                    capacityChanged
                    || previous.PlacementWeight != descriptor.PlacementWeight
                    || previous.ObjectRole != descriptor.ObjectRole;
                await _lane
                    .RunAsync(() => _descriptors[descriptor.Rid] = descriptor)
                    .ConfigureAwait(false);
                var channelsChanged =
                    previous.ChannelWeights.Count != descriptor.ChannelWeights.Count
                    || !previous.ChannelWeights.All(pair =>
                        descriptor.ChannelWeights.TryGetValue(pair.Key, out var weight)
                        && weight == pair.Value
                    );
                if (
                    previous.ObjectRole != descriptor.ObjectRole
                    || placementChanged && descriptor.Rid == sourceRid
                    || channelsChanged
                )
                    PublishCurrentStatus();
            }
            var currentIds = current.Items.Select(static descriptor => descriptor.Rid).ToHashSet();
            var removedDescriptors = await _lane
                .RunAsync(() =>
                {
                    var removed = _descriptors
                        .Keys.Where(rid => !currentIds.Contains(rid))
                        .ToArray();
                    foreach (var descriptor in removed)
                        _descriptors.Remove(descriptor);
                    return removed;
                })
                .ConfigureAwait(false);
            if (removedDescriptors.Any(removed => removed != sourceRid))
                PublishCurrentStatus();
        }

        private static bool SameCapacity(
            ZLinkPlacementCapacity left,
            ZLinkPlacementCapacity right
        ) =>
            left.Actors == right.Actors
            && left.Spots == right.Spots
            && left.SpotTypes.SequenceEqual(right.SpotTypes);

        private void PublishCurrentStatus()
        {
            _ = _lane.IsOnLane
                ? PublishCurrentStatusOnLane()
                : AwaitStateLane(_lane.RunAsync(PublishCurrentStatusOnLane));
        }

        private ZLinkRouteMeshStatus PublishCurrentStatusOnLane()
        {
            if (_stop.IsCancellationRequested && _lastStatus is { } stopped)
                return stopped;
            if (_lastStatus is { } terminal && terminal.State.IsTerminal())
                return terminal;
            var status = _owner.BuildStatus(_meshName.Value, _nodeRuntime, this);
            return PublishStatusOnLane(status);
        }

        private ZLinkRouteMeshStatus PublishStatusOnLane(ZLinkRouteMeshStatus status)
        {
            if (
                _lastStatus is { } previous
                && (previous.State.IsTerminal() || ReferenceEquals(previous, status))
            )
                return previous;
            status = status with { Sequence = (_lastStatus?.Sequence ?? 0) + 1 };
            _lastStatus = status;
            foreach (var observer in _observers)
                observer.Publish(status, status.State.IsTerminal());
            return status;
        }
    }
}
