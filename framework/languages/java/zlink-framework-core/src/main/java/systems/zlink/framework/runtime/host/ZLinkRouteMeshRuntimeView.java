package systems.zlink.framework.runtime.host;

import systems.zlink.framework.channels.ZLinkMeshChannelRuntimeOptions;
import systems.zlink.framework.channels.ZLinkMeshPlacementRuntimeOptions;
import systems.zlink.framework.channels.ZLinkRouteMeshRuntimeOptions;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.locations.ZLinkCapacityUsage;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.monitoring.ZLinkMeshChannelSnapshot;
import systems.zlink.framework.monitoring.ZLinkMeshNodeSnapshot;
import systems.zlink.framework.monitoring.ZLinkMeshPeerSnapshot;
import systems.zlink.framework.monitoring.ZLinkObservedStatus;
import systems.zlink.framework.monitoring.ZLinkPeerState;
import systems.zlink.framework.monitoring.ZLinkPlacementSnapshot;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;
import systems.zlink.framework.monitoring.ZLinkTopologyReason;
import systems.zlink.framework.monitoring.ZLinkTopologyState;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeState;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerEntry;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerState;
import systems.zlink.framework.runtime.internal.binding.spot.PeerChannels;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkMeshNodeMonitoringProjection;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkTopologyRuntimeProjection;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkTopologyStatusSource;

import java.time.Instant;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.Flow;
import java.util.concurrent.TimeUnit;
import java.util.logging.Level;
import java.util.logging.Logger;

final class ZLinkRouteMeshRuntimeView
        implements ZLinkRouteMeshRuntime, ZLinkRouteMeshRuntimeOptions, AutoCloseable {
    private static final Logger LOGGER =
            Logger.getLogger(ZLinkRouteMeshRuntimeView.class.getName());
    private static final long LOCATION_HEALTH_QUERY_TIMEOUT_MILLIS = 500;
    private final ZLinkFrameworkRuntime runtime;
    private final ZLinkTopologyStatusSource<ZLinkMeshNodeSnapshot> statuses;
    private final ConcurrentHashMap<String, SignalHub> signalHubs = new ConcurrentHashMap<>();

    ZLinkRouteMeshRuntimeView(ZLinkFrameworkRuntime runtime) {
        this.runtime = runtime;
        statuses =
                new ZLinkTopologyStatusSource<>(
                        this::buildSnapshot,
                        status ->
                                List.of(
                                        status.state(),
                                        status.isReady(),
                                        status.readyPeerCount(),
                                        status.channels(),
                                        status.peers(),
                                        status.placement()),
                        (status, sequence) ->
                                new ZLinkMeshNodeSnapshot(
                                        status.meshName(),
                                        status.state(),
                                        status.isReady(),
                                        status.readyPeerCount(),
                                        status.channels(),
                                        status.peers(),
                                        status.placement(),
                                        sequence,
                                        status.observedAt()),
                        ZLinkMeshNodeSnapshot::sequence,
                        status ->
                                status.state() == ZLinkTopologyState.STOPPED
                                        || status.state() == ZLinkTopologyState.FAILED,
                        status -> status.state() == ZLinkTopologyState.STOPPING);
    }

    @Override
    public ZLinkMeshNodeSnapshot snapshot(String meshName) {
        return statuses.snapshot(meshName);
    }

    private ZLinkMeshNodeSnapshot buildSnapshot(String meshName, ZLinkMeshNodeSnapshot previous) {
        ZLinkFrameworkRuntimeState hostState = runtime.status().state();
        if (previous != null
                && (runtime.closing()
                        || hostState == ZLinkFrameworkRuntimeState.STOPPED
                        || hostState == ZLinkFrameworkRuntimeState.ERROR)) {
            return new ZLinkMeshNodeSnapshot(
                    meshName,
                    ZLinkTopologyRuntimeProjection.hostState(hostState),
                    false,
                    previous.readyPeerCount(),
                    previous.channels().stream()
                            .map(
                                    channel ->
                                            new ZLinkMeshChannelSnapshot(
                                                    channel.channelName(),
                                                    false,
                                                    channel.readyTargetCount()))
                            .toList(),
                    previous.peers(),
                    new ZLinkPlacementSnapshot(
                            false,
                            previous.placement().activeActorCount(),
                            previous.placement().activeSpotCount(),
                            Optional.of(placementUnavailableReason(hostState, true))),
                    0,
                    Instant.now());
        }
        ZLinkInternalMeshNode node = requireNode(meshName);
        var nativeStatus = node.status();
        List<MeshPeerEntry> nativePeers = List.copyOf(node.peers());
        ZLinkTopologyState state = topologyState(nativeStatus.state());
        boolean locationStoreHealthy = locationStoreHealthy();
        if (hostState != ZLinkFrameworkRuntimeState.SERVING) {
            state = ZLinkTopologyRuntimeProjection.hostState(hostState);
        }
        if (state == ZLinkTopologyState.READY && !locationStoreHealthy) {
            state = ZLinkTopologyState.DEGRADED;
        }
        ZLinkMeshNodeMonitoringProjection placement =
                runtime.monitoringMeshNodeProjection(meshName, nativeStatus.routingId());
        boolean placementAvailable =
                state == ZLinkTopologyState.READY
                        && placement.objectRole() == ZLinkMeshNodeObjectRole.SERVER
                        && placement.placementWeight() > 0
                        && hasActivationCapacity(placement)
                        && hasAvailableObjectCapacity(placement);
        if (state == ZLinkTopologyState.READY
                && nativePeers.stream()
                        .anyMatch(ZLinkRouteMeshRuntimeView::requiredPeerUnavailable)) {
            state = ZLinkTopologyState.DEGRADED;
        }
        List<ZLinkMeshChannelSnapshot> channels =
                runtime.monitoringMeshNodeChannelNames(meshName).stream()
                        .distinct()
                        .sorted()
                        .map(
                                channelName -> {
                                    long readyTargets =
                                            nativePeers.stream()
                                                    .filter(
                                                            peer ->
                                                                    peer.state()
                                                                            == systems.zlink
                                                                                    .framework
                                                                                    .runtime
                                                                                    .internal
                                                                                    .binding.spot
                                                                                    .MeshPeerState
                                                                                    .ADMITTED)
                                                    .map(peer -> peerChannels(node, peer))
                                                    .filter(
                                                            peerChannels -> {
                                                                int index =
                                                                        peerChannels
                                                                                .names()
                                                                                .indexOf(
                                                                                        channelName);
                                                                return index >= 0
                                                                        && peerChannels
                                                                                        .weights()
                                                                                        .get(index)
                                                                                > 0;
                                                            })
                                                    .count();
                                    return new ZLinkMeshChannelSnapshot(
                                            channelName,
                                            hostState == ZLinkFrameworkRuntimeState.SERVING
                                                    && readyTargets > 0,
                                            Math.toIntExact(readyTargets));
                                })
                        .toList();
        return new ZLinkMeshNodeSnapshot(
                meshName,
                state,
                state == ZLinkTopologyState.READY,
                Math.toIntExact(
                        nativePeers.stream()
                                .filter(
                                        peer ->
                                                peer.state()
                                                        == systems.zlink.framework.runtime.internal
                                                                .binding.spot.MeshPeerState
                                                                .ADMITTED)
                                .count()),
                channels,
                nativePeers.stream().map(ZLinkRouteMeshRuntimeView::peer).toList(),
                new ZLinkPlacementSnapshot(
                        placementAvailable,
                        runtime.activeActorCount(meshName),
                        runtime.activeSpotCount(meshName),
                        placementAvailable
                                ? Optional.empty()
                                : Optional.of(
                                        placementUnavailableReason(
                                                hostState, locationStoreHealthy))),
                0,
                Instant.now());
    }

    @Override
    public Flow.Publisher<ZLinkObservedStatus<ZLinkMeshNodeSnapshot>> observe(
            String meshName, int capacity) {
        var publisher = statuses.observe(meshName, capacity);
        if (statuses.isTerminal(meshName) || runtime.closing()) {
            return publisher;
        }
        signalHubs.compute(
                meshName,
                (ignored, existing) ->
                        existing == null
                                ? new SignalHub(meshName, requireNode(meshName))
                                : existing);
        return publisher;
    }

    @Override
    public boolean isReady(String meshName) {
        return snapshot(meshName).isReady();
    }

    @Override
    public ZLinkMeshChannelRuntimeOptions channel(String meshName, String channelName) {
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
        statuses.signalAll();
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
        ZLinkInternalMeshNode node = runtime.monitoringMeshNode(meshName);
        if (node == null) {
            throw new ZLinkConfigurationException("RouteMesh is not configured: " + meshName);
        }
        return node;
    }

    private static ZLinkTopologyReason placementUnavailableReason(
            ZLinkFrameworkRuntimeState hostState, boolean locationStoreHealthy) {
        return switch (hostState) {
            case RELOCATING, RELOCATED, DRAINING -> ZLinkTopologyReason.DRAINING;
            case PREPARING, STOPPED, ERROR -> ZLinkTopologyReason.RUNTIME_NOT_READY;
            case SERVING ->
                    locationStoreHealthy
                            ? ZLinkTopologyReason.CAPACITY_EXCEEDED
                            : ZLinkTopologyReason.LOCATION_UNAVAILABLE;
        };
    }

    private boolean locationStoreHealthy() {
        try {
            return runtime.monitoringLocationRuntimeQuery()
                    .getStatus()
                    .toCompletableFuture()
                    .orTimeout(LOCATION_HEALTH_QUERY_TIMEOUT_MILLIS, TimeUnit.MILLISECONDS)
                    .join()
                    .storeHealthy();
        } catch (ZLinkConfigurationException notConfigured) {
            return true;
        } catch (RuntimeException unavailable) {
            return false;
        }
    }

    private static ZLinkMeshPeerSnapshot peer(MeshPeerEntry peer) {
        ZLinkPeerState state =
                switch (peer.state()) {
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

    private static PeerChannels peerChannels(ZLinkInternalMeshNode node, MeshPeerEntry peer) {
        try {
            return node.peerChannels(peer.routingId(), peer.lifecycleGeneration());
        } catch (RuntimeException ignored) {
            return new PeerChannels(List.of(), List.of());
        }
    }

    private static boolean requiredPeerUnavailable(MeshPeerEntry peer) {
        return peer.state() != MeshPeerState.ADMITTED
                && peer.state() != MeshPeerState.DRAINING
                && peer.state() != MeshPeerState.NOT_REQUIRED;
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

    private static boolean hasAvailableObjectCapacity(ZLinkMeshNodeMonitoringProjection placement) {
        return hasRemainingCapacity(placement.objectCapacity().actors())
                || hasRemainingCapacity(placement.objectCapacity().spots());
    }

    private static boolean hasRemainingCapacity(ZLinkCapacityUsage capacity) {
        return capacity.limit() == 0
                || (long) capacity.active() + capacity.reserved() < capacity.limit();
    }

    private static boolean hasActivationCapacity(ZLinkMeshNodeMonitoringProjection placement) {
        int limit = placement.activationConcurrency().limit();
        return limit == 0 || placement.activationConcurrency().active() < limit;
    }

    private final class SignalHub implements AutoCloseable {
        private final String meshName;
        private final ZLinkInternalMeshNode node;
        private final ZLinkStateLane stateLane = new ZLinkStateLane();
        private boolean stopped;
        private boolean subscriptionStarted;
        private AutoCloseable subscription;

        SignalHub(String meshName, ZLinkInternalMeshNode node) {
            this.meshName = meshName;
            this.node = node;
            statuses.onActiveSubscriptions(
                    meshName,
                    active -> {
                        if (active) register();
                    });
        }

        void register() {
            stateLane
                    .runAsync(
                            () -> {
                                if (stopped || subscriptionStarted) return false;
                                subscriptionStarted = true;
                                return true;
                            })
                    .thenAccept(
                            start -> {
                                if (!start) return;
                                AutoCloseable created;
                                try {
                                    created = node.onStateChanged(this::signal);
                                } catch (RuntimeException failure) {
                                    registrationFailed(failure);
                                    return;
                                }
                                stateLane
                                        .runAsync(
                                                () -> {
                                                    if (stopped) return false;
                                                    subscription = created;
                                                    return true;
                                                })
                                        .whenComplete(
                                                (retained, failure) -> {
                                                    if (failure != null
                                                            || !Boolean.TRUE.equals(retained))
                                                        closeSubscription(created);
                                                    if (failure != null)
                                                        registrationFailed(failure);
                                                    else if (retained) signal();
                                                });
                            })
                    .exceptionally(
                            failure -> {
                                registrationFailed(failure);
                                return null;
                            });
        }

        private void registrationFailed(Throwable failure) {
            stateLane
                    .runAsync(
                            () -> {
                                subscriptionStarted = false;
                                return !stopped;
                            })
                    .whenComplete(
                            (active, ownerFailure) -> {
                                if (Boolean.TRUE.equals(active)) statuses.fail(meshName, failure);
                                LOGGER.log(
                                        Level.WARNING,
                                        "Mesh status source registration failed",
                                        failure);
                                if (ownerFailure != null)
                                    LOGGER.log(
                                            Level.WARNING,
                                            "Mesh status source failure delivery failed",
                                            ownerFailure);
                            });
        }

        void signal() {
            statuses.signal(meshName);
        }

        @Override
        public void close() {
            stateLane
                    .runAsync(
                            () -> {
                                if (stopped) return null;
                                stopped = true;
                                AutoCloseable active = subscription;
                                subscription = null;
                                return active;
                            })
                    .whenComplete(
                            (current, failure) -> {
                                closeSubscription(current);
                                if (failure != null)
                                    LOGGER.log(
                                            Level.WARNING,
                                            "Mesh status source close failed",
                                            failure);
                            });
        }

        private void closeSubscription(AutoCloseable current) {
            if (current == null) return;
            try {
                current.close();
            } catch (Exception failure) {
                LOGGER.log(Level.WARNING, "Mesh status subscription cleanup failed", failure);
            }
        }
    }
}
