package systems.zlink.framework.spring.internal.runtime;
import java.util.Objects;
import systems.zlink.framework.monitoring.ZLinkObservedStatus;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeMonitor;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerSource;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerState;

import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;

import java.time.Instant;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Queue;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.Flow;
import java.util.concurrent.ForkJoinPool;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.locks.LockSupport;
import java.util.function.Supplier;
import java.util.function.BiFunction;
import java.util.function.Function;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.binding.spot.MeshMonitorEvent;
import systems.zlink.framework.runtime.internal.binding.spot.MeshMonitorEventKind;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeState;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerEntry;
import systems.zlink.framework.runtime.internal.binding.spot.PeerChannels;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.locations.ZLinkActivationConcurrency;
import systems.zlink.framework.monitoring.ZLinkMeshChannelSnapshot;
import systems.zlink.framework.monitoring.ZLinkMeshNodeSnapshot;
import systems.zlink.framework.monitoring.ZLinkMeshPeerSnapshot;
import systems.zlink.framework.monitoring.ZLinkPeerState;
import systems.zlink.framework.monitoring.ZLinkPlacementSnapshot;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;
import systems.zlink.framework.monitoring.ZLinkTopologyReason;
import systems.zlink.framework.monitoring.ZLinkTopologyState;
import systems.zlink.framework.locations.ZLinkLocationRuntimeQuery;
import systems.zlink.framework.locations.ZLinkCapacityUsage;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.locations.ZLinkPlacementCapacity;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkMeshNodeMonitoringProjection;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkStatusPublisher;

final class ZLinkRouteMeshRuntimeService implements ZLinkRouteMeshRuntime, AutoCloseable {
    private static final long MONITOR_IDLE_NANOS = 10_000_000L;
    private static final long DESCRIPTOR_POLL_NANOS = 100_000_000L;

    private final Supplier<Map<String, ZLinkInternalMeshNode>> nodes;
    private final Supplier<ZLinkLocationRuntimeQuery> locationRuntime;
    private final BiFunction<String, RoutingId, ZLinkMeshNodeMonitoringProjection>
        placementProjection;
    private final Function<String, List<String>> channelNames;
    private final Map<String, AtomicLong> sequences = new ConcurrentHashMap<>();
    private final Map<String, MonitorHub> monitorHubs = new ConcurrentHashMap<>();
    private final AtomicReference<Instant> lastLocationFailure = new AtomicReference<>();
    private volatile boolean locationHealthy = true;

    public ZLinkRouteMeshRuntimeService(ZLinkFrameworkLifecycle lifecycle) {
        this(
            lifecycle::monitoringMeshNodes,
            lifecycle::monitoringLocationRuntimeQuery,
            lifecycle::monitoringMeshNodeProjection,
            lifecycle::monitoringMeshNodeChannelNames);
    }

    ZLinkRouteMeshRuntimeService(
        Supplier<Map<String, ZLinkInternalMeshNode>> nodes,
        Supplier<ZLinkLocationRuntimeQuery> locationRuntime) {
        this(
            nodes,
            locationRuntime,
            (meshName, rid) -> defaultPlacement(nodes.get().get(meshName)),
            meshName -> List.copyOf(
                nodes.get().get(meshName).channelWeights().keySet()));
    }

    ZLinkRouteMeshRuntimeService(
        Supplier<Map<String, ZLinkInternalMeshNode>> nodes,
        Supplier<ZLinkLocationRuntimeQuery> locationRuntime,
        BiFunction<String, RoutingId, ZLinkMeshNodeMonitoringProjection> placementProjection) {
        this(
            nodes,
            locationRuntime,
            placementProjection,
            meshName -> List.copyOf(
                nodes.get().get(meshName).channelWeights().keySet()));
    }

    ZLinkRouteMeshRuntimeService(
        Supplier<Map<String, ZLinkInternalMeshNode>> nodes,
        Supplier<ZLinkLocationRuntimeQuery> locationRuntime,
        BiFunction<String, RoutingId, ZLinkMeshNodeMonitoringProjection> placementProjection,
        Function<String, List<String>> channelNames) {
        this.nodes = Objects.requireNonNull(nodes, "nodes");
        this.locationRuntime =
            Objects.requireNonNull(locationRuntime, "locationRuntime");
        this.placementProjection = Objects.requireNonNull(
            placementProjection,
            "placementProjection");
        this.channelNames = Objects.requireNonNull(
            channelNames,
            "channelNames");
    }

    @Override
    public ZLinkMeshNodeSnapshot snapshot(String meshName) {
        ZLinkInternalMeshNode node = requireNode(meshName);
        var status = node.status();
        List<MeshPeerEntry> peers = node.peers();
        ZLinkTopologyState state = mapTopologyState(status.state());
        LocationStatusSnapshot location = locationSnapshot();
        boolean localPlacementReady =
            state == ZLinkTopologyState.READY
                && !"degraded".equals(location.state());
        if (state == ZLinkTopologyState.READY
            && "degraded".equals(location.state())) {
            state = ZLinkTopologyState.DEGRADED;
        }
        if (state == ZLinkTopologyState.READY
            && peers.stream().anyMatch(ZLinkRouteMeshRuntimeService::isRequiredPeerUnavailable)) {
            state = ZLinkTopologyState.DEGRADED;
        }
        ZLinkMeshNodeMonitoringProjection placement =
            placementProjection.apply(meshName, status.routingId());
        List<ZLinkMeshChannelSnapshot> channels = channelNames.apply(meshName).stream()
            .distinct()
            .sorted()
            .map(channelName -> {
                long readyMembers = peers.stream()
                    .filter(peer -> peer.state()
                        == MeshPeerState.ADMITTED)
                    .map(peer -> peerChannels(node, peer))
                    .filter(peerChannels -> {
                        int index = peerChannels.names().indexOf(channelName);
                        return index >= 0 && peerChannels.weights().get(index) > 0;
                    })
                    .count();
                return new ZLinkMeshChannelSnapshot(
                    channelName, readyMembers > 0, Math.toIntExact(readyMembers));
                })
            .toList();
        boolean hasAdmittedPeer = peers.stream().anyMatch(peer ->
            peer.state()
                == MeshPeerState.ADMITTED);
        if (state == ZLinkTopologyState.READY
            && hasAdmittedPeer
            && channels.stream().anyMatch(channel -> !channel.isReady())) {
            state = ZLinkTopologyState.DEGRADED;
        }
        boolean placementAvailable =
            placement.objectRole() == ZLinkMeshNodeObjectRole.SERVER
                && localPlacementReady
                && placement.placementWeight() > 0
                && hasActivationCapacity(placement)
                && hasAvailableObjectCapacity(placement);
        return new ZLinkMeshNodeSnapshot(
            meshName,
            state,
            state == ZLinkTopologyState.READY,
            Math.toIntExact(peers.stream()
                .filter(peer -> peer.state()
                    == MeshPeerState.ADMITTED)
                .count()),
            channels,
            peers.stream().map(peer -> mapPeer(node, peer)).toList(),
            new ZLinkPlacementSnapshot(
                placementAvailable,
                Math.toIntExact(placement.objectCapacity().actors().active()),
                Math.toIntExact(placement.objectCapacity().spots().active()),
                placementAvailable
                    ? Optional.empty()
                    : Optional.of("degraded".equals(location.state())
                        ? ZLinkTopologyReason.LOCATION_UNAVAILABLE
                        : localPlacementReady
                            ? ZLinkTopologyReason.CAPACITY_EXCEEDED
                            : ZLinkTopologyReason.RUNTIME_NOT_READY)),
            nextSequence(meshName),
            Instant.now());
    }

    @Override
    public Flow.Publisher<ZLinkObservedStatus<
        ZLinkMeshNodeSnapshot>> observe(String meshName, int capacity) {
        if (capacity <= 0) {
            throw new IllegalArgumentException("capacity must be positive");
        }
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
        MonitorHub hub = monitorHubs.computeIfAbsent(
            meshName, ignored -> new MonitorHub(meshName, requireNode(meshName)));
        hub.registerSignal(publisher::signal);
        return publisher;
    }

    @Override
    public boolean isReady(String meshName) {
        return snapshot(meshName).isReady();
    }

    @Override
    public void close() {
        new ArrayList<>(monitorHubs.values()).forEach(MonitorHub::close);
        monitorHubs.clear();
    }

    private ZLinkInternalMeshNode requireNode(String meshName) {
        if (meshName == null || meshName.isBlank()) {
            throw new IllegalArgumentException("meshName is required");
        }
        ZLinkInternalMeshNode node = nodes.get().get(meshName);
        if (node == null) {
            throw new ZLinkConfigurationException(
                "RouteMesh is not configured: " + meshName);
        }
        return node;
    }

    private LocationStatusSnapshot locationSnapshot() {
        try {
            var status = locationRuntime.get()
                .getStatus()
                .toCompletableFuture()
                .join();
            if (!status.storeHealthy() && locationHealthy) {
                Instant failureAt = Instant.now();
                if (status.lastRefreshAt() != null
                    && !failureAt.isAfter(status.lastRefreshAt())) {
                    failureAt = status.lastRefreshAt().plusNanos(1);
                }
                lastLocationFailure.set(failureAt);
            }
            locationHealthy = status.storeHealthy();
            Instant lastSuccessAt = status.lastRefreshAt();
            Instant failureAt = lastLocationFailure.get();
            if (status.storeHealthy()
                && failureAt != null
                && (lastSuccessAt == null || !lastSuccessAt.isAfter(failureAt))) {
                lastSuccessAt = failureAt.plusNanos(1);
            }
            return new LocationStatusSnapshot(
                status.storeHealthy() ? "ready" : "degraded",
                Optional.ofNullable(lastSuccessAt),
                Optional.ofNullable(failureAt));
        } catch (RuntimeException ignored) {
            return new LocationStatusSnapshot(
                "not_configured", Optional.empty(), Optional.empty());
        }
    }

    private long nextSequence(String meshName) {
        return sequences.computeIfAbsent(meshName, ignored -> new AtomicLong())
            .incrementAndGet();
    }

    private static ZLinkMeshPeerSnapshot mapPeer(
        ZLinkInternalMeshNode node,
        MeshPeerEntry peer) {
        ZLinkPeerState state;
        Optional<ZLinkTopologyReason> unavailableReason;
        switch (peer.state()) {
            case CONFIGURED -> {
                state = ZLinkPeerState.NOT_CONNECTED;
                unavailableReason = Optional.of(ZLinkTopologyReason.NO_READY_PEER);
            }
            case CONNECTING -> {
                state = ZLinkPeerState.CONNECTING;
                unavailableReason = Optional.of(ZLinkTopologyReason.NO_READY_PEER);
            }
            case ADMITTED -> {
                state = ZLinkPeerState.READY;
                unavailableReason = Optional.empty();
            }
            case DRAINING -> {
                state = ZLinkPeerState.DRAINING;
                unavailableReason = Optional.of(ZLinkTopologyReason.DRAINING);
            }
            case CLOSED -> {
                state = ZLinkPeerState.NOT_CONNECTED;
                unavailableReason = Optional.of(ZLinkTopologyReason.NO_READY_PEER);
            }
            default -> {
                state = ZLinkPeerState.NOT_CONNECTED;
                unavailableReason = Optional.of(ZLinkTopologyReason.INTERNAL_FAILURE);
            }
            case NOT_REQUIRED -> {
                state = ZLinkPeerState.NOT_REQUIRED;
                unavailableReason = Optional.empty();
            }
        }
        return new ZLinkMeshPeerSnapshot(
            peer.routingId(),
            state,
            unavailableReason);
    }

    private static PeerChannels peerChannels(
        ZLinkInternalMeshNode node,
        MeshPeerEntry peer) {
        if (peer.state()
                != MeshPeerState.ADMITTED
            && peer.state()
                != MeshPeerState.DRAINING) {
            return new PeerChannels(List.of(), List.of());
        }
        try {
            return node.peerChannels(
                peer.routingId(), peer.lifecycleGeneration());
        } catch (RuntimeException ignored) {
            // The peer may have changed generation between peers() and this query.
            return new PeerChannels(List.of(), List.of());
        }
    }

    private static List<String> descriptorSources(List<MeshPeerEntry> peers) {
        boolean manual = peers.stream().anyMatch(peer ->
            peer.source() == MeshPeerSource.MANUAL
                || peer.source() == MeshPeerSource.MIXED);
        boolean redis = peers.stream().anyMatch(peer ->
            peer.source() == MeshPeerSource.DISCOVERY
                || peer.source() == MeshPeerSource.MIXED);
        if (manual && redis) {
            return List.of("manual_and_redis");
        }
        if (manual) {
            return List.of("manual");
        }
        return redis ? List.of("redis") : List.of();
    }

    private static LegacyMeshNodeState mapNodeState(MeshNodeState state) {
        return switch (state) {
            case CREATED -> LegacyMeshNodeState.STARTING;
            case STARTED, PARTIAL_READY, READY -> LegacyMeshNodeState.SERVING;
            case DRAINING -> LegacyMeshNodeState.DRAINING;
            case STOPPED -> LegacyMeshNodeState.STOPPED;
            case ERROR -> LegacyMeshNodeState.FAULTED;
        };
    }

    private static boolean isRequiredPeerUnavailable(MeshPeerEntry peer) {
        return peer.state()
            != MeshPeerState.ADMITTED
            && peer.state()
            != MeshPeerState.NOT_REQUIRED;
    }

    private static boolean hasAvailableObjectCapacity(
        ZLinkMeshNodeMonitoringProjection placement) {
        boolean acceptsActors = placement.objectCapabilities().stream()
            .anyMatch(capability ->
                capability.objectKind() == ZLinkPlacementObjectKind.ACTOR);
        boolean acceptsSpots = placement.objectCapabilities().stream()
            .anyMatch(capability ->
                capability.objectKind() != ZLinkPlacementObjectKind.ACTOR);
        boolean actorCapacityAvailable =
            acceptsActors && hasRemainingCapacity(placement.objectCapacity().actors());
        boolean spotCapacityAvailable =
            acceptsSpots
                && hasRemainingCapacity(placement.objectCapacity().spots())
                && (placement.objectCapacity().spotTypes().isEmpty()
                    || placement.objectCapacity().spotTypes().stream()
                        .anyMatch(type -> hasRemainingCapacity(type.usage())));
        return actorCapacityAvailable || spotCapacityAvailable;
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

    private static ZLinkTopologyState mapTopologyState(MeshNodeState state) {
        return switch (state) {
            case CREATED -> ZLinkTopologyState.STARTING;
            case STARTED, PARTIAL_READY, READY -> ZLinkTopologyState.READY;
            case DRAINING -> ZLinkTopologyState.STOPPING;
            case STOPPED -> ZLinkTopologyState.STOPPED;
            case ERROR -> ZLinkTopologyState.FAILED;
        };
    }

    private List<ZLinkMeshRuntimeEvent> mapEvents(
        String meshName,
        RoutingId sourceRid,
        MeshMonitorEvent event) {
        String identifier = switch (event.kind()) {
            case STATE_CHANGED -> "zlink.runtime.mesh_node.state_changed";
            case CHANNEL_CHANGED -> "zlink.runtime.mesh_node.channel_changed";
            case CLAIM_REVOKED -> "zlink.runtime.mesh_node.claim_changed";
            case PEER_CONNECTING, PEER_ADMITTED, PEER_DRAINING, PEER_CLOSED,
                PEER_REJECTED, PROTOCOL_ERROR -> "zlink.runtime.mesh_node.peer_changed";
            default -> null;
        };
        if (identifier == null) {
            return List.of();
        }
        String reason = switch (event.kind()) {
            case PEER_CONNECTING -> "connecting";
            case PEER_ADMITTED -> "ready";
            case PEER_DRAINING -> "draining";
            case PEER_CLOSED -> "disconnected";
            case PEER_REJECTED -> "HandshakeFailed";
            case PROTOCOL_ERROR -> "rejected";
            case BACKPRESSURED -> "backpressure";
            default -> null;
        };
        ZLinkMeshRuntimeEvent mapped = new ZLinkMeshRuntimeEvent(
            identifier,
            nextSequence(meshName),
            Instant.now(),
            meshName,
            sourceRid,
            optionalRid(event.peerRid()),
            optionalPositive(event.peerLifecycleGeneration()),
            optionalText(event.channelName()),
            event.kind() == MeshMonitorEventKind.CLAIM_REVOKED
                ? Optional.of("application")
                : Optional.empty(),
            Optional.empty(),
            Optional.ofNullable(reason),
            event.kind() == MeshMonitorEventKind.STATE_CHANGED
                ? Optional.of(mapNodeState(event.meshState()))
                : Optional.empty());
        if (event.kind() != MeshMonitorEventKind.STATE_CHANGED) {
            return List.of(mapped);
        }
        return List.of(
            mapped,
            new ZLinkMeshRuntimeEvent(
                "zlink.runtime.mesh_node.drain_changed",
                nextSequence(meshName),
                mapped.timestamp(),
                meshName,
                sourceRid,
                mapped.peerRid(),
                mapped.lifecycleGeneration(),
                mapped.channelName(),
                mapped.claimDomain(),
                mapped.messageKind(),
                mapped.reason(),
                mapped.state()));
    }

    private static Optional<RoutingId> optionalRid(RoutingId value) {
        return value == null || value.size() == 0 ? Optional.empty() : Optional.of(value);
    }

    private static Optional<Long> optionalPositive(long value) {
        return value == 0 ? Optional.empty() : Optional.of(value);
    }

    private static Optional<String> optionalText(String value) {
        return value == null || value.isEmpty() ? Optional.empty() : Optional.of(value);
    }

    private record LocationStatusSnapshot(
        String state,
        Optional<Instant> lastSuccessAt,
        Optional<Instant> lastFailureAt) {
    }

    private final class MonitorHub implements AutoCloseable {
        private final String meshName;
        private final ZLinkInternalMeshNode node;
        private final ZLinkMeshNodeMonitoringProjection initialPlacement;
        private final StateLane stateLane = new StateLane();
        private final List<ObserverSubscription> observers = new ArrayList<>();
        private final List<Runnable> signals = new ArrayList<>();
        private boolean stopped;
        private Thread pump;

        MonitorHub(String meshName, ZLinkInternalMeshNode node) {
            this.meshName = meshName;
            this.node = node;
            this.initialPlacement = placementProjection.apply(
                meshName,
                node.status().routingId());
        }

        void subscribe(Flow.Subscriber<? super ZLinkMeshRuntimeEvent> subscriber, int capacity) {
            ObserverSubscription observer =
                new ObserverSubscription(this, subscriber, capacity);
            if (!inStateLane(() -> subscribeCore(observer))) {
                subscriber.onSubscribe(observer);
                observer.fail(new IllegalStateException("RouteMesh monitor is closed"));
                return;
            }
            subscriber.onSubscribe(observer);
            if (!isStopped()) {
                var status = node.status();
                observer.enqueue(event(
                    "zlink.runtime.mesh_node.state_changed",
                    meshName,
                    status.routingId(),
                    Optional.of(mapNodeState(status.state()))));
            }
            startPumpIfNecessary();
        }

        void registerSignal(Runnable signal) {
            if (!inStateLane(() -> registerSignalCore(signal))) {
                return;
            }
            signal.run();
            startPumpIfNecessary();
        }

        void remove(ObserverSubscription observer) {
            boolean empty = inStateLane(() -> removeCore(observer));
            if (empty) {
                close();
                monitorHubs.remove(meshName, this);
            }
        }

        private void pump() {
            MeshNodeMonitor monitor = null;
            try {
                try {
                    monitor = node.openMonitor();
                } catch (RuntimeException ignored) {
                    // Snapshot polling remains authoritative when another monitor owns
                    // the Core push handle.
                }
                MeshNodeState previousState = node.status().state();
                List<MeshPeerEntry> previousPeers = List.copyOf(node.peers());
                String previousChannels = channelSignature(node, previousPeers);
                String previousLocationState = locationSnapshot().state();
                ZLinkMeshNodeMonitoringProjection previousPlacement = initialPlacement;
                long nextDescriptorPoll = System.nanoTime() + DESCRIPTOR_POLL_NANOS;
                while (!isStopped()) {
                    MeshMonitorEvent nativeEvent = null;
                    if (monitor != null) {
                        try {
                            nativeEvent = monitor.recv(RecvFlags.DONT_WAIT);
                        } catch (RuntimeException ignored) {
                            try {
                                monitor.close();
                            } catch (RuntimeException closeFailure) {
                                ignored.addSuppressed(closeFailure);
                            }
                            monitor = null;
                        }
                    }
                    long now = System.nanoTime();
                    if (now >= nextDescriptorPoll) {
                        var status = node.status();
                        ZLinkMeshNodeMonitoringProjection currentPlacement =
                            placementProjection.apply(meshName, status.routingId());
                        if (!samePlacementCapacity(previousPlacement, currentPlacement)) {
                            publish(List.of(placementChangedEvent(
                                meshName,
                                status.routingId(),
                                currentPlacement.descriptorRevision())));
                        }
                        previousPlacement = currentPlacement;
                        nextDescriptorPoll = now + DESCRIPTOR_POLL_NANOS;
                    }
                    if (nativeEvent == null) {
                        var status = node.status();
                        List<MeshPeerEntry> peers = List.copyOf(node.peers());
                        String channels = channelSignature(node, peers);
                        String locationState = locationSnapshot().state();
                        List<ZLinkMeshRuntimeEvent> derived = new ArrayList<>(4);
                        if (status.state() != previousState) {
                            derived.add(event(
                                "zlink.runtime.mesh_node.state_changed",
                                meshName,
                                status.routingId(),
                                Optional.of(mapNodeState(status.state()))));
                        }
                        if (!peers.equals(previousPeers)) {
                            derived.add(peerChangedEvent(
                                meshName,
                                status.routingId(),
                                changedPeer(previousPeers, peers)));
                        }
                        if (!channels.equals(previousChannels)) {
                            derived.add(event(
                                "zlink.runtime.mesh_node.channel_changed",
                                meshName,
                                status.routingId(),
                                Optional.empty()));
                        }
                        if (!locationState.equals(previousLocationState)) {
                            derived.add(locationChangedEvent(
                                meshName,
                                status.routingId(),
                                locationState));
                        }
                        publish(derived);
                        previousState = status.state();
                        previousPeers = peers;
                        previousChannels = channels;
                        previousLocationState = locationState;
                        LockSupport.parkNanos(MONITOR_IDLE_NANOS);
                        continue;
                    }
                    RoutingId sourceRid = node.status().routingId();
                    List<ZLinkMeshRuntimeEvent> events =
                        mapEvents(meshName, sourceRid, nativeEvent);
                    publish(events);
                    previousState = node.status().state();
                    previousPeers = List.copyOf(node.peers());
                    previousChannels = channelSignature(node, previousPeers);
                    previousLocationState = locationSnapshot().state();
                }
            } catch (RuntimeException failure) {
                ObserverSubscription[] current = inStateLane(this::failCore);
                for (ObserverSubscription observer : current) {
                    observer.fail(failure);
                }
            } finally {
                if (monitor != null) {
                    monitor.close();
                }
            }
        }

        private void publish(List<ZLinkMeshRuntimeEvent> events) {
            if (events.isEmpty()) {
                return;
            }
            ObserverSubscription[] current;
            Runnable[] currentSignals;
            MonitorHubSnapshot snapshot = inStateLane(this::snapshotCore);
            current = snapshot.observers();
            currentSignals = snapshot.signals();
            for (Runnable signal : currentSignals) {
                signal.run();
            }
            for (ZLinkMeshRuntimeEvent event : events) {
                for (ObserverSubscription observer : current) {
                    observer.enqueue(event);
                }
            }
        }

        @Override
        public void close() {
            MonitorHubCloseState closeState = inStateLane(this::closeCore);
            if (closeState == null) {
                return;
            }
            Thread current = closeState.pump();
            if (current != null) {
                current.interrupt();
            }
            for (ObserverSubscription observer : closeState.observers()) {
                observer.complete();
            }
            for (Runnable signal : closeState.signals()) {
                signal.run();
            }
        }

        private boolean subscribeCore(ObserverSubscription observer) {
            if (stopped) {
                return false;
            }
            observers.add(observer);
            return true;
        }

        private boolean registerSignalCore(Runnable signal) {
            if (stopped) {
                return false;
            }
            signals.add(signal);
            return true;
        }

        private boolean removeCore(ObserverSubscription observer) {
            observers.remove(observer);
            return observers.isEmpty();
        }

        private boolean isStopped() {
            return inStateLane(() -> stopped);
        }

        private void startPumpIfNecessary() {
            Thread candidate = Thread.ofVirtual()
                .name("zlink-mesh-monitor-" + meshName)
                .unstarted(this::pump);
            if (inStateLane(() -> installPumpCore(candidate))) {
                candidate.start();
            }
        }

        private boolean installPumpCore(Thread candidate) {
            if (stopped || pump != null) {
                return false;
            }
            pump = candidate;
            return true;
        }

        private ObserverSubscription[] failCore() {
            stopped = true;
            ObserverSubscription[] current = observers.toArray(ObserverSubscription[]::new);
            observers.clear();
            return current;
        }

        private MonitorHubSnapshot snapshotCore() {
            return new MonitorHubSnapshot(
                observers.toArray(ObserverSubscription[]::new),
                signals.toArray(Runnable[]::new));
        }

        private MonitorHubCloseState closeCore() {
            if (stopped) {
                return null;
            }
            stopped = true;
            ObserverSubscription[] currentObservers =
                observers.toArray(ObserverSubscription[]::new);
            observers.clear();
            Runnable[] currentSignals = signals.toArray(Runnable[]::new);
            signals.clear();
            return new MonitorHubCloseState(pump, currentObservers, currentSignals);
        }

        private <T> T inStateLane(Supplier<T> work) {
            try {
                return stateLane.runAsync(work).toCompletableFuture().join();
            } catch (java.util.concurrent.CompletionException failure) {
                Throwable cause = failure.getCause();
                if (cause instanceof RuntimeException runtimeFailure) {
                    throw runtimeFailure;
                }
                if (cause instanceof Error error) {
                    throw error;
                }
                throw failure;
            }
        }

        private record MonitorHubSnapshot(
            ObserverSubscription[] observers,
            Runnable[] signals) {
        }

        private record MonitorHubCloseState(
            Thread pump,
            ObserverSubscription[] observers,
            Runnable[] signals) {
        }
    }

    private static MeshPeerEntry changedPeer(
        List<MeshPeerEntry> previous,
        List<MeshPeerEntry> current) {
        for (MeshPeerEntry peer : current) {
            if (!previous.contains(peer)) {
                return peer;
            }
        }
        for (MeshPeerEntry peer : previous) {
            if (!current.contains(peer)) {
                return peer;
            }
        }
        return current.isEmpty() ? null : current.getFirst();
    }

    private ZLinkMeshRuntimeEvent peerChangedEvent(
        String meshName,
        RoutingId sourceRid,
        MeshPeerEntry peer) {
        return new ZLinkMeshRuntimeEvent(
            "zlink.runtime.mesh_node.peer_changed",
            nextSequence(meshName),
            Instant.now(),
            meshName,
            sourceRid,
            peer == null ? Optional.empty() : Optional.of(peer.routingId()),
            peer == null ? Optional.empty() : Optional.of(peer.lifecycleGeneration()),
            Optional.empty(),
            Optional.empty(),
            Optional.empty(),
            peer == null ? Optional.empty() : Optional.of(peer.state().name().toLowerCase()),
            Optional.empty());
    }

    private static String channelSignature(
        ZLinkInternalMeshNode node,
        List<MeshPeerEntry> peers) {
        StringBuilder signature = new StringBuilder(node.channelWeights().toString());
        for (MeshPeerEntry peer : peers) {
            signature.append('|')
                .append(peer.routingId().toHex())
                .append(':')
                .append(peer.lifecycleGeneration())
                .append(':')
                .append(peerChannels(node, peer));
        }
        return signature.toString();
    }

    private ZLinkMeshRuntimeEvent event(
        String identifier,
        String meshName,
        RoutingId sourceRid,
        Optional<LegacyMeshNodeState> state) {
        return new ZLinkMeshRuntimeEvent(
            identifier,
            nextSequence(meshName),
            Instant.now(),
            meshName,
            sourceRid,
            Optional.empty(),
            Optional.empty(),
            Optional.empty(),
            Optional.empty(),
            Optional.empty(),
            Optional.empty(),
            state);
    }

    private ZLinkMeshRuntimeEvent locationChangedEvent(
        String meshName,
        RoutingId sourceRid,
        String state) {
        return new ZLinkMeshRuntimeEvent(
            "zlink.runtime.location.store_changed",
            nextSequence(meshName),
            Instant.now(),
            meshName,
            sourceRid,
            Optional.empty(),
            Optional.empty(),
            Optional.empty(),
            Optional.empty(),
            Optional.empty(),
            Optional.of(state),
            Optional.empty());
    }

    private ZLinkMeshRuntimeEvent placementChangedEvent(
        String meshName,
        RoutingId sourceRid,
        long descriptorRevision) {
        return new ZLinkMeshRuntimeEvent(
            "zlink.runtime.object.placement_changed",
            nextSequence(meshName),
            Instant.now(),
            meshName,
            sourceRid,
            Optional.of(sourceRid),
            Optional.empty(),
            Optional.empty(),
            Optional.empty(),
            Optional.empty(),
            Optional.of("updated"),
            Optional.empty());
    }

    private static boolean samePlacementCapacity(
        ZLinkMeshNodeMonitoringProjection left,
        ZLinkMeshNodeMonitoringProjection right) {
        return left.objectCapacity().equals(right.objectCapacity())
            && left.activationConcurrency().equals(right.activationConcurrency());
    }

    private static ZLinkMeshNodeMonitoringProjection defaultPlacement(
        ZLinkInternalMeshNode node) {
        return new ZLinkMeshNodeMonitoringProjection(
            node == null ? 0 : node.status().descriptorRevision(),
            ZLinkMeshNodeObjectRole.NONE,
            node == null ? 0 : node.placementWeight(),
            new ZLinkPlacementCapacity(
                new ZLinkCapacityUsage(0, 0, 0),
                new ZLinkCapacityUsage(0, 0, 0),
                List.of()),
            new ZLinkActivationConcurrency(0, 128),
            List.of(),
            0,
            Optional.empty());
    }

    private static final class ObserverSubscription implements Flow.Subscription {
        private final MonitorHub hub;
        private final Flow.Subscriber<? super ZLinkMeshRuntimeEvent> subscriber;
        private final int capacity;
        private final ArrayDeque<ZLinkMeshRuntimeEvent> pending = new ArrayDeque<>();
        private final StateLane stateLane = new StateLane();
        private final AtomicLong demand = new AtomicLong();
        private final AtomicInteger draining = new AtomicInteger();
        private boolean cancelled;

        ObserverSubscription(
            MonitorHub hub,
            Flow.Subscriber<? super ZLinkMeshRuntimeEvent> subscriber,
            int capacity) {
            this.hub = hub;
            this.subscriber = subscriber;
            this.capacity = capacity;
        }

        @Override
        public void request(long count) {
            if (count <= 0) {
                fail(new IllegalArgumentException("demand must be positive"));
                return;
            }
            demand.getAndUpdate(current -> {
                long updated = current + count;
                return updated < 0 ? Long.MAX_VALUE : updated;
            });
            scheduleDrain();
        }

        @Override
        public void cancel() {
            if (inStateLane(this::cancelCore)) {
                hub.remove(this);
            }
        }

        void enqueue(ZLinkMeshRuntimeEvent event) {
            if (!inStateLane(() -> enqueueCore(event))) {
                return;
            }
            scheduleDrain();
        }

        void fail(Throwable failure) {
            if (!inStateLane(this::failCore)) {
                return;
            }
            try {
                subscriber.onError(failure);
            } finally {
                hub.remove(this);
            }
        }

        void complete() {
            if (!inStateLane(this::completeCore)) {
                return;
            }
            subscriber.onComplete();
        }

        private void scheduleDrain() {
            if (inStateLane(() -> !cancelled)
                && draining.compareAndSet(0, 1)) {
                ForkJoinPool.commonPool().execute(this::drain);
            }
        }

        private void drain() {
            try {
                while (true) {
                    ZLinkMeshRuntimeEvent event = inStateLane(
                        () -> takePendingCore(demand.get()));
                    if (event == null) {
                        return;
                    }
                    if (demand.get() != Long.MAX_VALUE) {
                        demand.decrementAndGet();
                    }
                    subscriber.onNext(event);
                }
            } catch (RuntimeException failure) {
                fail(failure);
            } finally {
                draining.set(0);
                if (inStateLane(() -> !pending.isEmpty()
                    && demand.get() > 0
                    && !cancelled)) {
                    scheduleDrain();
                }
            }
        }

        private boolean cancelCore() {
            if (cancelled) {
                return false;
            }
            cancelled = true;
            pending.clear();
            return true;
        }

        private boolean enqueueCore(ZLinkMeshRuntimeEvent event) {
            if (cancelled) {
                return false;
            }
            if (pending.size() == capacity) {
                coalesceOrReplace(event);
            } else {
                pending.addLast(event);
            }
            return true;
        }

        private boolean failCore() {
            return cancelCore();
        }

        private boolean completeCore() {
            return cancelCore();
        }

        private ZLinkMeshRuntimeEvent takePendingCore(long currentDemand) {
            if (cancelled || currentDemand <= 0) {
                return null;
            }
            return pending.pollFirst();
        }

        private <T> T inStateLane(Supplier<T> work) {
            try {
                return stateLane.runAsync(work).toCompletableFuture().join();
            } catch (java.util.concurrent.CompletionException failure) {
                Throwable cause = failure.getCause();
                if (cause instanceof RuntimeException runtimeFailure) {
                    throw runtimeFailure;
                }
                if (cause instanceof Error error) {
                    throw error;
                }
                throw failure;
            }
        }

        private void coalesceOrReplace(ZLinkMeshRuntimeEvent event) {
            if (pending.stream().anyMatch(ObserverSubscription::isTerminalDrain)
                && !isTerminalDrain(event)) {
                return;
            }
            if (isTerminalDrain(event)) {
                boolean removed = pending.stream()
                    .filter(current -> !isTerminalDrain(current))
                    .findFirst()
                    .map(pending::remove)
                    .orElse(false);
                if (!removed) {
                    pending.pollFirst();
                }
            } else {
                pending.pollFirst();
            }
            pending.addLast(event);
        }

        private static boolean isTerminalDrain(ZLinkMeshRuntimeEvent event) {
            if (!event.identifier().equals("zlink.runtime.mesh_node.drain_changed")
                || event.state().isEmpty()) {
                return false;
            }
            return switch (event.state().get()) {
                case STOPPED, FORCE_STOPPING -> true;
                default -> false;
            };
        }

    }

    private static final class StateLane {
        private static final ThreadLocal<StateLane> CURRENT = new ThreadLocal<>();

        private final Queue<Runnable> mailbox = new ConcurrentLinkedQueue<>();
        private final AtomicBoolean draining = new AtomicBoolean();
        private final ExecutorService executor = Executors.newVirtualThreadPerTaskExecutor();

        <T> CompletionStage<T> runAsync(Supplier<T> work) {
            Objects.requireNonNull(work, "work");
            if (CURRENT.get() == this) {
                throw new IllegalStateException(
                    "This code already runs on the state lane it is trying to enter. Call the "
                        + "component's private state method directly instead of re-entering its "
                        + "public surface.");
            }
            CompletableFuture<T> result = new CompletableFuture<>();
            mailbox.add(() -> runWithCurrent(() -> {
                try {
                    T value = work.get();
                    result.completeAsync(() -> value);
                } catch (RuntimeException | Error error) {
                    result.completeAsync(() -> {
                        throw error;
                    });
                }
            }));
            scheduleDrain();
            return result;
        }

        private void scheduleDrain() {
            if (draining.compareAndSet(false, true)) {
                executor.execute(this::drain);
            }
        }

        private void drain() {
            while (true) {
                Runnable work = mailbox.poll();
                if (work == null) {
                    draining.set(false);
                    if (!mailbox.isEmpty()) {
                        scheduleDrain();
                    }
                    return;
                }
                work.run();
            }
        }

        private void runWithCurrent(Runnable work) {
            StateLane previous = CURRENT.get();
            CURRENT.set(this);
            try {
                work.run();
            } finally {
                if (previous == null) {
                    CURRENT.remove();
                } else {
                    CURRENT.set(previous);
                }
            }
        }
    }
}

record ZLinkMeshRuntimeEvent(
    String identifier,
    long sequence,
    Instant timestamp,
    String meshName,
    RoutingId sourceRid,
    Optional<RoutingId> peerRid,
    Optional<Long> lifecycleGeneration,
    Optional<String> channelName,
    Optional<String> claimDomain,
    Optional<String> messageKind,
    Optional<String> reason,
    Optional<LegacyMeshNodeState> state) {
}

enum LegacyMeshNodeState {
    STARTING,
    SERVING,
    DRAINING,
    FORCE_STOPPING,
    STOPPED,
    FAULTED
}
