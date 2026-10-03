using System.Diagnostics;
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
    private static readonly TimeSpan MonitorIdleDelay = TimeSpan.FromMilliseconds(10);

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

    private (ZLinkMeshNodeSnapshot Snapshot, bool LocationUnavailable) SnapshotInternal(
        string meshName
    )
    {
        var nodeRuntime = _runtime.GetMeshNodeRuntime(meshName);
        var hub = GetOrCreateHub(meshName, nodeRuntime);
        return SnapshotInternal(meshName, nodeRuntime, hub);
    }

    private (ZLinkMeshNodeSnapshot Snapshot, bool LocationUnavailable) SnapshotInternal(
        string meshName,
        ZLinkSpotNodeRuntime nodeRuntime,
        MonitorHub hub
    )
    {
        var status = nodeRuntime.Node.MeshStatus();
        var peers = nodeRuntime.Node.MeshPeers();
        var peerChannels = peers.Select(peer => SnapshotPeerChannels(nodeRuntime, peer)).ToArray();
        var state = MapNodeState(status.State);
        var placement = hub.LocalDescriptor(status.RoutingId);
        var capacity = placement?.Capacity ?? BuildPopulationCapacity(nodeRuntime.Registration);
        // Active counts come from this MeshNode's local activations.
        capacity = capacity with
        {
            Actors = capacity.Actors with { Active = _runtime.GetActiveActorCount(meshName) },
            Spots = capacity.Spots with { Active = nodeRuntime.ActiveSpotCount },
        };
        var locationHealth = _storeHealth?.GetSnapshot();
        var snapshot = new ZLinkMeshNodeSnapshot(
            meshName,
            status.RoutingId,
            status.LifecycleGeneration,
            status.DescriptorRevision,
            status.LocalEndpoint,
            state,
            hub.CurrentSequence,
            DateTimeOffset.UtcNow,
            DescriptorSources(nodeRuntime),
            peers.Select((peer, index) => MapPeer(peer, peerChannels[index])).ToArray(),
            MapChannels(nodeRuntime, peers, peerChannels),
            new ZLinkMeshClaimSnapshot(
                ApplicationActive: state == ZLinkMeshNodeState.Serving,
                status.PendingApplicationMessages,
                InfrastructureActive: state
                    is ZLinkMeshNodeState.Serving
                        or ZLinkMeshNodeState.Draining,
                status.PendingInfrastructureMessages
            ),
            ZLinkLocationStoreHealth.ProjectSnapshot(locationHealth)
        )
        {
            ApplicationVersion =
                placement?.ApplicationVersion ?? _runtime.Registration.ApplicationVersion,
            ObjectRole = placement?.ObjectRole ?? nodeRuntime.Registration.ObjectRole,
            PlacementWeight =
                placement?.PlacementWeight ?? nodeRuntime.Registration.PlacementWeight,
            PopulationCapacity = capacity,
            ActivationConcurrency = new ZLinkActivationConcurrency(
                nodeRuntime.ActivationAdmission.Active,
                nodeRuntime.ActivationAdmission.Limit
            ),
            ObjectCapabilities =
                placement?.ObjectCapabilities ?? Array.Empty<ZLinkObjectCapability>(),
            InstanceSpots = nodeRuntime.GetInstanceSpotMonitoringSnapshots(),
        };
        return (snapshot, locationHealth is { Healthy: false });
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
        var (snapshot, locationUnavailable) = SnapshotInternal(meshName, nodeRuntime, hub);
        var state = MapTopologyState(snapshot.State);
        var hostState = _hostLifecycle.State;
        state = ApplyHostState(state, hostState);
        var localRuntimeReady =
            state == ZLinkTopologyState.Ready && hostState == ZLinkFrameworkRuntimeState.Serving;
        var localPlacementReady = localRuntimeReady && !locationUnavailable;
        var peers = snapshot
            .Peers.Select(static peer => new ZLinkPeerStatus(
                peer.Rid,
                MapPeerState(peer.State),
                MapPeerReason(peer)
            ))
            .ToList();
        var physicalPeerIds = peers.Select(static peer => peer.NodeRid).ToHashSet();
        var localObjectRole = snapshot.ObjectRole;
        foreach (var descriptor in hub.Descriptors())
        {
            if (descriptor.Rid == snapshot.Rid || physicalPeerIds.Contains(descriptor.Rid))
                continue;
            var notRequired = ZLinkRouteMeshConnectionPolicy.IsNotRequired(
                localObjectRole,
                nodeRuntime.Registration.ChannelMemberships.Any(static membership =>
                    membership.IsServer
                ),
                descriptor.ObjectRole,
                descriptor.ChannelWeights.Count != 0
            );
            peers.Add(
                new ZLinkPeerStatus(
                    descriptor.Rid,
                    notRequired ? ZLinkPeerState.NotRequired : ZLinkPeerState.NotConnected,
                    notRequired ? null : ZLinkTopologyReason.NoReadyPeer
                )
            );
        }
        peers.Sort(
            static (left, right) =>
                StringComparer.Ordinal.Compare(left.NodeRid.ToHex(), right.NodeRid.ToHex())
        );
        if (
            state == ZLinkTopologyState.Ready
            && peers.Any(static peer =>
                peer.State is ZLinkPeerState.Connecting or ZLinkPeerState.NotConnected
            )
        )
            state = ZLinkTopologyState.Degraded;
        if (state == ZLinkTopologyState.Ready && locationUnavailable)
            state = ZLinkTopologyState.Degraded;
        var isReady =
            state == ZLinkTopologyState.Ready && hostState == ZLinkFrameworkRuntimeState.Serving;
        var channels = snapshot
            .Channels.Select(channel => new ZLinkChannelStatus(
                channel.ChannelName,
                isReady && channel.Selectable,
                channel.ReadyMemberCount
            ))
            .ToArray();
        var hasRemainingCapacity =
            HasRemainingCapacity(snapshot.PopulationCapacity.Actors)
            || HasRemainingCapacity(snapshot.PopulationCapacity.Spots);
        var hasActivationConcurrency = HasRemainingCapacity(snapshot.ActivationConcurrency);
        var placementAvailable =
            localPlacementReady
            && snapshot.ObjectRole == ZLinkMeshNodeObjectRole.Server
            && snapshot.PlacementWeight > 0
            && hasRemainingCapacity
            && hasActivationConcurrency;
        return new ZLinkRouteMeshStatus(
            snapshot.MeshName,
            state,
            isReady,
            peers.Count(static peer => peer.State == ZLinkPeerState.Ready),
            channels,
            peers,
            new ZLinkPlacementStatus(
                placementAvailable,
                snapshot.PopulationCapacity.Actors.Active,
                snapshot.PopulationCapacity.Spots.Active,
                PlacementUnavailableReason(placementAvailable, hostState, locationUnavailable)
            ),
            snapshot.Sequence,
            snapshot.ObservedAt
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
        capacity.Limit == 0 || (long)capacity.Active + capacity.Reserved < capacity.Limit;

    internal static bool HasRemainingCapacity(ZLinkActivationConcurrency concurrency) =>
        concurrency.Limit == 0 || concurrency.Active < concurrency.Limit;

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
            .Registration.SpotNodes.Keys.Select(meshName => SnapshotInternal(meshName).Snapshot)
            .Select(static snapshot =>
            {
                var source = snapshot.DescriptorSources.FirstOrDefault() ?? "manual";
                var capacity = snapshot.PopulationCapacity;
                return new ZLinkRuntimeMetricMeshSnapshot(
                    snapshot.MeshName,
                    source,
                    snapshot.Peers.Count,
                    snapshot.Peers.Count(static peer =>
                        peer.State
                            is MeshPeerState.Connecting
                                or MeshPeerState.Admitted
                                or MeshPeerState.Draining
                    ),
                    snapshot.Peers.Count(static peer => peer.State == MeshPeerState.Admitted),
                    snapshot
                        .Channels.Select(static channel => new ZLinkRuntimeMetricChannel(
                            channel.ChannelName,
                            channel.ReadyMemberCount
                        ))
                        .ToArray(),
                    new ZLinkRuntimeMetricCapacity(
                        capacity.Actors.Active,
                        capacity.Actors.Reserved,
                        capacity.Actors.Limit
                    ),
                    new ZLinkRuntimeMetricCapacity(
                        capacity.Spots.Active,
                        capacity.Spots.Reserved,
                        capacity.Spots.Limit
                    ),
                    capacity
                        .SpotTypes.Select(static entry => new ZLinkRuntimeMetricSpotTypeCapacity(
                            entry.ObjectKind == ZLinkPlacementObjectKind.InstanceSpot
                                ? "instance"
                                : "user",
                            entry.StableType,
                            entry.Active,
                            entry.Reserved,
                            entry.Limit
                        ))
                        .ToArray(),
                    new ZLinkRuntimeMetricActivation(
                        snapshot.ActivationConcurrency.Active,
                        snapshot.ActivationConcurrency.Limit
                    ),
                    snapshot
                        .InstanceSpots.Select(static entry => new ZLinkRuntimeMetricInstanceSpot(
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

    private static IReadOnlyList<MeshPeerChannel> SnapshotPeerChannels(
        ZLinkSpotNodeRuntime nodeRuntime,
        MeshNodePeer peer
    )
    {
        if (peer.State == MeshPeerState.Closed || peer.RoutingId.IsEmpty)
            return [];
        try
        {
            return nodeRuntime.Node.MeshPeerChannels(peer.RoutingId, peer.LifecycleGeneration);
        }
        catch (ZlinkConfigException error)
            when (error.Result == ZlinkConfigException.ErrorCode.NotFound)
        {
            // Peers and their channel table are separate atomic Core reads.
            // A lifecycle that ended between them is no longer selectable.
            return [];
        }
    }

    private static ZLinkMeshPeerSnapshot MapPeer(
        MeshNodePeer peer,
        IReadOnlyList<MeshPeerChannel> channels
    )
    {
        return new ZLinkMeshPeerSnapshot(
            peer.RoutingId,
            peer.LifecycleGeneration,
            peer.DescriptorRevision,
            peer.Endpoint,
            peer.State,
            channels.Select(static channel => channel.Name).ToArray(),
            peer.LastError == 0 ? null : $"errno {peer.LastError}"
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

    private static IReadOnlyList<ZLinkMeshChannelSnapshot> MapChannels(
        ZLinkSpotNodeRuntime nodeRuntime,
        IReadOnlyList<MeshNodePeer> peers,
        IReadOnlyList<MeshPeerChannel>[] peerChannels
    )
    {
        var localMemberships = nodeRuntime
            .Registration.ChannelMemberships.Where(static membership => membership.IsServer)
            .ToDictionary(
                static membership => membership.ChannelName,
                static membership => membership.Weight,
                StringComparer.Ordinal
            );
        var channelNames = nodeRuntime
            .Registration.ChannelMemberships.Select(static membership => membership.ChannelName)
            .Concat(
                peerChannels.SelectMany(static channels =>
                    channels.Select(static channel => channel.Name)
                )
            )
            .Distinct(StringComparer.Ordinal)
            .OrderBy(static channelName => channelName, StringComparer.Ordinal);

        return channelNames
            .Select(channelName =>
            {
                var localWeight = localMemberships.GetValueOrDefault(channelName);
                // RouteMesh select-one sends through an admitted peer. The
                // local Server membership is published for remote callers,
                // but this node is not its own peer target.
                var readyMembers = 0;
                for (var index = 0; index < peers.Count; index++)
                {
                    if (peers[index].State != MeshPeerState.Admitted)
                        continue;
                    if (
                        peerChannels[index]
                            .Any(channel =>
                                channel.Weight > 0
                                && string.Equals(
                                    channel.Name,
                                    channelName,
                                    StringComparison.Ordinal
                                )
                            )
                    )
                        readyMembers++;
                }
                return new ZLinkMeshChannelSnapshot(
                    channelName,
                    localWeight,
                    readyMembers,
                    readyMembers > 0
                );
            })
            .ToArray();
    }

    private static ZLinkMeshNodeState MapNodeState(MeshNodeState state)
    {
        return state switch
        {
            MeshNodeState.Created => ZLinkMeshNodeState.Starting,
            MeshNodeState.Started or MeshNodeState.PartialReady or MeshNodeState.Ready =>
                ZLinkMeshNodeState.Serving,
            MeshNodeState.Draining => ZLinkMeshNodeState.Draining,
            MeshNodeState.Stopped => ZLinkMeshNodeState.Stopped,
            _ => ZLinkMeshNodeState.Faulted,
        };
    }

    private static ZLinkTopologyState MapTopologyState(ZLinkMeshNodeState state) =>
        state switch
        {
            ZLinkMeshNodeState.Starting => ZLinkTopologyState.Starting,
            ZLinkMeshNodeState.Serving => ZLinkTopologyState.Ready,
            ZLinkMeshNodeState.Draining
            or ZLinkMeshNodeState.Drained
            or ZLinkMeshNodeState.ForceStopping => ZLinkTopologyState.Stopping,
            ZLinkMeshNodeState.Stopped => ZLinkTopologyState.Stopped,
            _ => ZLinkTopologyState.Failed,
        };

    private static ZLinkTopologyReason? MapPeerReason(ZLinkMeshPeerSnapshot peer) =>
        MapPeerState(peer.State) switch
        {
            ZLinkPeerState.Ready => null,
            ZLinkPeerState.NotRequired => null,
            ZLinkPeerState.Draining => ZLinkTopologyReason.Draining,
            ZLinkPeerState.Connecting or ZLinkPeerState.NotConnected =>
                ZLinkTopologyReason.NoReadyPeer,
            _ => ZLinkTopologyReason.InternalFailure,
        };

    /// <summary>
    /// Owns Core's single monitor for one MeshNode and fans events out without
    /// putting observer backpressure on the native receive loop.
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
        private TimeSpan _nextDescriptorPoll;
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

        public ZLinkMeshNodeDescriptor? LocalDescriptor(RoutingId rid)
        {
            if (_lane.IsOnLane)
                return _descriptors.GetValueOrDefault(rid);
            return AwaitStateLane(_lane.RunAsync(() => _descriptors.GetValueOrDefault(rid)));
        }

        public IReadOnlyList<ZLinkMeshNodeDescriptor> Descriptors() =>
            _lane.IsOnLane
                ? DescriptorsOnLane()
                : AwaitStateLane(_lane.RunAsync(DescriptorsOnLane));

        private IReadOnlyList<ZLinkMeshNodeDescriptor> DescriptorsOnLane() =>
            _descriptors
                .Values.OrderBy(static descriptor => descriptor.Rid, ZLinkRoutingIdOrder.Instance)
                .ToArray();

        public ulong CurrentSequence =>
            _lane.IsOnLane
                ? _lastStatus?.Sequence ?? 0
                : AwaitStateLane(_lane.RunAsync(() => _lastStatus?.Sequence ?? 0));

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

                        await PublishDescriptorChangesAsync(_stop.Token).ConfigureAwait(false);

                        var nativeEvent = _monitor.Recv(RecvFlags.DontWait);
                        if (nativeEvent is null)
                        {
                            await Task.Delay(MonitorIdleDelay, _stop.Token).ConfigureAwait(false);
                            continue;
                        }

                        if (UpdatesStatus(nativeEvent.Kind))
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
                        await Task.Delay(MonitorIdleDelay, _stop.Token).ConfigureAwait(false);
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
            if (
                _owner._locationQuery is null
                || !await _lane
                    .RunAsync(() =>
                    {
                        if (Stopwatch.GetElapsedTime(0) < _nextDescriptorPoll)
                            return false;
                        _nextDescriptorPoll =
                            Stopwatch.GetElapsedTime(0) + TimeSpan.FromMilliseconds(100);
                        return true;
                    })
                    .ConfigureAwait(false)
            )
                return;

            ZLinkLocationPage<ZLinkMeshNodeDescriptor> current;
            try
            {
                current = await _owner
                    ._locationQuery.ListMeshNodeDescriptorsAsync(
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

            var sourceRid = _nodeRuntime.Node.MeshStatus().RoutingId;
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
            var status = _owner.BuildStatus(_meshName.Value, _nodeRuntime, this);
            return PublishStatusOnLane(status);
        }

        private ZLinkRouteMeshStatus PublishStatusOnLane(ZLinkRouteMeshStatus status)
        {
            if (
                _lastStatus is { } previous
                && (previous.State.IsTerminal() || SamePublicStatus(previous, status))
            )
                return previous;
            status = status with { Sequence = (_lastStatus?.Sequence ?? 0) + 1 };
            _lastStatus = status;
            foreach (var observer in _observers)
                observer.Publish(status, status.State.IsTerminal());
            return status;
        }

        private static bool SamePublicStatus(
            ZLinkRouteMeshStatus left,
            ZLinkRouteMeshStatus right
        ) =>
            left.MeshName == right.MeshName
            && left.State == right.State
            && left.IsReady == right.IsReady
            && left.ReadyPeerCount == right.ReadyPeerCount
            && left.Placement == right.Placement
            && left.Channels.SequenceEqual(right.Channels)
            && left.Peers.SequenceEqual(right.Peers);
    }
}
