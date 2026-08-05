using System.Runtime.CompilerServices;
using Zlink.Framework.Runtime.Diagnostics;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.Runtime.Host;

/// <summary>
/// IZLinkRouteMeshRuntime over the registered MeshNodes (spec 50). Snapshots
/// read the Core status/peer tables directly; the event stream consumes the
/// independent Core MeshNode monitor, so event handlers never sit on the
/// dispatch path.
/// Peer ChannelName sets and channel readiness are derived from the Core peer
/// and peer-channel snapshots.
/// </summary>
internal sealed class ZLinkRouteMeshRuntimeService : IZLinkRouteMeshRuntime, IDisposable
{
    private static readonly TimeSpan MonitorIdleDelay = TimeSpan.FromMilliseconds(10);

    private readonly ZLinkFrameworkRuntime _runtime;
    private readonly ZLinkFrameworkHostLifecycleState _hostLifecycle;
    private readonly ZLinkLocationStoreHealth? _storeHealth;
    private readonly IZLinkLocationDescriptorQuery? _locationQuery;
    private readonly object _sequenceGate = new();
    private readonly Dictionary<string, ulong> _sequences = new(StringComparer.Ordinal);
    private readonly object _monitorGate = new();
    private readonly Dictionary<string, MonitorHub> _monitorHubs =
        new(StringComparer.Ordinal);
    private IDisposable? _metricRegistration;
    private bool _stopped;

    internal ZLinkRouteMeshRuntimeService(
        ZLinkFrameworkRuntime runtime,
        ZLinkFrameworkHostLifecycleState hostLifecycle,
        ZLinkLocationStoreHealth? storeHealth,
        IZLinkLocationDescriptorQuery? locationQuery)
    {
        _runtime = runtime;
        _hostLifecycle = hostLifecycle;
        _storeHealth = storeHealth;
        _locationQuery = locationQuery;
        _hostLifecycle.Changed += OnHostStateChanged;
    }

    private ZLinkMeshNodeSnapshot SnapshotInternal(string meshName)
    {
        var nodeRuntime = _runtime.GetMeshNodeRuntime(meshName);
        var hub = GetOrCreateHub(meshName, nodeRuntime);
        var status = nodeRuntime.Node.MeshStatus();
        var peers = nodeRuntime.Node.MeshPeers();
        var peerChannels = peers.Select(peer =>
                SnapshotPeerChannels(nodeRuntime, peer))
            .ToArray();
        var state = MapNodeState(status.State);
        var placement = hub.LocalDescriptor(status.RoutingId);
        var fallbackCapacity = BuildPopulationCapacity(nodeRuntime.Registration);
        return new ZLinkMeshNodeSnapshot(
            meshName,
            status.RoutingId,
            status.LifecycleGeneration,
            status.DescriptorRevision,
            status.LocalEndpoint,
            state,
            NextSequence(meshName),
            DateTimeOffset.UtcNow,
            DescriptorSources(nodeRuntime),
            peers.Select((peer, index) => MapPeer(peer, peerChannels[index]))
                .ToArray(),
            MapChannels(nodeRuntime, peers, peerChannels),
            new ZLinkMeshClaimSnapshot(
                ApplicationActive: state == ZLinkMeshNodeState.Serving,
                status.PendingApplicationMessages,
                InfrastructureActive: state is ZLinkMeshNodeState.Serving or ZLinkMeshNodeState.Draining,
                status.PendingInfrastructureMessages),
            LocationSnapshot())
        {
            ApplicationVersion = placement?.ApplicationVersion
                ?? _runtime.Registration.ApplicationVersion,
            ObjectRole = placement?.ObjectRole
                ?? nodeRuntime.Registration.ObjectRole,
            PlacementWeight = placement?.PlacementWeight
                ?? nodeRuntime.Registration.PlacementWeight,
            PopulationCapacity = placement?.Capacity ?? fallbackCapacity,
            ActivationConcurrency = new ZLinkActivationConcurrency(
                nodeRuntime.ActivationAdmission.Active,
                nodeRuntime.ActivationAdmission.Limit),
            ObjectCapabilities = placement?.ObjectCapabilities
                ?? Array.Empty<ZLinkObjectCapability>(),
            InstanceSpots = nodeRuntime.GetInstanceSpotMonitoringSnapshots()
        };
    }

    public ZLinkRouteMeshStatus GetStatus(string meshName)
    {
        var nodeRuntime = _runtime.GetMeshNodeRuntime(meshName);
        var hub = GetOrCreateHub(meshName, nodeRuntime);
        var snapshot = SnapshotInternal(meshName);
        var state = MapTopologyState(snapshot.State);
        var hostState = _hostLifecycle.State;
        state = ApplyHostState(state, hostState);
        var localRuntimeReady = state == ZLinkTopologyState.Ready
                                && hostState == ZLinkFrameworkRuntimeState.Serving;
        var locationUnavailable = string.Equals(
            snapshot.Location.State,
            "degraded",
            StringComparison.Ordinal);
        var localPlacementReady = localRuntimeReady && !locationUnavailable;
        var peers = snapshot.Peers
            .Select(static peer => new ZLinkPeerStatus(
                peer.Rid,
                MapPeerStatus(peer),
                MapPeerReason(peer)))
            .ToList();
        var physicalPeerIds = peers
            .Select(static peer => peer.NodeRid)
            .ToHashSet();
        var localObjectRole = snapshot.ObjectRole;
        foreach (var descriptor in hub.Descriptors())
        {
            if (descriptor.Rid == snapshot.Rid
                || physicalPeerIds.Contains(descriptor.Rid))
                continue;
            var notRequired = ZLinkRouteMeshConnectionPolicy.IsNotRequired(
                localObjectRole,
                nodeRuntime.Registration.ChannelMemberships.Any(
                    static membership => membership.IsServer),
                descriptor.ObjectRole,
                descriptor.ChannelWeights.Count != 0);
            peers.Add(new ZLinkPeerStatus(
                descriptor.Rid,
                notRequired
                    ? ZLinkPeerState.NotRequired
                    : ZLinkPeerState.NotConnected,
                notRequired ? null : ZLinkTopologyReason.NoReadyPeer));
        }
        peers.Sort(static (left, right) => StringComparer.Ordinal.Compare(
            left.NodeRid.ToHex(),
            right.NodeRid.ToHex()));
        if (state == ZLinkTopologyState.Ready
            && peers.Any(static peer =>
                peer.State is ZLinkPeerState.Connecting
                    or ZLinkPeerState.NotConnected))
            state = ZLinkTopologyState.Degraded;
        if (state == ZLinkTopologyState.Ready && locationUnavailable)
            state = ZLinkTopologyState.Degraded;
        var isReady = state == ZLinkTopologyState.Ready
                      && hostState == ZLinkFrameworkRuntimeState.Serving;
        var channels = snapshot.Channels
            .Select(channel => new ZLinkChannelStatus(
                channel.ChannelName,
                isReady && channel.Selectable,
                channel.ReadyMemberCount))
            .ToArray();
        var counts = _runtime.GetDrainRemainderCounts();
        var hasRemainingCapacity =
            HasRemainingCapacity(snapshot.PopulationCapacity.Actors)
            || HasRemainingCapacity(snapshot.PopulationCapacity.Spots);
        var hasActivationConcurrency =
            HasRemainingCapacity(snapshot.ActivationConcurrency);
        var placementAvailable = localPlacementReady
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
                counts.Actors,
                counts.Spots,
                placementAvailable
                    ? null
                    : state == ZLinkTopologyState.Stopping
                        ? ZLinkTopologyReason.Draining
                        : locationUnavailable
                            ? ZLinkTopologyReason.LocationUnavailable
                        : !localPlacementReady
                            ? ZLinkTopologyReason.RuntimeNotReady
                            : ZLinkTopologyReason.CapacityExceeded),
            snapshot.Sequence,
            snapshot.ObservedAt);
    }

    internal static bool HasRemainingCapacity(
        ZLinkPopulationCapacity capacity) =>
        capacity.Limit == 0
        || (long)capacity.Active + capacity.Reserved < capacity.Limit;

    internal static bool HasRemainingCapacity(
        ZLinkActivationConcurrency concurrency) =>
        concurrency.Limit == 0 || concurrency.Active < concurrency.Limit;

    internal void Start()
    {
        foreach (var meshName in _runtime.Registration.SpotNodes.Keys)
        {
            var nodeRuntime = _runtime.GetMeshNodeRuntime(meshName);
            _ = GetOrCreateHub(meshName, nodeRuntime);
        }
        _metricRegistration ??= ZLinkRuntimeMetrics.RegisterMeshSnapshots(
            BuildMetricSnapshots);
    }

    internal void Stop()
    {
        MonitorHub[] hubs;
        lock (_monitorGate)
        {
            if (_stopped)
                return;
            _stopped = true;
            hubs = [.. _monitorHubs.Values];
        }
        _hostLifecycle.Changed -= OnHostStateChanged;
        Interlocked.Exchange(ref _metricRegistration, null)?.Dispose();
        foreach (var hub in hubs)
            hub.Stop();
        lock (_monitorGate)
            _monitorHubs.Clear();
    }

    public void Dispose() => Stop();

    private IReadOnlyList<ZLinkRuntimeMetricMeshSnapshot> BuildMetricSnapshots() =>
        _runtime.Registration.SpotNodes.Keys
            .Select(SnapshotInternal)
            .Select(static snapshot =>
            {
                var source = snapshot.DescriptorSources.FirstOrDefault()
                             ?? "manual";
                var capacity = snapshot.PopulationCapacity;
                return new ZLinkRuntimeMetricMeshSnapshot(
                    snapshot.MeshName,
                    source,
                    snapshot.Peers.Count,
                    snapshot.Peers.Count(static peer =>
                        peer.AdmissionState is "connecting" or "ready" or "draining"),
                    snapshot.Peers.Count(static peer => peer.Ready),
                    snapshot.Channels.Select(static channel =>
                            new ZLinkRuntimeMetricChannel(
                                channel.ChannelName,
                                channel.ReadyMemberCount))
                        .ToArray(),
                    new ZLinkRuntimeMetricCapacity(
                        capacity.Actors.Active,
                        capacity.Actors.Reserved,
                        capacity.Actors.Limit),
                    new ZLinkRuntimeMetricCapacity(
                        capacity.Spots.Active,
                        capacity.Spots.Reserved,
                        capacity.Spots.Limit),
                    capacity.SpotTypes.Select(static entry =>
                            new ZLinkRuntimeMetricSpotTypeCapacity(
                                entry.ObjectKind == ZLinkPlacementObjectKind.InstanceSpot
                                    ? "instance"
                                    : "user",
                                entry.StableType,
                                entry.Active,
                                entry.Reserved,
                                entry.Limit))
                        .ToArray(),
                    new ZLinkRuntimeMetricActivation(
                        snapshot.ActivationConcurrency.Active,
                        snapshot.ActivationConcurrency.Limit),
                    snapshot.InstanceSpots.Select(static entry =>
                            new ZLinkRuntimeMetricInstanceSpot(
                                entry.InstanceSpotType,
                                entry.PendingMessageCount,
                                entry.PendingByteCount))
                        .ToArray());
            })
            .ToArray();

    private MonitorHub GetOrCreateHub(
        string meshName,
        ZLinkSpotNodeRuntime nodeRuntime)
    {
        lock (_monitorGate)
        {
            if (_monitorHubs.TryGetValue(meshName, out var hub))
                return hub;
            if (_stopped)
                throw new ObjectDisposedException(nameof(ZLinkRouteMeshRuntimeService));
            hub = new MonitorHub(this, meshName, nodeRuntime);
            _monitorHubs.Add(meshName, hub);
            hub.Start();
            return hub;
        }
    }

    public async IAsyncEnumerable<ZLinkObservedStatus<ZLinkRouteMeshStatus>> ObserveAsync(
        string meshName,
        [EnumeratorCancellation] CancellationToken cancellationToken = default)
    {
        const int capacity = 1024;
        var (hub, observer) = SubscribeMonitor(meshName, capacity);
        try
        {
            await foreach (var status in observer.ReadAllAsync(cancellationToken)
                               .ConfigureAwait(false))
                yield return status;
        }
        finally
        {
            UnsubscribeMonitor(meshName, hub, observer);
        }
    }

    private (
        MonitorHub Hub,
        ZLinkObservationQueue<ZLinkRouteMeshStatus> Observer) SubscribeMonitor(
        string meshName,
        int capacity)
    {
        lock (_monitorGate)
        {
            if (_stopped)
                throw new ObjectDisposedException(
                    nameof(ZLinkRouteMeshRuntimeService));
            var nodeRuntime = _runtime.GetMeshNodeRuntime(meshName);
            var hub = GetOrCreateHub(meshName, nodeRuntime);
            return (hub, hub.Subscribe(capacity));
        }
    }

    private void OnHostStateChanged(ZLinkFrameworkRuntimeState state)
    {
        MonitorHub[] hubs;
        lock (_monitorGate)
            hubs = [.. _monitorHubs.Values];
        foreach (var hub in hubs)
            hub.PublishHostStateChanged(state);
    }

    private static ZLinkTopologyState ApplyHostState(
        ZLinkTopologyState topologyState,
        ZLinkFrameworkRuntimeState hostState) =>
        hostState switch
        {
            ZLinkFrameworkRuntimeState.Serving => topologyState,
            ZLinkFrameworkRuntimeState.Preparing => ZLinkTopologyState.Starting,
            ZLinkFrameworkRuntimeState.Relocating
                or ZLinkFrameworkRuntimeState.Relocated
                or ZLinkFrameworkRuntimeState.Draining =>
                ZLinkTopologyState.Stopping,
            ZLinkFrameworkRuntimeState.Stopped =>
                ZLinkTopologyState.Stopped,
            ZLinkFrameworkRuntimeState.Error => ZLinkTopologyState.Failed,
            _ => topologyState
        };

    private void UnsubscribeMonitor(
        string meshName,
        MonitorHub hub,
        ZLinkObservationQueue<ZLinkRouteMeshStatus> observer)
    {
        lock (_monitorGate)
        {
            _ = meshName;
            hub.Unsubscribe(observer);
        }
    }

    private IReadOnlyList<ZLinkMeshRuntimeEvent> MapMonitorEvents(
        string meshName,
        RoutingId sourceRid,
        MeshMonitorEvent nativeEvent)
    {
        RoutingId? peerRid = nativeEvent.PeerRid.IsEmpty
            ? null
            : nativeEvent.PeerRid;
        var identifier = nativeEvent.Kind switch
        {
            MeshMonitorEventKind.StateChanged =>
                "zlink.runtime.mesh_node.state_changed",
            MeshMonitorEventKind.ChannelChanged =>
                "zlink.runtime.mesh_node.channel_changed",
            MeshMonitorEventKind.ClaimRevoked =>
                "zlink.runtime.mesh_node.claim_changed",
            MeshMonitorEventKind.PeerConnecting
                or MeshMonitorEventKind.PeerAdmitted
                or MeshMonitorEventKind.PeerDraining
                or MeshMonitorEventKind.PeerClosed
                or MeshMonitorEventKind.PeerNotRequired
                or MeshMonitorEventKind.PeerRejected
                or MeshMonitorEventKind.ProtocolError =>
                "zlink.runtime.mesh_node.peer_changed",
            _ => null
        };
        if (identifier is null)
            return [];

        var reason = nativeEvent.Kind switch
        {
            MeshMonitorEventKind.PeerConnecting => "connecting",
            MeshMonitorEventKind.PeerAdmitted => "ready",
            MeshMonitorEventKind.PeerDraining => "draining",
            MeshMonitorEventKind.PeerClosed => "disconnected",
            MeshMonitorEventKind.PeerNotRequired => "not_required",
            MeshMonitorEventKind.PeerRejected => "HandshakeFailed",
            MeshMonitorEventKind.ProtocolError => "rejected",
            MeshMonitorEventKind.Backpressured => "backpressure",
            _ => null
        };
        var mapped = new ZLinkMeshRuntimeEvent(
            identifier,
            NextSequence(meshName),
            DateTimeOffset.UtcNow,
            meshName,
            sourceRid,
            peerRid,
            nativeEvent.PeerLifecycleGeneration == 0
                ? null
                : nativeEvent.PeerLifecycleGeneration,
            nativeEvent.PeerDescriptorRevision == 0
                ? null
                : nativeEvent.PeerDescriptorRevision,
            string.IsNullOrEmpty(nativeEvent.ChannelName)
                ? null
                : nativeEvent.ChannelName,
            ClaimDomain: nativeEvent.Kind == MeshMonitorEventKind.ClaimRevoked
                ? "application"
                : null,
            MessageKind: null,
            PlacementOutcome: null,
            Capacity: null,
            PopulationCapacity: null,
            ActivationConcurrency: null,
            reason,
            nativeEvent.Kind == MeshMonitorEventKind.StateChanged
                ? MapNodeState(nativeEvent.MeshState)
                : null);
        if (nativeEvent.Kind != MeshMonitorEventKind.StateChanged)
            return [mapped];

        return [mapped];
    }

    private ulong NextSequence(string meshName)
    {
        lock (_sequenceGate)
        {
            var next = _sequences.TryGetValue(meshName, out var current) ? current + 1 : 1;
            _sequences[meshName] = next;
            return next;
        }
    }

    private ZLinkMeshRuntimeEvent Event(
        string identifier,
        string meshName,
        RoutingId sourceRid,
        RoutingId? peerRid = null,
        ulong? lifecycleGeneration = null,
        ulong? descriptorRevision = null,
        string? channelName = null,
        string? placementOutcome = null,
        ZLinkCapacityVector? capacity = null,
        ZLinkPlacementCapacity? populationCapacity = null,
        ZLinkActivationConcurrency? activationConcurrency = null,
        string? reason = null,
        ZLinkMeshNodeState? state = null)
    {
        return new ZLinkMeshRuntimeEvent(
            identifier,
            NextSequence(meshName),
            DateTimeOffset.UtcNow,
            meshName,
            sourceRid,
            peerRid,
            lifecycleGeneration,
            descriptorRevision,
            channelName,
            ClaimDomain: null,
            MessageKind: null,
            placementOutcome,
            capacity,
            populationCapacity,
            activationConcurrency,
            reason,
            state);
    }

    private static IReadOnlyList<string> DescriptorSources(ZLinkSpotNodeRuntime nodeRuntime)
    {
        var manual = nodeRuntime.Registration.Router?.ManualConnections.Count > 0;
        var redis = nodeRuntime.Registration.Router?.AcquisitionMode
            == ZLinkPeerAcquisitionMode.AutoConnect;
        return manual && redis
            ? ["manual_and_redis"]
            : manual
                ? ["manual"]
                : redis
                    ? ["redis"]
                    : [];
    }

    internal static ZLinkPlacementCapacity BuildPopulationCapacity(
        ZLinkSpotNodeRegistration registration)
    {
        var spotTypes = registration.SpotRelocations
            .Select(static entry => new ZLinkSpotTypeCapacity(
                ZLinkPlacementObjectKind.UserSpot,
                entry.Key,
                Active: 0,
                Reserved: 0,
                entry.Value.Placement.MaxActiveObjects ?? 0))
            .Concat(registration.InstanceSpotRelocations.Select(
                static entry => new ZLinkSpotTypeCapacity(
                    ZLinkPlacementObjectKind.InstanceSpot,
                    entry.Key,
                    Active: 0,
                    Reserved: 0,
                    entry.Value.Placement.MaxActiveObjects ?? 0)))
            .OrderBy(static entry => entry.ObjectKind)
            .ThenBy(static entry => entry.StableType, StringComparer.Ordinal)
            .ToArray();
        return new ZLinkPlacementCapacity(
            new ZLinkPopulationCapacity(0, 0, registration.ActorLimit),
            new ZLinkPopulationCapacity(0, 0, registration.SpotLimit),
            spotTypes);
    }

    private ZLinkLocationRuntimeSnapshot LocationSnapshot()
    {
        if (_storeHealth is null)
            return new ZLinkLocationRuntimeSnapshot("not_configured", null, null);

        var snapshot = _storeHealth.GetSnapshot();
        return new ZLinkLocationRuntimeSnapshot(
            snapshot.Healthy ? "ready" : "degraded",
            snapshot.LastSuccessAt,
            snapshot.LastFailureAt);
    }

    private static IReadOnlyList<MeshPeerChannel> SnapshotPeerChannels(
        ZLinkSpotNodeRuntime nodeRuntime,
        MeshNodePeer peer)
    {
        if (peer.State == MeshPeerState.Closed || peer.RoutingId.IsEmpty)
            return [];
        try
        {
            return nodeRuntime.Node.MeshPeerChannels(
                peer.RoutingId,
                peer.LifecycleGeneration);
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
        IReadOnlyList<MeshPeerChannel> channels)
    {
        var mapped = MapPeerState(peer);
        return new ZLinkMeshPeerSnapshot(
            peer.RoutingId,
            peer.LifecycleGeneration,
            peer.DescriptorRevision,
            peer.Endpoint,
            mapped.AdmissionState,
            mapped.Ready,
            mapped.DrainState,
            channels.Select(static channel => channel.Name).ToArray(),
            peer.LastError == 0 ? null : $"errno {peer.LastError}");
    }

    private static (string AdmissionState, bool Ready, string DrainState) MapPeerState(
        MeshNodePeer peer)
    {
        return peer.State switch
        {
            MeshPeerState.Configured => ("configured", false, "serving"),
            MeshPeerState.Connecting => ("connecting", false, "serving"),
            MeshPeerState.Admitted => ("ready", true, "serving"),
            MeshPeerState.Draining => ("draining", false, "draining"),
            MeshPeerState.NotRequired => ("not_required", false, "serving"),
            MeshPeerState.Closed => ("disconnected", false, "serving"),
            _ => ("rejected", false, "serving")
        };
    }

    private static IReadOnlyList<ZLinkMeshChannelSnapshot> MapChannels(
        ZLinkSpotNodeRuntime nodeRuntime,
        IReadOnlyList<MeshNodePeer> peers,
        IReadOnlyList<MeshPeerChannel>[] peerChannels)
    {
        var localMemberships = nodeRuntime.Registration.ChannelMemberships
            .Where(static membership => membership.IsServer)
            .ToDictionary(
                static membership => membership.ChannelName,
                static membership => membership.Weight,
                StringComparer.Ordinal);
        var channelNames = nodeRuntime.Registration.ChannelMemberships
            .Select(static membership => membership.ChannelName)
            .Concat(peerChannels.SelectMany(static channels =>
                channels.Select(static channel => channel.Name)))
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
                    if (peerChannels[index].Any(channel =>
                            channel.Weight > 0
                            && string.Equals(
                                channel.Name,
                                channelName,
                                StringComparison.Ordinal)))
                        readyMembers++;
                }
                return new ZLinkMeshChannelSnapshot(
                    channelName,
                    localWeight,
                    readyMembers,
                    readyMembers > 0);
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
            _ => ZLinkMeshNodeState.Faulted
        };
    }

    private static ZLinkTopologyState MapTopologyState(
        ZLinkMeshNodeState state) =>
        state switch
        {
            ZLinkMeshNodeState.Starting => ZLinkTopologyState.Starting,
            ZLinkMeshNodeState.Serving => ZLinkTopologyState.Ready,
            ZLinkMeshNodeState.Draining
                or ZLinkMeshNodeState.Drained
                or ZLinkMeshNodeState.ForceStopping =>
                ZLinkTopologyState.Stopping,
            ZLinkMeshNodeState.Stopped => ZLinkTopologyState.Stopped,
            _ => ZLinkTopologyState.Failed
        };

    private static ZLinkPeerState MapPeerStatus(
        ZLinkMeshPeerSnapshot peer) =>
        peer.AdmissionState switch
        {
            "ready" => ZLinkPeerState.Ready,
            "draining" => ZLinkPeerState.Draining,
            "not_required" => ZLinkPeerState.NotRequired,
            "configured" or "connecting" => ZLinkPeerState.Connecting,
            _ => ZLinkPeerState.NotConnected
        };

    private static ZLinkTopologyReason? MapPeerReason(
        ZLinkMeshPeerSnapshot peer) =>
        MapPeerStatus(peer) switch
        {
            ZLinkPeerState.Ready => null,
            ZLinkPeerState.NotRequired => null,
            ZLinkPeerState.Draining => ZLinkTopologyReason.Draining,
            ZLinkPeerState.Connecting
                or ZLinkPeerState.NotConnected =>
                ZLinkTopologyReason.NoReadyPeer,
            _ => ZLinkTopologyReason.InternalFailure
        };

    /// <summary>
    /// Owns Core's single monitor for one MeshNode and fans events out without
    /// putting observer backpressure on the native receive loop.
    /// </summary>
    private sealed class MonitorHub
    {
        private readonly ZLinkRouteMeshRuntimeService _owner;
        private readonly string _meshName;
        private readonly ZLinkSpotNodeRuntime _nodeRuntime;
        private readonly IMeshNodeMonitor _monitor;
        private readonly CancellationTokenSource _stop = new();
        private Task? _pump;
        private readonly object _gate = new();
        private readonly List<ZLinkObservationQueue<ZLinkRouteMeshStatus>> _observers = [];
        private readonly Dictionary<RoutingId, ZLinkMeshNodeDescriptor> _descriptors = [];
        private DateTimeOffset _nextDescriptorPoll;
        private string? _lastLocationState;
        private ZLinkRouteMeshStatus? _lastStatus;

        public MonitorHub(
            ZLinkRouteMeshRuntimeService owner,
            string meshName,
            ZLinkSpotNodeRuntime nodeRuntime)
        {
            _owner = owner;
            _meshName = meshName;
            _nodeRuntime = nodeRuntime;
            _monitor = nodeRuntime.Node.OpenMeshMonitor();
        }

        public void Start() => _pump ??= Task.Run(PumpAsync);

        public ZLinkMeshNodeDescriptor? LocalDescriptor(RoutingId rid)
        {
            lock (_gate)
                return _descriptors.GetValueOrDefault(rid);
        }

        public IReadOnlyList<ZLinkMeshNodeDescriptor> Descriptors()
        {
            lock (_gate)
                return _descriptors.Values
                    .OrderBy(
                        static descriptor => descriptor.Rid.ToHex(),
                        StringComparer.Ordinal)
                    .ToArray();
        }

        public ZLinkObservationQueue<ZLinkRouteMeshStatus> Subscribe(int capacity)
        {
            var observer = new ZLinkObservationQueue<ZLinkRouteMeshStatus>(
                capacity,
                static status => status.Sequence,
                "route_mesh");
            var status = _owner.GetStatus(_meshName);
            lock (_gate)
            {
                _lastStatus = status;
                _observers.Add(observer);
            }
            observer.Publish(status, IsTerminalStatus(status));
            return observer;
        }

        public void Unsubscribe(
            ZLinkObservationQueue<ZLinkRouteMeshStatus> observer)
        {
            lock (_gate)
            {
                _observers.Remove(observer);
            }
            observer.Complete();
        }

        public void Stop()
        {
            ZLinkObservationQueue<ZLinkRouteMeshStatus>[] observers;
            ZLinkRouteMeshStatus? currentStatus;
            lock (_gate)
            {
                observers = [.. _observers];
                _observers.Clear();
                currentStatus = _lastStatus;
            }
            if (currentStatus is not null)
            {
                var terminalStatus = currentStatus with
                {
                    State = ZLinkTopologyState.Stopped,
                    IsReady = false,
                    Channels = currentStatus.Channels
                        .Select(static channel => channel with { IsReady = false })
                        .ToArray(),
                    Placement = currentStatus.Placement with
                    {
                        IsAvailable = false,
                        UnavailableReason = ZLinkTopologyReason.RuntimeNotReady
                    },
                    Sequence = _owner.NextSequence(_meshName),
                    ObservedAt = DateTimeOffset.UtcNow
                };
                foreach (var observer in observers)
                    observer.Publish(terminalStatus, terminal: true);
            }
            _stop.Cancel();
            _pump?.GetAwaiter().GetResult();
            _stop.Dispose();
        }

        public void PublishHostStateChanged(ZLinkFrameworkRuntimeState state)
        {
            _ = state;
            var projected = _owner.GetStatus(_meshName);
            ZLinkObservationQueue<ZLinkRouteMeshStatus>[] observers;
            lock (_gate)
            {
                _lastStatus = projected;
                observers = [.. _observers];
            }
            foreach (var observer in observers)
                observer.Publish(projected, IsTerminalStatus(projected));
        }

        private async Task PumpAsync()
        {
            try
            {
                while (!_stop.IsCancellationRequested)
                {
                    try
                    {
                        await PublishDescriptorChangesAsync(_stop.Token)
                            .ConfigureAwait(false);
                    }
                    catch (OperationCanceledException) when (_stop.IsCancellationRequested)
                    {
                        throw;
                    }
                    catch
                    {
                        // Location health owns store-failure reporting.
                        // Monitoring remains subscribed and retries the
                        // snapshot on the next polling interval.
                    }

                    var nativeEvent = _monitor.Recv(RecvFlags.DontWait);
                    if (nativeEvent is null)
                    {
                        await Task.Delay(MonitorIdleDelay, _stop.Token)
                            .ConfigureAwait(false);
                        continue;
                    }

                    var sourceRid = _nodeRuntime.Node.MeshStatus().RoutingId;
                    var mapped = _owner.MapMonitorEvents(
                        _meshName, sourceRid, nativeEvent);
                    foreach (var runtimeEvent in mapped)
                        Publish(runtimeEvent);
                }
            }
            catch (OperationCanceledException) when (_stop.IsCancellationRequested)
            {
            }
            catch (Exception)
            {
                lock (_gate)
                    _observers.Clear();
            }
            finally
            {
                _monitor.Dispose();
            }
        }

        private async ValueTask PublishDescriptorChangesAsync(
            CancellationToken cancellationToken)
        {
            PublishLocationHealthChange();
            if (_owner._locationQuery is null || DateTimeOffset.UtcNow < _nextDescriptorPoll)
                return;
            _nextDescriptorPoll = DateTimeOffset.UtcNow.AddMilliseconds(100);

            var current = await _owner._locationQuery.ListMeshNodeDescriptorsAsync(
                    _meshName,
                    default,
                    cancellationToken)
                .ConfigureAwait(false);
            if (_descriptors.Count == 0)
            {
                lock (_gate)
                {
                    foreach (var descriptor in current.Items)
                        _descriptors[descriptor.Rid] = descriptor;
                }
                var initialSourceRid =
                    _nodeRuntime.Node.MeshStatus().RoutingId;
                foreach (var descriptor in current.Items)
                {
                    if (descriptor.Rid == initialSourceRid)
                    {
                        PublishPlacementChange(initialSourceRid, descriptor);
                    }
                    else
                    {
                        PublishPeerDescriptorChange(
                            initialSourceRid,
                            descriptor,
                            "discovered");
                    }
                }
                return;
            }

            var sourceRid = _nodeRuntime.Node.MeshStatus().RoutingId;
            foreach (var descriptor in current.Items)
            {
                ZLinkMeshNodeDescriptor? previous;
                lock (_gate)
                    _descriptors.TryGetValue(descriptor.Rid, out previous);
                if (previous is null)
                {
                    lock (_gate)
                        _descriptors[descriptor.Rid] = descriptor;
                    if (descriptor.Rid != sourceRid)
                        PublishPeerDescriptorChange(sourceRid, descriptor, "discovered");
                    continue;
                }
                var capacityChanged =
                    !SameCapacity(previous.Capacity, descriptor.Capacity)
                    || previous.ActivationConcurrency
                        != descriptor.ActivationConcurrency;
                var placementChanged =
                    capacityChanged
                    || previous.PlacementWeight != descriptor.PlacementWeight
                    || previous.ObjectRole != descriptor.ObjectRole;
                lock (_gate)
                    _descriptors[descriptor.Rid] = descriptor;
                if (previous.ObjectRole != descriptor.ObjectRole)
                    PublishPeerDescriptorChange(sourceRid, descriptor, "role_changed");
                if (placementChanged && descriptor.Rid == sourceRid)
                    PublishPlacementChange(sourceRid, descriptor);
                if (previous.DescriptorRevision == descriptor.DescriptorRevision
                    && previous.ChannelWeights.Count == descriptor.ChannelWeights.Count
                    && previous.ChannelWeights.All(pair =>
                        descriptor.ChannelWeights.TryGetValue(pair.Key, out var weight)
                        && weight == pair.Value))
                    continue;

                foreach (var channelName in previous.ChannelWeights.Keys
                             .Concat(descriptor.ChannelWeights.Keys)
                             .Distinct(StringComparer.Ordinal))
                {
                    previous.ChannelWeights.TryGetValue(channelName, out var oldWeight);
                    descriptor.ChannelWeights.TryGetValue(channelName, out var newWeight);
                    if (oldWeight == newWeight)
                        continue;
                    Publish(_owner.Event(
                        "zlink.runtime.mesh_node.channel_changed",
                        _meshName,
                        sourceRid,
                        peerRid: descriptor.Rid,
                        descriptorRevision: descriptor.DescriptorRevision,
                        channelName: channelName,
                        reason: "admission_changed"));
                }
            }
            var currentIds = current.Items
                .Select(static descriptor => descriptor.Rid)
                .ToHashSet();
            RoutingId[] removedDescriptors;
            lock (_gate)
            {
                removedDescriptors = _descriptors.Keys
                    .Where(rid => !currentIds.Contains(rid))
                    .ToArray();
                foreach (var removed in removedDescriptors)
                    _descriptors.Remove(removed);
            }
            foreach (var removed in removedDescriptors)
            {
                if (removed != sourceRid)
                    Publish(_owner.Event(
                        "zlink.runtime.mesh_node.peer_changed",
                        _meshName,
                        sourceRid,
                        peerRid: removed,
                        reason: "removed"));
            }
        }

        private void PublishPeerDescriptorChange(
            RoutingId sourceRid,
            ZLinkMeshNodeDescriptor descriptor,
            string change)
        {
            var notRequired = ZLinkRouteMeshConnectionPolicy.IsNotRequired(
                _nodeRuntime.Registration.ObjectRole,
                _nodeRuntime.Registration.ChannelMemberships.Any(
                    static membership => membership.IsServer),
                descriptor.ObjectRole,
                descriptor.ChannelWeights.Count != 0);
            Publish(_owner.Event(
                "zlink.runtime.mesh_node.peer_changed",
                _meshName,
                sourceRid,
                peerRid: descriptor.Rid,
                descriptorRevision: descriptor.DescriptorRevision,
                reason: notRequired
                    ? $"not_required:{change}"
                    : $"not_connected:{change}"));
        }

        private void PublishPlacementChange(
            RoutingId sourceRid,
            ZLinkMeshNodeDescriptor descriptor) =>
            Publish(_owner.Event(
                "zlink.runtime.object.placement_changed",
                _meshName,
                sourceRid,
                peerRid: descriptor.Rid,
                descriptorRevision: descriptor.DescriptorRevision,
                placementOutcome: "updated",
                populationCapacity: descriptor.Capacity,
                activationConcurrency: descriptor.ActivationConcurrency));

        private static bool SameCapacity(
            ZLinkPlacementCapacity left,
            ZLinkPlacementCapacity right) =>
            left.Actors == right.Actors
            && left.Spots == right.Spots
            && left.SpotTypes.SequenceEqual(right.SpotTypes);

        private void PublishLocationHealthChange()
        {
            if (_owner._storeHealth is null)
                return;
            var state = _owner._storeHealth.GetSnapshot().Healthy ? "ready" : "degraded";
            if (_lastLocationState is null)
            {
                _lastLocationState = state;
                return;
            }
            if (string.Equals(_lastLocationState, state, StringComparison.Ordinal))
                return;
            _lastLocationState = state;
            var sourceRid = _nodeRuntime.Node.MeshStatus().RoutingId;
            Publish(_owner.Event(
                "zlink.runtime.location.store_changed",
                _meshName,
                sourceRid,
                reason: state));
        }

        private void Publish(ZLinkMeshRuntimeEvent runtimeEvent)
        {
            _ = runtimeEvent;
            var status = _owner.GetStatus(_meshName);
            ZLinkObservationQueue<ZLinkRouteMeshStatus>[] observers;
            lock (_gate)
            {
                _lastStatus = status;
                observers = [.. _observers];
            }
            foreach (var observer in observers)
                observer.Publish(status, IsTerminalStatus(status));
        }
        private static bool IsTerminalStatus(ZLinkRouteMeshStatus status) =>
            status.State is ZLinkTopologyState.Stopped
                or ZLinkTopologyState.Failed;
    }
}
