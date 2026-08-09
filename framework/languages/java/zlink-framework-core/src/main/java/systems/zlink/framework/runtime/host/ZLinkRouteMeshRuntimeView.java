package systems.zlink.framework.runtime.host;
import java.util.concurrent.TimeUnit;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerState;

import java.time.Instant;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Objects;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.Flow;
import java.lang.ref.WeakReference;
import java.util.concurrent.locks.LockSupport;
import java.util.concurrent.atomic.AtomicLong;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.locations.ZLinkCapacityUsage;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.monitoring.ZLinkMeshChannelSnapshot;
import systems.zlink.framework.monitoring.ZLinkMeshNodeSnapshot;
import systems.zlink.framework.monitoring.ZLinkObservedStatus;
import systems.zlink.framework.monitoring.ZLinkMeshPeerSnapshot;
import systems.zlink.framework.monitoring.ZLinkPeerState;
import systems.zlink.framework.monitoring.ZLinkPlacementSnapshot;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;
import systems.zlink.framework.monitoring.ZLinkTopologyReason;
import systems.zlink.framework.monitoring.ZLinkTopologyState;
import systems.zlink.framework.channels.ZLinkMeshChannelRuntimeOptions;
import systems.zlink.framework.channels.ZLinkMeshPlacementRuntimeOptions;
import systems.zlink.framework.channels.ZLinkRouteMeshRuntimeOptions;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeMonitor;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeState;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeStatus;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerEntry;
import systems.zlink.framework.runtime.internal.binding.spot.PeerChannels;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkMeshNodeMonitoringProjection;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkStatusPublisher;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.core.RoutingId;

final class ZLinkRouteMeshRuntimeView
    implements ZLinkRouteMeshRuntime, ZLinkRouteMeshRuntimeOptions, AutoCloseable {
    private static final long MONITOR_IDLE_NANOS = 10_000_000L;
    private final ZLinkFrameworkRuntime runtime;
    private final AtomicLong sequence = new AtomicLong();
    private final ConcurrentHashMap<String, SignalHub> signalHubs =
        new ConcurrentHashMap<>();

    ZLinkRouteMeshRuntimeView(ZLinkFrameworkRuntime runtime) {
        this.runtime = runtime;
    }

    @Override
    public ZLinkMeshNodeSnapshot snapshot(String meshName) {
        ZLinkInternalMeshNode node = requireNode(meshName);
        var nativeStatus = node.status();
        List<MeshPeerEntry> nativePeers = List.copyOf(node.peers());
        ZLinkTopologyState state = topologyState(nativeStatus.state());
        ZLinkFrameworkRuntimeState hostState = runtime.status().state();
        boolean locationStoreHealthy = locationStoreHealthy();
        if (hostState != ZLinkFrameworkRuntimeState.SERVING) {
            state = switch (hostState) {
                case PREPARING -> ZLinkTopologyState.STARTING;
                case SERVING -> state;
                case RELOCATING, RELOCATED, DRAINING ->
                    ZLinkTopologyState.STOPPING;
                case STOPPED -> ZLinkTopologyState.STOPPED;
                case ERROR -> ZLinkTopologyState.FAILED;
            };
        }
        if (state == ZLinkTopologyState.READY && !locationStoreHealthy) {
            state = ZLinkTopologyState.DEGRADED;
        }
        if (state == ZLinkTopologyState.READY
            && nativePeers.stream().anyMatch(
                ZLinkRouteMeshRuntimeView::requiredPeerUnavailable)) {
            state = ZLinkTopologyState.DEGRADED;
        }
        ZLinkMeshNodeMonitoringProjection placement =
            runtime.monitoringMeshNodeProjection(
                meshName, nativeStatus.routingId());
        List<ZLinkMeshChannelSnapshot> channels =
            runtime.monitoringMeshNodeChannelNames(meshName).stream()
                .distinct()
                .sorted()
                .map(channelName -> {
                    long readyTargets = nativePeers.stream()
                        .filter(peer -> peer.state()
                            == systems.zlink.framework.runtime.internal.binding.spot
                                .MeshPeerState.ADMITTED)
                        .map(peer -> peerChannels(node, peer))
                        .filter(peerChannels -> {
                            int index = peerChannels.names().indexOf(channelName);
                            return index >= 0
                                && peerChannels.weights().get(index) > 0;
                        })
                        .count();
                    return new ZLinkMeshChannelSnapshot(
                        channelName,
                        readyTargets > 0,
                        Math.toIntExact(readyTargets));
                })
                .toList();
        boolean hasAdmittedPeer = nativePeers.stream().anyMatch(peer ->
            peer.state()
                == MeshPeerState.ADMITTED);
        if (state == ZLinkTopologyState.READY
            && hasAdmittedPeer
            && channels.stream().anyMatch(channel -> !channel.isReady())) {
            state = ZLinkTopologyState.DEGRADED;
        }
        boolean placementAvailable =
            state == ZLinkTopologyState.READY
                && placement.objectRole() == ZLinkMeshNodeObjectRole.SERVER
                && placement.placementWeight() > 0
                && hasActivationCapacity(placement)
                && hasAvailableObjectCapacity(placement);
        return new ZLinkMeshNodeSnapshot(
            meshName,
            state,
            state == ZLinkTopologyState.READY,
            Math.toIntExact(nativePeers.stream()
                .filter(peer -> peer.state()
                    == systems.zlink.framework.runtime.internal.binding.spot
                        .MeshPeerState.ADMITTED)
                .count()),
            channels,
            nativePeers.stream().map(ZLinkRouteMeshRuntimeView::peer).toList(),
            new ZLinkPlacementSnapshot(
                placementAvailable,
                runtime.activeActorCount(),
                runtime.activeSpotCount(),
                placementAvailable
                    ? Optional.empty()
                    : Optional.of(
                        state == ZLinkTopologyState.STOPPING
                            ? ZLinkTopologyReason.DRAINING
                            : !locationStoreHealthy
                                ? ZLinkTopologyReason.LOCATION_UNAVAILABLE
                            : state != ZLinkTopologyState.READY
                                ? ZLinkTopologyReason.RUNTIME_NOT_READY
                                : ZLinkTopologyReason.CAPACITY_EXCEEDED)),
            sequence.get(),
            Instant.now());
    }

    @Override
    public Flow.Publisher<ZLinkObservedStatus<ZLinkMeshNodeSnapshot>> observe(
        String meshName,
        int capacity) {
        requireNode(meshName);
        ZLinkStatusPublisher<ZLinkMeshNodeSnapshot> publisher =
            ZLinkStatusPublisher.create(
                () -> snapshot(meshName),
                status -> List.of(
                    status.state(),
                    status.isReady(),
                    status.readyPeerCount(),
                    status.channels(),
                    status.peers(),
                    status.placement()),
                capacity,
                status -> status.state() == ZLinkTopologyState.STOPPED
                    || status.state() == ZLinkTopologyState.FAILED,
                status -> status.state() == ZLinkTopologyState.STOPPING);
        // The hub only holds the publisher weakly so that a publisher nobody
        // subscribes to stays collectable. A subscriber that drops its
        // Subscription is the natural call shape, so the retention below is
        // what keeps a live subscription's publisher reachable. It is released
        // again when the last subscription is cancelled or fails.
        PublisherSignal signal = new PublisherSignal(publisher);
        publisher.onActiveSubscriptions(active ->
            signal.retain(active ? publisher : null));
        signalHubs.compute(meshName, (ignored, existing) ->
            existing == null || existing.isStopped()
                ? new SignalHub(requireNode(meshName))
                : existing).register(signal);
        return publisher;
    }

    @Override
    public boolean isReady(String meshName) {
        return snapshot(meshName).isReady();
    }

    @Override
    public ZLinkMeshChannelRuntimeOptions channel(
        String meshName,
        String channelName) {
        return options().channel(meshName, channelName);
    }

    @Override
    public ZLinkMeshPlacementRuntimeOptions mesh(String meshName) {
        return options().mesh(meshName);
    }

    @Override
    public ZLinkMeshChannelRuntimeOptions channel(String channelName) {
        return options().channel(channelName);
    }

    private ZLinkRouteMeshRuntimeOptions options() {
        return runtime.routeMeshRuntimeOptionsInternal();
    }

    void signalAll() {
        signalHubs.values().forEach(SignalHub::signal);
    }

    @Override
    public void close() {
        signalHubs.values().forEach(SignalHub::close);
        signalHubs.clear();
    }

    private ZLinkInternalMeshNode requireNode(String meshName) {
        if (meshName == null || meshName.isBlank()) {
            throw new IllegalArgumentException("meshName is required");
        }
        ZLinkInternalMeshNode node =
            runtime.monitoringMeshNode(meshName);
        if (node == null) {
            throw new ZLinkConfigurationException(
                "RouteMesh is not configured: " + meshName);
        }
        return node;
    }

    private boolean locationStoreHealthy() {
        try {
            return runtime.monitoringLocationRuntimeQuery()
                .getStatus()
                .toCompletableFuture()
                .orTimeout(500, TimeUnit.MILLISECONDS)
                .join()
                .storeHealthy();
        } catch (ZLinkConfigurationException notConfigured) {
            return true;
        } catch (RuntimeException unavailable) {
            return false;
        }
    }

    private static ZLinkMeshPeerSnapshot peer(MeshPeerEntry peer) {
        ZLinkPeerState state = switch (peer.state()) {
            case CONNECTING -> ZLinkPeerState.CONNECTING;
            case ADMITTED -> ZLinkPeerState.READY;
            case DRAINING -> ZLinkPeerState.DRAINING;
            case NOT_REQUIRED -> ZLinkPeerState.NOT_REQUIRED;
            default -> ZLinkPeerState.NOT_CONNECTED;
        };
        return new ZLinkMeshPeerSnapshot(
            peer.routingId(),
            state,
            state == ZLinkPeerState.READY || state == ZLinkPeerState.NOT_REQUIRED
                ? Optional.empty()
                : Optional.of(
                    state == ZLinkPeerState.DRAINING
                        ? ZLinkTopologyReason.DRAINING
                        : ZLinkTopologyReason.NO_READY_PEER));
    }

    private static PeerChannels peerChannels(
        ZLinkInternalMeshNode node,
        MeshPeerEntry peer) {
        try {
            return node.peerChannels(
                peer.routingId(), peer.lifecycleGeneration());
        } catch (RuntimeException ignored) {
            return new PeerChannels(List.of(), List.of());
        }
    }

    private static boolean requiredPeerUnavailable(MeshPeerEntry peer) {
        return peer.state()
            != systems.zlink.framework.runtime.internal.binding.spot
                .MeshPeerState.ADMITTED
            && peer.state()
                != systems.zlink.framework.runtime.internal.binding.spot
                    .MeshPeerState.NOT_REQUIRED;
    }

    private static ZLinkTopologyState topologyState(MeshNodeState state) {
        return switch (state) {
            case CREATED -> ZLinkTopologyState.STARTING;
            case STARTED, PARTIAL_READY, READY -> ZLinkTopologyState.READY;
            case DRAINING -> ZLinkTopologyState.STOPPING;
            case STOPPED -> ZLinkTopologyState.STOPPED;
            case ERROR -> ZLinkTopologyState.FAILED;
        };
    }

    private static boolean hasAvailableObjectCapacity(
        ZLinkMeshNodeMonitoringProjection placement) {
        boolean actorAvailable = placement.objectCapabilities().stream()
            .anyMatch(capability ->
                capability.objectKind() == ZLinkPlacementObjectKind.ACTOR)
            && hasRemainingCapacity(placement.objectCapacity().actors());
        boolean spotAvailable = placement.objectCapabilities().stream()
            .anyMatch(capability ->
                capability.objectKind() != ZLinkPlacementObjectKind.ACTOR)
            && hasRemainingCapacity(placement.objectCapacity().spots())
            && (placement.objectCapacity().spotTypes().isEmpty()
                || placement.objectCapacity().spotTypes().stream()
                    .anyMatch(type -> hasRemainingCapacity(type.usage())));
        return actorAvailable && spotAvailable;
    }

    private static boolean hasRemainingCapacity(ZLinkCapacityUsage capacity) {
        return capacity.limit() == 0
            || (long) capacity.active() + capacity.reserved() < capacity.limit();
    }

    private static boolean hasActivationCapacity(
        ZLinkMeshNodeMonitoringProjection placement) {
        int limit = placement.activationConcurrency().limit();
        return limit == 0
            || placement.activationConcurrency().active() < limit;
    }

    /**
     * One hub registration. The weak reference lets an unsubscribed publisher
     * be collected; {@code retained} keeps a subscribed one reachable.
     */
    private static final class PublisherSignal {
        private final WeakReference<ZLinkStatusPublisher<ZLinkMeshNodeSnapshot>>
            reference;
        /** Read by nothing on purpose: this field is the reachability root. */
        @SuppressWarnings("unused")
        private volatile ZLinkStatusPublisher<ZLinkMeshNodeSnapshot> retained;

        PublisherSignal(ZLinkStatusPublisher<ZLinkMeshNodeSnapshot> publisher) {
            this.reference = new WeakReference<>(publisher);
        }

        void retain(ZLinkStatusPublisher<ZLinkMeshNodeSnapshot> publisher) {
            retained = publisher;
        }

        boolean isCollected() {
            return reference.refersTo(null);
        }

        void signal() {
            ZLinkStatusPublisher<ZLinkMeshNodeSnapshot> current =
                reference.get();
            if (current != null) {
                current.signal();
            }
        }
    }

    private final class SignalHub implements AutoCloseable {
        private final ZLinkInternalMeshNode node;
        private final Object gate = new Object();
        private final List<PublisherSignal> signals = new ArrayList<>();
        private volatile boolean stopped;
        private Thread pump;

        SignalHub(ZLinkInternalMeshNode node) {
            this.node = node;
        }

        boolean isStopped() {
            return stopped;
        }

        void register(PublisherSignal signal) {
            Objects.requireNonNull(signal, "signal");
            synchronized (gate) {
                if (stopped) {
                    return;
                }
                signals.removeIf(PublisherSignal::isCollected);
                signals.add(signal);
                signal.signal();
                if (pump == null) {
                    pump = Thread.ofVirtual()
                        .name("zlink-mesh-status-monitor")
                        .start(this::pump);
                }
            }
        }

        void signal() {
            sequence.incrementAndGet();
            PublisherSignal[] current;
            synchronized (gate) {
                signals.removeIf(PublisherSignal::isCollected);
                current = signals.toArray(PublisherSignal[]::new);
            }
            for (PublisherSignal signal : current) {
                signal.signal();
            }
        }

        private void pump() {
            MeshNodeMonitor monitor = null;
            try {
                try {
                    monitor = node.openMonitor();
                } catch (UnsupportedOperationException unavailable) {
                    // Alternate backends may not expose a monitor. The
                    // initial snapshot remains available in that case.
                }
                // The Java binding currently exposes a nonblocking snapshot
                // adapter. This is one runtime-owned source probe for the hub;
                // subscribers receive hub signals and never poll the source.
                SourceSnapshot previous = sourceSnapshot();
                while (!stopped) {
                    boolean monitorChanged = monitor != null
                        && monitor.recv(RecvFlags.DONT_WAIT) != null;
                    SourceSnapshot current = sourceSnapshot();
                    if (monitorChanged || !Objects.equals(previous, current)) {
                        signal();
                    }
                    previous = current;
                    LockSupport.parkNanos(MONITOR_IDLE_NANOS);
                }
            } catch (RuntimeException ignored) {
                // Monitoring must not terminate the application runtime or a
                // subscriber dispatcher when the backend monitor closes.
            } finally {
                if (monitor != null) {
                    monitor.close();
                }
            }
        }

        private SourceSnapshot sourceSnapshot() {
            MeshNodeStatus status = node.status();
            List<MeshPeerEntry> peers = List.copyOf(node.peers());
            Map<RoutingId, PeerChannels> peerChannels = new HashMap<>();
            for (MeshPeerEntry peer : peers) {
                peerChannels.put(
                    peer.routingId(),
                    peerChannels(node, peer));
            }
            return new SourceSnapshot(
                status,
                peers,
                Map.copyOf(peerChannels),
                Map.copyOf(node.channelWeights()),
                runtime.activeActorCount(),
                runtime.activeSpotCount());
        }

        private record SourceSnapshot(
            MeshNodeStatus status,
            List<MeshPeerEntry> peers,
            Map<RoutingId, PeerChannels> peerChannels,
            Map<String, Integer> channelWeights,
            int activeActorCount,
            int activeSpotCount) {
        }

        @Override
        public void close() {
            Thread current;
            synchronized (gate) {
                if (stopped) {
                    return;
                }
                stopped = true;
                current = pump;
                signals.clear();
            }
            if (current != null) {
                current.interrupt();
            }
        }
    }
}
