package systems.zlink.framework.runtime.locations;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locations.ZLinkLocationOptions;
import systems.zlink.framework.locations.ZLinkLocationRole;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.locations.ZLinkObjectCapability;
import systems.zlink.framework.locations.ZLinkPageRequest;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeState;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerState;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot;
import systems.zlink.framework.runtime.internal.locations.ZLinkAutoConnectPeer;
import systems.zlink.framework.runtime.internal.locations.ZLinkAutoConnectPeerResolver;
import systems.zlink.framework.runtime.internal.locations.ZLinkAutoConnectType;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptorKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementAllocationState;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceMessageFollowWireCodec;
import systems.zlink.framework.spots.ZLinkSpotKind;

import java.time.Duration;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ThreadLocalRandom;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.UnaryOperator;

/** Resolves public handles from durable authority without legacy location rows. */
public final class ZLinkStoreLocationResolvers
        implements ZLinkAutoConnectPeerResolver, AutoCloseable {
    private static final int DESCRIPTOR_SCAN_PAGE_ITEMS = 1000;
    private final ZLinkRegisteredLocationStores stores;
    private final ZLinkLiveLocationRows liveRows;
    private final Duration routeCacheMaxAge;
    private final Map<String, CachedRoute<SpotRoute>> spotRoutes = new ConcurrentHashMap<>();
    // Entries exist only while a store resolution is in flight. All changes to a key's
    // read eligibility and positive route use spotRoutes.compute for the same key.
    private final Map<String, SpotRouteRead> spotRouteReads = new ConcurrentHashMap<>();

    private static final class SpotRouteRead {
        private String storeVersion;
    }

    private final Map<String, CachedRoute<ActorRoute>> actorRoutes = new ConcurrentHashMap<>();
    private final ZLinkServiceAuthorityPayloadCodec spotAuthorityCodec =
            new ZLinkServiceAuthorityPayloadCodec();
    private final ZLinkActorAuthorityPayloadCodec actorAuthorityCodec =
            new ZLinkActorAuthorityPayloadCodec();
    private final Map<String, PlacementObservation> placement = new HashMap<>();
    private boolean closed;

    // A null node list means discovery has not observed this mesh yet. An empty list is a
    // published observation and must produce Unavailable rather than waiting for another poll.
    private record PlacementObservation(
            List<ZLinkMeshNodeDescriptor> nodes, CompletableFuture<Void> changed) {
        private PlacementObservation() {
            this(null, new CompletableFuture<>());
        }
    }

    private Runnable routeReadAdmission = () -> {};

    public void routeReadAdmission(Runnable admission) {
        routeReadAdmission = Objects.requireNonNull(admission);
    }

    private final AtomicBoolean authorityStoreFailure = new AtomicBoolean();

    public ZLinkStoreLocationResolvers(
            ZLinkRegisteredLocationStores stores, ZLinkLocationOptions options) {
        this(stores, ZLinkLiveLocationRows.create(stores, options));
    }

    public ZLinkStoreLocationResolvers(
            ZLinkRegisteredLocationStores stores, ZLinkLiveLocationRows liveRows) {
        this.stores = Objects.requireNonNull(stores, "stores");
        this.liveRows = Objects.requireNonNull(liveRows, "liveRows");
        this.routeCacheMaxAge = liveRows.routeCacheMaxAge();
    }

    public CompletionStage<SpotRoute> resolveSpot(String spotId) {
        SpotRoute cached = cached(spotRoutes, spotId);
        if (cached != null) {
            return CompletableFuture.completedFuture(cached);
        }
        SpotRouteRead pending = beginSpotRead(spotId);
        try {
            routeReadAdmission.run();
            return observeAuthorityRead(
                            stores.unifiedStore()
                                    .read(ZLinkAuthorityKeyCodec.spot(spotId), () -> false))
                    .thenCompose(read -> resolveReadySpot(spotId, read, pending))
                    .whenComplete((value, failure) -> endSpotRead(spotId, pending));
        } catch (RuntimeException | Error failure) {
            endSpotRead(spotId, pending);
            throw failure;
        }
    }

    public CompletionStage<ActorRoute> resolveActor(String actorId) {
        ActorRoute cached = cached(actorRoutes, actorId);
        if (cached != null) {
            return CompletableFuture.completedFuture(cached);
        }
        return readActor(actorId);
    }

    /** Reads current authority directly; a failed read preserves the positive route cache. */
    public CompletionStage<ActorRoute> readActor(String actorId) {
        return readActor(actorId, actorRoutes);
    }

    /**
     * Reads current authority for a public {@code Find}. The result is an observation: it neither
     * stores nor removes a positive route, so only a request resolution decides which owner later
     * requests use (04-actor-model §6.6, 08-routing §2.2).
     */
    public CompletionStage<ActorRoute> findActor(String actorId) {
        return readActor(actorId, null);
    }

    private CompletionStage<ActorRoute> readActor(
            String actorId, Map<String, CachedRoute<ActorRoute>> cache) {
        routeReadAdmission.run();
        return observeAuthorityRead(
                        stores.unifiedStore()
                                .read(ZLinkAuthorityKeyCodec.actor(actorId), () -> false))
                .thenCompose(read -> resolveReadyActor(actorId, read, cache));
    }

    @Override
    public CompletionStage<List<ZLinkAutoConnectPeer>> listPeers(
            ZLinkAutoConnectType type, String meshName, ZLinkLocationRole role) {
        PlacementObservation readFrom;
        synchronized (placement) {
            readFrom =
                    closed
                            ? null
                            : placement.computeIfAbsent(
                                    meshName, ignored -> new PlacementObservation());
        }
        return listLiveMeshNodes(meshName)
                .thenApply(nodes -> publishPlacementScan(meshName, readFrom, nodes))
                .thenApply(
                        nodes ->
                                nodes.stream()
                                        .map(
                                                node ->
                                                        new ZLinkAutoConnectPeer(
                                                                type,
                                                                node.meshName(),
                                                                node.rid(),
                                                                role == null
                                                                        ? ZLinkLocationRole.ROUTER
                                                                        : role,
                                                                node.endpoint(),
                                                                node.placementWeight(),
                                                                node.state()
                                                                        != ZLinkFrameworkRuntimeState
                                                                                .SERVING,
                                                                node.lifecycleGeneration(),
                                                                Map.of(
                                                                        ZLinkAutoConnectPlanner
                                                                                .SECURITY_IDENTITY_METADATA_KEY,
                                                                        node.securityIdentity()),
                                                                node.objectCapabilities().stream()
                                                                        .filter(
                                                                                capability ->
                                                                                        capability
                                                                                                        .objectKind()
                                                                                                == ZLinkPlacementObjectKind
                                                                                                        .ACTOR)
                                                                        .map(
                                                                                capability ->
                                                                                        "actor:"
                                                                                                + capability
                                                                                                        .stableType())
                                                                        .toList(),
                                                                node.ownerId(),
                                                                node.leaseGeneration(),
                                                                node.updatedAt(),
                                                                node.objectRole(),
                                                                !node.channelWeights().isEmpty()))
                                        .toList());
    }

    private List<ZLinkMeshNodeDescriptor> publishPlacementScan(
            String meshName, PlacementObservation readFrom, List<ZLinkMeshNodeDescriptor> nodes) {
        updatePlacement(
                meshName,
                current ->
                        current == readFrom
                                ? new PlacementObservation(
                                        List.copyOf(nodes), new CompletableFuture<>())
                                : current);
        return nodes;
    }

    private void updatePlacement(String meshName, UnaryOperator<PlacementObservation> update) {
        PlacementObservation previous;
        synchronized (placement) {
            if (closed) return;
            previous = placement.get(meshName);
            PlacementObservation next = update.apply(previous);
            if (next == previous) return;
            placement.put(meshName, next);
        }
        if (previous != null) previous.changed().complete(null);
    }

    /** A confirmed local Store publication is another discovery observation, not a request read. */
    public CompletionStage<Void> observePublishedMeshNode(ZLinkMeshNodeDescriptor descriptor) {
        return liveRows.meshNodes(List.of(descriptor))
                .thenAccept(
                        live ->
                                updatePlacement(
                                        descriptor.meshName(),
                                        previous -> {
                                            List<ZLinkMeshNodeDescriptor> nodes = new ArrayList<>();
                                            if (previous != null && previous.nodes() != null) {
                                                for (var node : previous.nodes()) {
                                                    if (!node.rid().equals(descriptor.rid()))
                                                        nodes.add(node);
                                                    else if (node.lifecycleGeneration()
                                                                    == descriptor
                                                                            .lifecycleGeneration()
                                                            && node.descriptorRevision()
                                                                    > descriptor
                                                                            .descriptorRevision())
                                                        return previous;
                                                }
                                            }
                                            nodes.addAll(live);
                                            return new PlacementObservation(
                                                    List.copyOf(nodes), new CompletableFuture<>());
                                        }));
    }

    /** MeshNode §5.2: placement consumes the observation owned by automatic discovery. */
    public CompletionStage<ZLinkMeshNodeDescriptor> selectPlacement(
            String meshName,
            ZLinkPlacementObjectKind kind,
            String stableType,
            boolean newActor,
            ZLinkInternalMeshNode source,
            Set<ZLinkMeshNodeDescriptorKey> excluded,
            long deadline) {
        return selectPlacement(
                meshName, kind, stableType, newActor, source, excluded, deadline, true);
    }

    private CompletionStage<ZLinkMeshNodeDescriptor> selectPlacement(
            String meshName,
            ZLinkPlacementObjectKind kind,
            String stableType,
            boolean newActor,
            ZLinkInternalMeshNode source,
            Set<ZLinkMeshNodeDescriptorKey> excluded,
            long deadline,
            boolean refreshAvailable) {
        PlacementObservation observation;
        synchronized (placement) {
            if (closed)
                return CompletableFuture.failedFuture(
                        new java.util.concurrent.CancellationException("discovery closed"));
            observation =
                    placement.computeIfAbsent(meshName, ignored -> new PlacementObservation());
        }
        if (System.currentTimeMillis() >= deadline) {
            return CompletableFuture.failedFuture(
                    new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                            "Placement deadline exceeded"));
        }
        List<ZLinkMeshNodeDescriptor> live = observedLiveNodes(observation);
        PlacementDecision decision =
                observation.nodes() == null
                        ? new PlacementDecision(List.of(), true)
                        : classifyPlacement(live, kind, stableType, newActor, source, excluded);
        boolean changed;
        synchronized (placement) {
            changed = closed || observation != placement.get(meshName);
        }
        if (changed)
            return selectPlacement(
                    meshName,
                    kind,
                    stableType,
                    newActor,
                    source,
                    excluded,
                    deadline,
                    refreshAvailable);
        if (!decision.candidates().isEmpty()) {
            return CompletableFuture.completedFuture(weightedPlacement(decision.candidates()));
        }
        if (!decision.waitForDiscovery() && !refreshAvailable) {
            return CompletableFuture.failedFuture(
                    new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.UNAVAILABLE, "Placement is unavailable"));
        }

        CompletionStage<?> discovery;
        if (decision.waitForDiscovery()) {
            discovery = withinPlacementDeadline(observation.changed(), deadline);
        } else {
            discovery = refreshPlacement(meshName, observation, deadline);
        }
        return discovery.thenCompose(
                ignored ->
                        selectPlacement(
                                meshName,
                                kind,
                                stableType,
                                newActor,
                                source,
                                excluded,
                                deadline,
                                refreshAvailable && decision.waitForDiscovery()));
    }

    private CompletionStage<List<ZLinkMeshNodeDescriptor>> refreshPlacement(
            String meshName, PlacementObservation observation, long deadline) {
        if (System.currentTimeMillis() >= deadline) {
            return CompletableFuture.failedFuture(
                    new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                            "Placement deadline exceeded"));
        }
        return withinPlacementDeadline(
                        CompletableFuture.completedFuture(meshName)
                                .thenCompose(this::listLiveMeshNodes),
                        deadline)
                .thenApply(nodes -> publishPlacementScan(meshName, observation, nodes));
    }

    private List<ZLinkMeshNodeDescriptor> observedLiveNodes(PlacementObservation observation) {
        return observation.nodes() == null
                ? List.of()
                : observation.nodes().stream().filter(liveRows::hasObservedLiveLease).toList();
    }

    private static ZLinkMeshNodeDescriptor weightedPlacement(
            List<ZLinkMeshNodeDescriptor> candidates) {
        long total = candidates.stream().mapToLong(ZLinkMeshNodeDescriptor::placementWeight).sum();
        long choice = ThreadLocalRandom.current().nextLong(total);
        for (var candidate : candidates) {
            choice -= candidate.placementWeight();
            if (choice < 0) return candidate;
        }
        return candidates.getLast();
    }

    public record InstancePlacement(String stableType, ZLinkMeshNodeDescriptor descriptor) {}

    /**
     * Spot address messaging §4.1: infer types from serving descriptors before applying capacity.
     */
    public CompletionStage<InstancePlacement> selectInstancePlacement(
            String meshName, String requestedType, Set<RoutingId> connected, long deadline) {
        return selectInstancePlacement(meshName, requestedType, connected, deadline, true);
    }

    private CompletionStage<InstancePlacement> selectInstancePlacement(
            String meshName,
            String requestedType,
            Set<RoutingId> connected,
            long deadline,
            boolean refreshAvailable) {
        PlacementObservation observation;
        synchronized (placement) {
            if (closed)
                return CompletableFuture.failedFuture(
                        new java.util.concurrent.CancellationException("discovery closed"));
            observation =
                    placement.computeIfAbsent(meshName, ignored -> new PlacementObservation());
        }
        var nodes = observedLiveNodes(observation);
        var types = instanceTypes(nodes);
        String stableType = requestedType;
        if (stableType == null && types.size() > 1) {
            return CompletableFuture.failedFuture(
                    new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.INVALID_OPERATION,
                            "Instance Spot type is required when multiple types are registered"));
        }
        if (stableType == null && !types.isEmpty()) stableType = types.getFirst();
        boolean supported = stableType != null && types.contains(stableType);
        String selectedType = stableType;
        var candidates =
                supported
                        ? nodes.stream()
                                .filter(node -> node.state() == ZLinkFrameworkRuntimeState.SERVING)
                                .filter(
                                        node ->
                                                isPlacementCandidate(
                                                        node,
                                                        ZLinkPlacementObjectKind.INSTANCE_SPOT,
                                                        selectedType,
                                                        false,
                                                        Set.of()))
                                .toList()
                        : List.<ZLinkMeshNodeDescriptor>of();
        if (!candidates.isEmpty() && System.currentTimeMillis() < deadline) {
            return CompletableFuture.completedFuture(
                    new InstancePlacement(
                            stableType,
                            weightedPlacement(
                                    preferConnectedInstanceTargets(candidates, connected))));
        }
        if (refreshAvailable) {
            return refreshPlacement(meshName, observation, deadline)
                    .thenCompose(
                            ignored ->
                                    selectInstancePlacement(
                                            meshName, requestedType, connected, deadline, false));
        }
        if (System.currentTimeMillis() >= deadline) {
            return CompletableFuture.failedFuture(
                    new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                            "Placement deadline exceeded"));
        }
        return CompletableFuture.failedFuture(
                new ZLinkFrameworkException(
                        supported
                                ? ZLinkFrameworkErrorKind.UNAVAILABLE
                                : ZLinkFrameworkErrorKind.NOT_FOUND,
                        supported
                                ? "No Instance Spot placement target is available"
                                : "No serving Instance Spot node provides the requested type"));
    }

    public static List<ZLinkMeshNodeDescriptor> preferConnectedInstanceTargets(
            List<ZLinkMeshNodeDescriptor> candidates, Set<RoutingId> connected) {
        var ready = candidates.stream().filter(node -> connected.contains(node.rid())).toList();
        return ready.isEmpty() ? candidates : ready;
    }

    public static List<String> instanceTypes(List<ZLinkMeshNodeDescriptor> nodes) {
        return nodes.stream()
                .filter(
                        node ->
                                node.state() == ZLinkFrameworkRuntimeState.SERVING
                                        && node.objectRole() == ZLinkMeshNodeObjectRole.SERVER)
                .flatMap(node -> node.objectCapabilities().stream())
                .filter(
                        capability ->
                                capability.objectKind() == ZLinkPlacementObjectKind.INSTANCE_SPOT)
                .map(ZLinkObjectCapability::stableType)
                .distinct()
                .toList();
    }

    private static <T> CompletionStage<T> withinPlacementDeadline(
            CompletionStage<T> operation, long deadline) {
        // Only the dependent future owns this request's deadline.
        return operation
                .thenApply(value -> value)
                .toCompletableFuture()
                .orTimeout(
                        Math.max(0L, deadline - System.currentTimeMillis()), TimeUnit.MILLISECONDS)
                .exceptionallyCompose(
                        failure ->
                                CompletableFuture.failedFuture(
                                        failure instanceof TimeoutException
                                                ? new ZLinkFrameworkException(
                                                        ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                                                        "Placement deadline exceeded",
                                                        failure)
                                                : failure));
    }

    public record PlacementDecision(
            List<ZLinkMeshNodeDescriptor> candidates, boolean waitForDiscovery) {}

    public static PlacementDecision classifyPlacement(
            List<ZLinkMeshNodeDescriptor> nodes,
            ZLinkPlacementObjectKind kind,
            String stableType,
            boolean newActor,
            ZLinkInternalMeshNode source,
            Set<ZLinkMeshNodeDescriptorKey> excluded) {
        List<ZLinkMeshNodeDescriptor> candidates = new ArrayList<>();
        boolean waiting = false;
        for (var node : nodes) {
            if (!isPlacementCandidate(node, kind, stableType, newActor, excluded)) continue;
            if (node.state() == ZLinkFrameworkRuntimeState.SERVING && exactReady(node, source))
                candidates.add(node);
            else waiting = true;
        }
        return new PlacementDecision(List.copyOf(candidates), waiting);
    }

    private static boolean isPlacementCandidate(
            ZLinkMeshNodeDescriptor node,
            ZLinkPlacementObjectKind kind,
            String stableType,
            boolean newActor,
            Set<ZLinkMeshNodeDescriptorKey> excluded) {
        return node.objectRole() == ZLinkMeshNodeObjectRole.SERVER
                && (node.state() == ZLinkFrameworkRuntimeState.SERVING
                        || node.state() == ZLinkFrameworkRuntimeState.PREPARING)
                && node.placementWeight() > 0
                && !excluded.contains(new ZLinkMeshNodeDescriptorKey(node.meshName(), node.rid()))
                && node.objectCapabilities().stream()
                        .anyMatch(
                                capability ->
                                        capability.objectKind() == kind
                                                && capability.stableType().equals(stableType)
                                                && (!newActor || capability.entryAccepted())
                                                && ((kind == ZLinkPlacementObjectKind.ACTOR
                                                                && !newActor)
                                                        || hasPlacementCapacity(node, capability)));
    }

    public static boolean hasPlacementCapacity(
            ZLinkMeshNodeDescriptor node, ZLinkObjectCapability capability) {
        if (capability.objectKind() == ZLinkPlacementObjectKind.ACTOR)
            return node.capacity().actors().hasRoomFor(1);
        return node.capacity().spots().hasRoomFor(1)
                && node.capacity().spotTypes().stream()
                        .anyMatch(
                                type ->
                                        type.objectKind() == capability.objectKind()
                                                && type.stableType().equals(capability.stableType())
                                                && type.usage().hasRoomFor(1));
    }

    private static boolean exactReady(
            ZLinkMeshNodeDescriptor candidate, ZLinkInternalMeshNode source) {
        if (source == null) return false;
        try {
            var local = source.status();
            if (candidate.rid().equals(local.routingId()))
                return candidate.meshName().equals(local.meshName())
                        && local.state() == MeshNodeState.READY
                        && candidate.lifecycleGeneration() == local.lifecycleGeneration();
            return source.peers().stream()
                    .anyMatch(
                            peer ->
                                    peer.routingId().equals(candidate.rid())
                                            && peer.lifecycleGeneration()
                                                    == candidate.lifecycleGeneration()
                                            && peer.state() == MeshPeerState.ADMITTED);
        } catch (RuntimeException unavailableSource) {
            return false;
        }
    }

    public CompletionStage<List<ZLinkMeshNodeDescriptor>> listLiveMeshNodes(String meshName) {
        return listMeshNodes(meshName).thenCompose(liveRows::meshNodes);
    }

    private static CompletionStage<List<ZLinkMeshNodeDescriptor>> listMeshNodes(
            ZLinkLocationRepository store,
            String meshName,
            String continuation,
            List<ZLinkMeshNodeDescriptor> values) {
        return store.listMeshNodes(
                        meshName, new ZLinkPageRequest(DESCRIPTOR_SCAN_PAGE_ITEMS, continuation))
                .thenCompose(
                        page -> {
                            values.addAll(page.items());
                            return page.continuationToken() == null
                                    ? CompletableFuture.completedFuture(List.copyOf(values))
                                    : listMeshNodes(
                                            store, meshName, page.continuationToken(), values);
                        });
    }

    public CompletionStage<List<ZLinkMeshNodeDescriptor>> listMeshNodes(String meshName) {
        return listMeshNodes(stores.unifiedStore(), meshName);
    }

    public static CompletionStage<List<ZLinkMeshNodeDescriptor>> listMeshNodes(
            ZLinkLocationRepository store, String meshName) {
        return listMeshNodes(store, meshName, null, new ArrayList<>());
    }

    public void invalidateActorRoute(String actorId) {
        actorRoutes.remove(Objects.requireNonNull(actorId, "actorId"));
    }

    public void invalidateSpotRoute(String spotId) {
        invalidateSpotRoute(spotId, null);
    }

    private void invalidateSpotRoute(String spotId, SpotRouteRead source) {
        spotRoutes.compute(
                Objects.requireNonNull(spotId, "spotId"),
                (key, cached) -> {
                    if (source != null && spotRouteReads.get(key) != source) return cached;
                    spotRouteReads.remove(key);
                    return null;
                });
    }

    /**
     * Removes a cached route only when every fence field still matches the cached value. A delayed
     * notice from an older owner cannot erase a newer positive route admitted after relocation.
     */
    public boolean invalidateRouteIfMatches(ZLinkServiceMessageFollowWireCodec.Route fence) {
        Objects.requireNonNull(fence, "fence");
        if (fence instanceof ZLinkServiceMessageFollowWireCodec.ActorRoute actor) {
            CachedRoute<ActorRoute> cached = actorRoutes.get(actor.actorId());
            if (cached == null
                    || !(cached.value() instanceof ActorRoute route)
                    || !route.actorRef().actorId().equals(actor.actorId())
                    || route.actorRef().objectGeneration() != actor.objectGeneration()
                    || !route.nodeRid().equals(actor.targetNodeRid())
                    || route.targetNodeGeneration() != actor.targetNodeGeneration()
                    || route.authorityOwnerGeneration() != actor.authorityOwnerGeneration()
                    || route.ownerLeaseGeneration() != actor.ownerLeaseGeneration()
                    || cached.ownerLeaseGeneration() != actor.ownerLeaseGeneration()) {
                return false;
            }
            return actorRoutes.remove(actor.actorId(), cached);
        }
        ZLinkServiceMessageFollowWireCodec.SpotRoute spot =
                (ZLinkServiceMessageFollowWireCodec.SpotRoute) fence;
        CachedRoute<SpotRoute> cached = spotRoutes.get(spot.spotId());
        if (cached == null
                || !(cached.value() instanceof SpotRoute route)
                || !route.spotId().equals(spot.spotId())
                || route.spotGeneration() != spot.objectGeneration()
                || !route.nodeRid().equals(spot.targetNodeRid())
                || route.targetNodeGeneration() != spot.targetNodeGeneration()
                || route.authorityOwnerGeneration() != spot.authorityOwnerGeneration()
                || route.ownerLeaseGeneration() != spot.ownerLeaseGeneration()
                || cached.ownerLeaseGeneration() != spot.ownerLeaseGeneration()) {
            return false;
        }
        boolean[] removed = {false};
        spotRoutes.compute(
                spot.spotId(),
                (key, current) -> {
                    if (current != cached) return current;
                    spotRouteReads.remove(key);
                    removed[0] = true;
                    return null;
                });
        return removed[0];
    }

    public boolean invalidateRouteIfMatches(ZLinkServiceMessageFollowWireCodec.Notice notice) {
        Objects.requireNonNull(notice, "notice");
        return invalidateRouteIfMatches(notice.source());
    }

    public void invalidateAllRoutes() {
        spotRouteReads.keySet().forEach(this::invalidateSpotRoute);
        spotRoutes.clear();
        actorRoutes.clear();
    }

    private SpotRouteRead beginSpotRead(String spotId) {
        SpotRouteRead pending = new SpotRouteRead();
        spotRoutes.compute(
                spotId,
                (key, cached) -> {
                    spotRouteReads.put(key, pending);
                    return cached;
                });
        return pending;
    }

    private void endSpotRead(String spotId, SpotRouteRead pending) {
        spotRoutes.compute(
                spotId,
                (key, cached) -> {
                    spotRouteReads.remove(key, pending);
                    return cached;
                });
    }

    private void observeSpotAuthority(String spotId, String storeVersion, SpotRouteRead source) {
        spotRoutes.compute(
                spotId,
                (key, cached) -> {
                    SpotRouteRead pending = spotRouteReads.get(key);
                    if (source != null && pending != source) return cached;
                    if (pending != null) {
                        if (storeVersion == null
                                || (pending.storeVersion != null
                                        && !pending.storeVersion.equals(storeVersion))) {
                            spotRouteReads.remove(key);
                        } else {
                            pending.storeVersion = storeVersion;
                        }
                    }
                    // StoreVersion is opaque; a different observation invalidates the snapshot.
                    return cached != null && !cached.storeVersion().equals(storeVersion)
                            ? null
                            : cached;
                });
    }

    public CompletionStage<SpotRoute> resolveReadySpot(String spotId, Object read) {
        observeSpotAuthority(
                spotId,
                read instanceof ZLinkAuthoritySnapshot snapshot ? snapshot.storeVersion() : null,
                null);
        SpotRouteRead pending = beginSpotRead(spotId);
        try {
            return resolveReadySpot(spotId, read, pending)
                    .whenComplete((value, failure) -> endSpotRead(spotId, pending));
        } catch (RuntimeException | Error failure) {
            endSpotRead(spotId, pending);
            throw failure;
        }
    }

    private CompletionStage<SpotRoute> resolveReadySpot(
            String spotId, Object read, SpotRouteRead pending) {
        observeSpotAuthority(
                spotId,
                read instanceof ZLinkAuthoritySnapshot snapshot ? snapshot.storeVersion() : null,
                pending);
        if (!(read instanceof ZLinkAuthoritySnapshot snapshot)) {
            invalidateSpotRoute(spotId, pending);
            return CompletableFuture.completedFuture(null);
        }
        var authority = spotAuthorityCodec.decode(snapshot.payload()).orElse(null);
        // Only Ready and Closing authorities route (spot address messaging §9: a direct call
        // without Instance intent to a Creating Spot is NotFound, as for Missing).
        if (authority == null
                || authority.state() == ZLinkServiceAuthorityPayloadCodec.State.CREATING
                || (authority.state() == ZLinkServiceAuthorityPayloadCodec.State.READY
                        && snapshot.allocation().state() != ZLinkPlacementAllocationState.ACTIVE)
                || !authority.spotId().equals(spotId)
                || !authority.ownerId().equals(snapshot.ownerId())
                || authority.ownerLeaseGeneration() != snapshot.ownerLeaseGeneration()) {
            invalidateSpotRoute(spotId, pending);
            return CompletableFuture.completedFuture(null);
        }
        SpotRoute route =
                new SpotRoute(
                        authority.meshName(),
                        authority.spotId(),
                        snapshot.objectGeneration(),
                        authority.nodeRid(),
                        authority.nodeGeneration(),
                        snapshot.authorityOwnerGeneration(),
                        snapshot.ownerLeaseGeneration(),
                        authority.user().isPresent() ? ZLinkSpotKind.USER : ZLinkSpotKind.INSTANCE,
                        authority.stableType(),
                        snapshot.ownerId(),
                        snapshot.storeVersion(),
                        authority.state());
        if (authority.state() != ZLinkServiceAuthorityPayloadCodec.State.READY) {
            invalidateSpotRoute(spotId, pending);
            return CompletableFuture.completedFuture(route);
        }
        return admitPositiveRoute(
                spotRoutes,
                spotId,
                route,
                snapshot.ownerId(),
                snapshot.ownerLeaseGeneration(),
                snapshot.storeVersion(),
                pending);
    }

    private CompletionStage<ActorRoute> resolveReadyActor(
            String actorId, Object read, Map<String, CachedRoute<ActorRoute>> cache) {
        if (!(read instanceof ZLinkAuthoritySnapshot snapshot)
                || snapshot.allocation().state() != ZLinkPlacementAllocationState.ACTIVE
                || snapshot.allocation().objectKind() != ZLinkPlacementObjectKind.ACTOR) {
            forget(cache, actorId);
            return CompletableFuture.completedFuture(null);
        }
        var authority = actorAuthorityCodec.decode(snapshot.payload()).orElse(null);
        if (authority == null
                || authority.state() != ZLinkActorAuthorityPayloadCodec.State.READY
                || !authority.actorId().equals(actorId)
                || !authority.ownerId().equals(snapshot.ownerId())
                || authority.ownerLeaseGeneration() != snapshot.ownerLeaseGeneration()) {
            ActorRoute canonical = projectCanonicalActor(snapshot, actorId);
            if (canonical == null) {
                forget(cache, actorId);
                return CompletableFuture.completedFuture(null);
            }
            return admitPositiveRoute(
                    cache,
                    actorId,
                    canonical,
                    snapshot.ownerId(),
                    snapshot.ownerLeaseGeneration(),
                    snapshot.storeVersion());
        }
        ActorRoute route =
                new ActorRoute(
                        new ActorRef(
                                actorId,
                                snapshot.objectGeneration(),
                                authority.meshName(),
                                authority.nodeRid()),
                        authority.currentSpotKind() == 1 ? ZLinkSpotKind.ENTRY : ZLinkSpotKind.USER,
                        authority.currentSpotId(),
                        authority.currentSpotGeneration(),
                        authority.meshName(),
                        authority.nodeRid(),
                        authority.nodeGeneration(),
                        snapshot.authorityOwnerGeneration(),
                        snapshot.ownerLeaseGeneration());
        return admitPositiveRoute(
                cache,
                actorId,
                route,
                snapshot.ownerId(),
                snapshot.ownerLeaseGeneration(),
                snapshot.storeVersion());
    }

    // The authority payload is implementation-private. Cross-language route
    // resolution instead relies on the canonical outer allocation row.
    private static ActorRoute projectCanonicalActor(
            ZLinkAuthoritySnapshot snapshot, String actorId) {
        // snapshot.objectGeneration() is spec-bounded to `1..long.MaxValue`
        // (01-glossary "ObjectGeneration"), so `<= 0` never misclassifies
        // valid traffic. Aligned to `== 0` for consistency with the
        // equivalent check already fixed at ZLinkJavaRawSpotNode.createSpot
        // (commit b7443ed9b4). ownerLeaseGeneration is likewise
        // spec-bounded (positive `long`), so `<= 0` is correct for it.
        if (snapshot.objectGeneration() == 0
                || snapshot.ownerLeaseGeneration() <= 0
                || actorId == null
                || actorId.isBlank()
                || snapshot.allocation().stableType().isBlank()
                || snapshot.allocation().descriptor().meshName().isBlank()) {
            return null;
        }
        RoutingId nodeRid = snapshot.allocation().descriptor().rid();
        return new ActorRoute(
                new ActorRef(
                        actorId,
                        snapshot.objectGeneration(),
                        snapshot.allocation().descriptor().meshName(),
                        nodeRid),
                ZLinkSpotKind.ENTRY,
                "",
                0,
                snapshot.allocation().descriptor().meshName(),
                nodeRid,
                snapshot.allocation().descriptorLifecycleGeneration(),
                snapshot.authorityOwnerGeneration(),
                snapshot.ownerLeaseGeneration());
    }

    private <V> CompletionStage<V> admitPositiveRoute(
            Map<String, CachedRoute<V>> cache,
            String key,
            V value,
            String ownerId,
            long ownerLeaseGeneration,
            String storeVersion) {
        return admitPositiveRoute(
                cache, key, value, ownerId, ownerLeaseGeneration, storeVersion, null);
    }

    private <V> CompletionStage<V> admitPositiveRoute(
            Map<String, CachedRoute<V>> cache,
            String key,
            V value,
            String ownerId,
            long ownerLeaseGeneration,
            String storeVersion,
            SpotRouteRead pending) {
        return liveRows.ownerLeaseRemaining(ownerId, ownerLeaseGeneration)
                .thenCompose(
                        remaining -> {
                            if (remaining == null) {
                                invalidateOwnerLease(ownerId, ownerLeaseGeneration);
                                return CompletableFuture.failedFuture(
                                        new ZLinkFrameworkException(
                                                ZLinkFrameworkErrorKind.UNAVAILABLE,
                                                "location owner lease is unavailable: " + ownerId));
                            }
                            if (cache != null && !routeCacheMaxAge.isZero()) {
                                Duration lifetime =
                                        remaining.compareTo(routeCacheMaxAge) < 0
                                                ? remaining
                                                : routeCacheMaxAge;
                                CachedRoute<V> route =
                                        new CachedRoute<>(
                                                value,
                                                storeVersion,
                                                ownerId,
                                                ownerLeaseGeneration,
                                                System.nanoTime(),
                                                boundedNanos(lifetime));
                                cache.compute(
                                        key,
                                        (id, cached) -> {
                                            if (pending != null
                                                    && (spotRouteReads.get(id) != pending
                                                            || (pending.storeVersion != null
                                                                    && !pending.storeVersion.equals(
                                                                            storeVersion)))) {
                                                return cached;
                                            }
                                            return route;
                                        });
                            }
                            return CompletableFuture.completedFuture(value);
                        });
    }

    private static <V> void forget(Map<String, CachedRoute<V>> cache, String key) {
        if (cache != null) {
            cache.remove(key);
        }
    }

    private static <V> V cached(Map<String, CachedRoute<V>> cache, String key) {
        CachedRoute<V> route = cache.get(key);
        if (route == null) {
            return null;
        }
        if (System.nanoTime() - route.storedAtNanos() >= route.lifetimeNanos()) {
            cache.remove(key, route);
            return null;
        }
        return route.value();
    }

    private static long boundedNanos(Duration value) {
        try {
            return value.toNanos();
        } catch (ArithmeticException overflow) {
            return Long.MAX_VALUE;
        }
    }

    private <T> CompletionStage<T> observeAuthorityRead(CompletionStage<T> read) {
        return read.whenComplete(
                (ignored, failure) -> {
                    if (failure != null) {
                        authorityStoreFailure.set(true);
                    } else if (authorityStoreFailure.compareAndSet(true, false)) {
                        invalidateAllRoutes();
                    }
                });
    }

    private void invalidateOwnerLease(String ownerId, long ownerLeaseGeneration) {
        // An in-flight read may not yet have projected its owner; none may cache a
        // snapshot admitted before this owner lease invalidation.
        spotRouteReads
                .keySet()
                .forEach(
                        key ->
                                spotRoutes.compute(
                                        key,
                                        (id, cached) -> {
                                            spotRouteReads.remove(id);
                                            return cached;
                                        }));
        spotRoutes
                .entrySet()
                .removeIf(entry -> entry.getValue().sameOwner(ownerId, ownerLeaseGeneration));
        actorRoutes
                .entrySet()
                .removeIf(entry -> entry.getValue().sameOwner(ownerId, ownerLeaseGeneration));
    }

    @Override
    public void close() {
        List<PlacementObservation> pending;
        synchronized (placement) {
            closed = true;
            pending = List.copyOf(placement.values());
            placement.clear();
        }
        pending.forEach(
                observation ->
                        observation
                                .changed()
                                .completeExceptionally(
                                        new java.util.concurrent.CancellationException(
                                                "discovery closed")));
        invalidateAllRoutes();
    }

    public record SpotRoute(
            String meshName,
            String spotId,
            long spotGeneration,
            RoutingId nodeRid,
            long targetNodeGeneration,
            long authorityOwnerGeneration,
            long ownerLeaseGeneration,
            ZLinkSpotKind spotKind,
            String stableType,
            String ownerId,
            String storeVersion,
            ZLinkServiceAuthorityPayloadCodec.State authorityState) {}

    public record ActorRoute(
            ActorRef actorRef,
            ZLinkSpotKind locationKind,
            String spotId,
            long spotGeneration,
            String meshName,
            RoutingId nodeRid,
            long targetNodeGeneration,
            long authorityOwnerGeneration,
            long ownerLeaseGeneration) {}

    private record CachedRoute<V>(
            V value,
            String storeVersion,
            String ownerId,
            long ownerLeaseGeneration,
            long storedAtNanos,
            long lifetimeNanos) {
        private boolean sameOwner(String candidateOwnerId, long candidateLeaseGeneration) {
            return ownerId.equals(candidateOwnerId)
                    && ownerLeaseGeneration == candidateLeaseGeneration;
        }
    }

    public static final class AddressResolvers {
        private final ZLinkStoreLocationResolvers routes;

        public AddressResolvers(List<String> meshNames, ZLinkStoreLocationResolvers routes) {
            Objects.requireNonNull(meshNames, "meshNames");
            this.routes = Objects.requireNonNull(routes, "routes");
        }

        public CompletionStage<ActorRoute> resolveActor(String actorId) {
            return routes.resolveActor(actorId);
        }

        public CompletionStage<SpotRoute> resolveSpot(String meshName, String spotId) {
            return routes.resolveSpot(spotId)
                    .thenApply(
                            route ->
                                    route != null && route.meshName().equals(meshName)
                                            ? route
                                            : null);
        }

        public CompletionStage<SpotRoute> resolveSpot(String spotId) {
            return routes.resolveSpot(spotId);
        }

        public void invalidateSpotRoute(String spotId) {
            routes.invalidateSpotRoute(spotId);
        }

        public String routerChannelId(String meshName) {
            return meshName;
        }
    }
}
