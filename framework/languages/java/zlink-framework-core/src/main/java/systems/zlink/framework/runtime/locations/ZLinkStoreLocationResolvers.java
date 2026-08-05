package systems.zlink.framework.runtime.locations;

import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicBoolean;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.locations.ZLinkLocationOptions;
import systems.zlink.framework.locations.ZLinkLocationRole;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;
import systems.zlink.framework.locations.ZLinkPageRequest;
import systems.zlink.framework.runtime.internal.locations.ZLinkAutoConnectPeer;
import systems.zlink.framework.runtime.internal.locations.ZLinkAutoConnectPeerResolver;
import systems.zlink.framework.runtime.internal.locations.ZLinkAutoConnectType;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceMessageFollowWireCodec;
import systems.zlink.framework.spots.ZLinkSpotKind;

/** Resolves public handles from durable authority without legacy location rows. */
public final class ZLinkStoreLocationResolvers
    implements ZLinkAutoConnectPeerResolver, AutoCloseable {
    private final ZLinkRegisteredLocationStores stores;
    private final ZLinkLiveLocationRows liveRows;
    private final Duration routeCacheMaxAge;
    private final Map<String, CachedRoute<SpotRoute>> spotRoutes = new ConcurrentHashMap<>();
    private final Map<String, CachedRoute<ActorRoute>> actorRoutes = new ConcurrentHashMap<>();
    private final ZLinkServiceAuthorityPayloadCodec spotAuthorityCodec =
        new ZLinkServiceAuthorityPayloadCodec();
    private final ZLinkActorAuthorityPayloadCodec actorAuthorityCodec =
        new ZLinkActorAuthorityPayloadCodec();
    private final AtomicBoolean authorityStoreFailure = new AtomicBoolean();

    public ZLinkStoreLocationResolvers(
        ZLinkRegisteredLocationStores stores,
        ZLinkLocationOptions options) {
        this(stores, ZLinkLiveLocationRows.create(stores, options));
    }

    public ZLinkStoreLocationResolvers(
        ZLinkRegisteredLocationStores stores,
        ZLinkLiveLocationRows liveRows) {
        this.stores = Objects.requireNonNull(stores, "stores");
        this.liveRows = Objects.requireNonNull(liveRows, "liveRows");
        this.routeCacheMaxAge = liveRows.routeCacheMaxAge();
    }

    public CompletionStage<SpotRoute> resolveSpot(String spotId) {
        SpotRoute cached = cached(spotRoutes, spotId);
        if (cached != null) {
            return CompletableFuture.completedFuture(cached);
        }
        return observeAuthorityRead(stores.unifiedStore()
            .read(ZLinkAuthorityKeyCodec.spot(spotId), () -> false))
            .thenCompose(read -> resolveReadySpot(spotId, read));
    }

    public CompletionStage<ActorRoute> resolveActor(String actorId) {
        ActorRoute cached = cached(actorRoutes, actorId);
        if (cached != null) {
            return CompletableFuture.completedFuture(cached);
        }
        return observeAuthorityRead(stores.unifiedStore()
            .read(ZLinkAuthorityKeyCodec.actor(actorId), () -> false))
            .thenCompose(read -> resolveReadyActor(actorId, read));
    }

    public CompletionStage<DirectJoinSessionFence> resolveDirectJoinSessionFence(
        String actorId,
        RoutingId sessionOwnerNodeRid,
        RoutingId targetNodeRid) {
        invalidateActorRoute(actorId);
        return stores.unifiedStore()
            .read(ZLinkAuthorityKeyCodec.actor(actorId), () -> false)
            .thenCompose(read -> {
                if (!(read instanceof systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot snapshot)) {
                    return CompletableFuture.failedFuture(
                        new IllegalStateException(
                            "Actor authority is unavailable: " + actorId));
                }
                var authority =
                    actorAuthorityCodec.decode(snapshot.payload()).orElseThrow(
                        () -> new IllegalStateException(
                            "Actor authority payload is invalid: " + actorId));
                return listMeshNodes(authority.meshName(), null, new java.util.ArrayList<>())
                    .thenApply(nodes -> new DirectJoinSessionFence(
                        snapshot.storeVersion(),
                        snapshot.authorityOwnerGeneration(),
                        snapshot.ownerId(),
                        snapshot.ownerLeaseGeneration(),
                        requireNode(nodes, authority.nodeRid(), "source Actor owner"),
                        requireNode(nodes, sessionOwnerNodeRid, "Session owner"),
                        requireNode(nodes, targetNodeRid, "target Actor owner")));
            });
    }

    private static ZLinkMeshNodeDescriptor requireNode(
        List<ZLinkMeshNodeDescriptor> nodes,
        RoutingId rid,
        String role) {
        return nodes.stream()
            .filter(node -> node.rid().equals(rid))
            .findFirst()
            .orElseThrow(() -> new IllegalStateException(
                role + " descriptor is unavailable: " + rid));
    }

    @Override
    public CompletionStage<List<ZLinkAutoConnectPeer>> listPeers(
        ZLinkAutoConnectType type,
        String meshName,
        ZLinkLocationRole role) {
        return listLiveMeshNodes(meshName)
            .thenApply(nodes -> nodes.stream().map(node -> new ZLinkAutoConnectPeer(
                type,
                node.meshName(),
                node.rid(),
                role == null ? ZLinkLocationRole.ROUTER : role,
                node.endpoint(),
                node.placementWeight(),
                node.state() != systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState.SERVING,
                node.lifecycleGeneration(),
                Map.of(
                    ZLinkAutoConnectPlanner.SECURITY_IDENTITY_METADATA_KEY,
                    node.securityIdentity()),
                node.objectCapabilities().stream()
                    .filter(capability -> capability.objectKind()
                        == systems.zlink.framework.locations.ZLinkPlacementObjectKind.ACTOR)
                    .map(capability -> "actor:" + capability.stableType())
                    .toList(),
                node.ownerId(),
                node.leaseGeneration(),
                node.updatedAt(),
                node.objectRole(),
                !node.channelWeights().isEmpty())).toList());
    }

    private CompletionStage<List<ZLinkMeshNodeDescriptor>> listLiveMeshNodes(
        String meshName) {
        return listMeshNodes(meshName, null, new java.util.ArrayList<>())
            .thenCompose(nodes -> {
                List<CompletableFuture<Boolean>> liveness = nodes.stream()
                    .map(node -> liveRows.ownerLeaseRemaining(
                            node.ownerId(), node.leaseGeneration())
                        .thenApply(remaining -> remaining != null)
                        .toCompletableFuture())
                    .toList();
                return CompletableFuture.allOf(
                        liveness.toArray(CompletableFuture[]::new))
                    .thenApply(ignored -> java.util.stream.IntStream
                        .range(0, nodes.size())
                        .filter(index -> liveness.get(index).join())
                        .mapToObj(nodes::get)
                        .toList());
            });
    }

    private CompletionStage<List<ZLinkMeshNodeDescriptor>> listMeshNodes(
        String meshName,
        String continuation,
        List<ZLinkMeshNodeDescriptor> values) {
        return stores.unifiedStore().listMeshNodes(
            meshName, new ZLinkPageRequest(1000, continuation)).thenCompose(page -> {
                values.addAll(page.items());
                return page.continuationToken() == null
                    ? CompletableFuture.completedFuture(List.copyOf(values))
                    : listMeshNodes(meshName, page.continuationToken(), values);
            });
    }

    public CompletionStage<List<ZLinkMeshNodeDescriptor>> listMeshNodes(
        String meshName) {
        return listMeshNodes(meshName, null, new java.util.ArrayList<>());
    }

    public void invalidateActorRoute(String actorId) {
        actorRoutes.remove(Objects.requireNonNull(actorId, "actorId"));
    }

    public void invalidateSpotRoute(String spotId) {
        spotRoutes.remove(Objects.requireNonNull(spotId, "spotId"));
    }

    /**
     * Removes a cached route only when every fence field still matches the
     * cached value. A delayed notice from an older owner cannot erase a newer
     * positive route admitted after relocation.
     */
    public boolean invalidateRouteIfMatches(
        ZLinkServiceMessageFollowWireCodec.Route fence) {
        Objects.requireNonNull(fence, "fence");
        if (fence instanceof ZLinkServiceMessageFollowWireCodec.ActorRoute actor) {
            CachedRoute<ActorRoute> cached = actorRoutes.get(actor.actorId());
            if (cached == null
                || !(cached.value() instanceof ActorRoute route)
                || !route.actorRef().actorId().equals(actor.actorId())
                || route.actorRef().objectGeneration() != actor.objectGeneration()
                || !route.nodeRid().equals(actor.targetNodeRid())
                || route.targetNodeGeneration() != actor.targetNodeGeneration()
                || route.authorityOwnerGeneration()
                    != actor.authorityOwnerGeneration()
                || route.ownerLeaseGeneration()
                    != actor.ownerLeaseGeneration()
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
            || route.authorityOwnerGeneration()
                != spot.authorityOwnerGeneration()
            || route.ownerLeaseGeneration()
                != spot.ownerLeaseGeneration()
            || cached.ownerLeaseGeneration() != spot.ownerLeaseGeneration()) {
            return false;
        }
        return spotRoutes.remove(spot.spotId(), cached);
    }

    public boolean invalidateRouteIfMatches(
        ZLinkServiceMessageFollowWireCodec.Notice notice) {
        Objects.requireNonNull(notice, "notice");
        return invalidateRouteIfMatches(notice.source());
    }

    public void invalidateAllRoutes() {
        spotRoutes.clear();
        actorRoutes.clear();
    }

    private CompletionStage<SpotRoute> resolveReadySpot(String spotId, Object read) {
        if (!(read instanceof systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot snapshot)
            || snapshot.allocation().state()
                != systems.zlink.framework.runtime.internal.locations.ZLinkPlacementAllocationState.ACTIVE) {
            spotRoutes.remove(spotId);
            return CompletableFuture.completedFuture(null);
        }
        var authority = spotAuthorityCodec.decode(snapshot.payload()).orElse(null);
        if (authority == null
            || authority.state() != ZLinkServiceAuthorityPayloadCodec.State.READY
            || !authority.spotId().equals(spotId)
            || !authority.ownerId().equals(snapshot.ownerId())
            || authority.ownerLeaseGeneration() != snapshot.ownerLeaseGeneration()) {
            spotRoutes.remove(spotId);
            return CompletableFuture.completedFuture(null);
        }
        SpotRoute route = new SpotRoute(
            authority.meshName(),
            authority.spotId(),
            snapshot.objectGeneration(),
            authority.nodeRid(),
            authority.nodeGeneration(),
            snapshot.authorityOwnerGeneration(),
            snapshot.ownerLeaseGeneration(),
            authority.kind() == ZLinkServiceAuthorityPayloadCodec.Kind.USER
                ? ZLinkSpotKind.USER
                : ZLinkSpotKind.INSTANCE);
        return admitPositiveRoute(
            spotRoutes, spotId, route, snapshot.ownerId(),
            snapshot.ownerLeaseGeneration(), snapshot.storeVersion());
    }

    private CompletionStage<ActorRoute> resolveReadyActor(String actorId, Object read) {
        if (!(read instanceof systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot snapshot)
            || snapshot.allocation().state()
                != systems.zlink.framework.runtime.internal.locations.ZLinkPlacementAllocationState.ACTIVE
            || snapshot.allocation().objectKind()
                != systems.zlink.framework.locations.ZLinkPlacementObjectKind.ACTOR) {
            actorRoutes.remove(actorId);
            return CompletableFuture.completedFuture(null);
        }
        var authority = actorAuthorityCodec.decode(snapshot.payload()).orElse(null);
        if (authority == null
            || authority.state() != ZLinkActorAuthorityPayloadCodec.State.READY
            || !authority.actorId().equals(actorId)
            || !authority.ownerId().equals(snapshot.ownerId())
            || authority.ownerLeaseGeneration() != snapshot.ownerLeaseGeneration()) {
            actorRoutes.remove(actorId);
            return CompletableFuture.completedFuture(null);
        }
        ActorRoute route = new ActorRoute(
            new ActorRef(actorId, snapshot.objectGeneration(), authority.meshName(), authority.nodeRid()),
            authority.currentSpotKind() == 1 ? ZLinkSpotKind.ENTRY : ZLinkSpotKind.USER,
            authority.currentSpotId(),
            authority.meshName(),
            authority.nodeRid(),
            authority.nodeGeneration(),
            snapshot.authorityOwnerGeneration(),
            snapshot.ownerLeaseGeneration());
        return admitPositiveRoute(
            actorRoutes, actorId, route, snapshot.ownerId(),
            snapshot.ownerLeaseGeneration(), snapshot.storeVersion());
    }

    private <V> CompletionStage<V> admitPositiveRoute(
        Map<String, CachedRoute<V>> cache,
        String key,
        V value,
        String ownerId,
        long ownerLeaseGeneration,
        String storeVersion) {
        return liveRows.ownerLeaseRemaining(ownerId, ownerLeaseGeneration)
            .thenApply(remaining -> {
                if (remaining == null) {
                    invalidateOwnerLease(ownerId, ownerLeaseGeneration);
                    return null;
                }
                if (!routeCacheMaxAge.isZero()) {
                    Duration lifetime = remaining.compareTo(routeCacheMaxAge) < 0
                        ? remaining : routeCacheMaxAge;
                    cache.put(key, new CachedRoute<>(
                        value, storeVersion, ownerId, ownerLeaseGeneration,
                        System.nanoTime(), boundedNanos(lifetime)));
                }
                return value;
            });
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
        return read.whenComplete((ignored, failure) -> {
            if (failure != null) {
                authorityStoreFailure.set(true);
            } else if (authorityStoreFailure.compareAndSet(true, false)) {
                invalidateAllRoutes();
            }
        });
    }

    private void invalidateOwnerLease(String ownerId, long ownerLeaseGeneration) {
        spotRoutes.entrySet().removeIf(entry -> entry.getValue().sameOwner(ownerId, ownerLeaseGeneration));
        actorRoutes.entrySet().removeIf(entry -> entry.getValue().sameOwner(ownerId, ownerLeaseGeneration));
    }

    @Override
    public void close() {
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
        ZLinkSpotKind spotKind) {
    }

    public record ActorRoute(
        ActorRef actorRef,
        ZLinkSpotKind locationKind,
        String spotId,
        String meshName,
        RoutingId nodeRid,
        long targetNodeGeneration,
        long authorityOwnerGeneration,
        long ownerLeaseGeneration) {
    }

    public record DirectJoinSessionFence(
        String sourceAuthorityStoreVersion,
        long sourceAuthorityOwnerGeneration,
        String sourceAuthorityOwnerId,
        long sourceAuthorityOwnerLeaseGeneration,
        ZLinkMeshNodeDescriptor sourceActorOwner,
        ZLinkMeshNodeDescriptor sessionOwner,
        ZLinkMeshNodeDescriptor targetActorOwner) {
    }

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
            return routes.resolveSpot(spotId).thenApply(route ->
                route != null && route.meshName().equals(meshName) ? route : null);
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
