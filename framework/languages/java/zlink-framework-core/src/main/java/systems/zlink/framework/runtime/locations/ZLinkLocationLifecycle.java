package systems.zlink.framework.runtime.locations;

import java.util.Map;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityExpectFound;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityPut;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityStored;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityGenerationTransition;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteStatus;
import systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation;
import systems.zlink.framework.spots.ZLinkSpotKind;

/** Tracks process-local materializations; durable ownership is stored only as authority. */
public final class ZLinkLocationLifecycle implements AutoCloseable {
    private static final ZLinkStoreCancellation NEVER_CANCEL = () -> false;
    private final Set<String> spots = ConcurrentHashMap.newKeySet();
    private final Map<String, ActorRef> actors = new ConcurrentHashMap<>();
    private final Set<RoutingId> sessionRoutes = ConcurrentHashMap.newKeySet();
    private final ZLinkLocationRepository store;
    private final ZLinkActorAuthorityPayloadCodec actorAuthorities =
        new ZLinkActorAuthorityPayloadCodec();
    private final ZLinkServiceAuthorityPayloadCodec spotAuthorities =
        new ZLinkServiceAuthorityPayloadCodec();

    public ZLinkLocationLifecycle(ZLinkLocationRuntime runtime) {
        this.store = java.util.Objects.requireNonNull(runtime, "runtime")
            .locationStore();
    }

    public CompletionStage<ZLinkLocationWriteStatus> claimSpot(
        String meshName,
        String spotId,
        long spotGeneration,
        String spotType,
        RoutingId nodeRid,
        ZLinkSpotKind spotKind,
        String routeEndpoint,
        Runnable deactivate) {
        spots.add(spotId);
        return CompletableFuture.completedFuture(ZLinkLocationWriteStatus.STORED);
    }

    public CompletionStage<ZLinkLocationWriteStatus> claimSpot(
        String meshName,
        String spotId,
        String spotType,
        RoutingId nodeRid,
        ZLinkSpotKind spotKind,
        String routeEndpoint,
        Runnable deactivate) {
        return claimSpot(meshName, spotId, 1L, spotType, nodeRid, spotKind, routeEndpoint, deactivate);
    }

    public CompletionStage<Void> releaseSpot(String meshName, String spotId) {
        spots.remove(spotId);
        return CompletableFuture.completedFuture(null);
    }

    public CompletionStage<ZLinkLocationWriteStatus> claimActor(
        String actorType,
        String actorId,
        RoutingId nodeRid,
        Runnable deactivate) {
        actors.putIfAbsent(actorId, nullActorRef(actorId, nodeRid));
        return CompletableFuture.completedFuture(ZLinkLocationWriteStatus.STORED);
    }

    public CompletionStage<ZLinkLocationWriteStatus> takeoverActor(
        String actorType,
        String actorId,
        RoutingId nodeRid,
        Runnable deactivate) {
        actors.put(actorId, nullActorRef(actorId, nodeRid));
        return CompletableFuture.completedFuture(ZLinkLocationWriteStatus.STORED);
    }

    public CompletionStage<Void> setActorRef(String actorType, String actorId, ActorRef actorRef) {
        actors.put(actorId, actorRef);
        return CompletableFuture.completedFuture(null);
    }

    public void abandonActor(String actorId) {
        actors.remove(actorId);
    }

    public CompletionStage<Void> notifyActorJoinedSpot(
        String actorType, String actorId, String meshName, String spotId) {
        ActorRef actorRef = actors.get(actorId);
        if (actorRef == null) {
            return CompletableFuture.failedFuture(new IllegalStateException(
                "Actor reference is unavailable for durable Spot join: "
                    + actorId));
        }
        String actorKey = ZLinkAuthorityKeyCodec.actor(actorId);
        String spotKey = ZLinkAuthorityKeyCodec.spot(spotId);
        return store.read(actorKey, NEVER_CANCEL).thenCompose(actorRead -> {
            if (!(actorRead instanceof ZLinkAuthoritySnapshot actorSnapshot)) {
                return CompletableFuture.failedFuture(new IllegalStateException(
                    "Actor authority is unavailable: " + actorId));
            }
            var actor = actorAuthorities.decode(actorSnapshot.payload())
                .orElseThrow(() -> new IllegalStateException(
                    "Actor authority payload is invalid: " + actorId));
            if (actor.state() != ZLinkActorAuthorityPayloadCodec.State.READY
                || !actor.actorId().equals(actorId)
                || !actor.stableType().equals(actorType)
                || actorSnapshot.objectGeneration()
                    != actorRef.objectGeneration()) {
                return CompletableFuture.failedFuture(new IllegalStateException(
                    "Actor authority changed before durable Spot join: "
                        + actorId));
            }
            return store.read(spotKey, NEVER_CANCEL).thenCompose(spotRead -> {
                if (!(spotRead instanceof ZLinkAuthoritySnapshot spotSnapshot)) {
                    return CompletableFuture.failedFuture(
                        new IllegalStateException(
                            "Spot authority is unavailable: " + spotId));
                }
                var spot = spotAuthorities.decode(spotSnapshot.payload())
                    .orElseThrow(() -> new IllegalStateException(
                        "Spot authority payload is invalid: " + spotId));
                if (spot.kind() != ZLinkServiceAuthorityPayloadCodec.Kind.USER
                    || spot.state()
                        != ZLinkServiceAuthorityPayloadCodec.State.READY
                    || !spot.spotId().equals(spotId)
                    || !spot.meshName().equals(meshName)
                    || !spot.nodeRid().equals(actorRef.nodeRid())) {
                    return CompletableFuture.failedFuture(
                        new IllegalStateException(
                            "Spot authority changed before durable Actor join: "
                                + spotId));
                }
                byte[] next = actorAuthorities.encode(
                    ZLinkActorAuthorityPayloadCodec.State.READY,
                    actor.stableType(),
                    actor.actorId(),
                    spot.spotId(),
                    spotSnapshot.objectGeneration(),
                    ZLinkSpotKind.USER.value(),
                    actor.ownerId(),
                    actor.ownerLeaseGeneration(),
                    spot.meshName(),
                    spot.nodeRid(),
                    spot.nodeGeneration());
                return store.compareExchange(
                        actorKey,
                        new ZLinkAuthorityExpectFound(
                            actorSnapshot.storeVersion()),
                        new ZLinkAuthorityPut(
                            next,
                            ZLinkAuthorityGenerationTransition.PRESERVE,
                            java.util.Optional.empty(),
                            java.util.Optional.empty()),
                        NEVER_CANCEL)
                    .thenCompose(result -> result instanceof ZLinkAuthorityStored
                        ? CompletableFuture.completedFuture(null)
                        : CompletableFuture.failedFuture(new IllegalStateException(
                            "Actor Spot join authority CAS conflicted: "
                                + actorId)));
            });
        });
    }

    public CompletionStage<Void> notifyActorLeftSpot(String actorType, String actorId) {
        return CompletableFuture.completedFuture(null);
    }

    public CompletionStage<Void> notifyActorMovedToEntrySpot(
        String actorType, String actorId, RoutingId nodeRid) {
        return CompletableFuture.completedFuture(null);
    }

    public CompletionStage<Void> releaseActor(String actorType, String actorId) {
        actors.remove(actorId);
        return CompletableFuture.completedFuture(null);
    }

    public CompletionStage<Void> bindActorSessionRoute(
        RoutingId sessionRid, String actorId, RoutingId ownerNodeRid) {
        sessionRoutes.add(sessionRid);
        return CompletableFuture.completedFuture(null);
    }

    public CompletionStage<Void> removeActorSessionRoute(RoutingId sessionRid) {
        sessionRoutes.remove(sessionRid);
        return CompletableFuture.completedFuture(null);
    }

    boolean ownsActor(String actorType, String actorId) {
        return actors.containsKey(actorId);
    }

    @Override
    public void close() {
        spots.clear();
        actors.clear();
        sessionRoutes.clear();
    }

    private static ActorRef nullActorRef(String actorId, RoutingId nodeRid) {
        return new ActorRef(actorId, 1L, "local", nodeRid);
    }
}
