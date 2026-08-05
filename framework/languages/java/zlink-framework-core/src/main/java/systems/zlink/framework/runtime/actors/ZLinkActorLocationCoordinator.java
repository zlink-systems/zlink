package systems.zlink.framework.runtime.actors;

import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.Function;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteIntent;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteStatus;
import systems.zlink.framework.runtime.locations.ZLinkLocationLifecycle;
import systems.zlink.framework.runtime.locations.ZLinkStoreLocationResolvers;

final class ZLinkActorLocationCoordinator {
    private final Function<String, String> actorTypeResolver;
    private Function<String, String> spotMeshResolver = ignored -> null;
    private ZLinkLocationLifecycle lifecycle;
    private ZLinkStoreLocationResolvers resolvers;

    ZLinkActorLocationCoordinator(Function<String, String> actorTypeResolver) {
        this.actorTypeResolver = actorTypeResolver;
    }

    void setLifecycle(ZLinkLocationLifecycle lifecycle) {
        this.lifecycle = lifecycle;
    }

    void setResolvers(ZLinkStoreLocationResolvers resolvers) {
        this.resolvers = resolvers;
    }

    void setSpotMeshResolver(Function<String, String> spotMeshResolver) {
        this.spotMeshResolver = spotMeshResolver == null ? ignored -> null : spotMeshResolver;
    }

    boolean claimsActors(ZLinkLocationWriteIntent intent) {
        return lifecycle != null && intent != null;
    }

    CompletionStage<Void> claimActor(
        String actorType,
        String actorId,
        RoutingId ownerNodeRid,
        ZLinkLocationWriteIntent intent,
        Runnable ownershipLost) {
        if (lifecycle == null || intent == null) {
            return CompletableFuture.completedFuture(null);
        }
        CompletionStage<ZLinkLocationWriteStatus> claim = intent == ZLinkLocationWriteIntent.TAKEOVER
            ? lifecycle.takeoverActor(actorType, actorId, ownerNodeRid, ownershipLost)
            : lifecycle.claimActor(actorType, actorId, ownerNodeRid, ownershipLost);
        return claim.thenCompose(status -> {
            if (status == ZLinkLocationWriteStatus.STORED) {
                return CompletableFuture.completedFuture(null);
            }
            return CompletableFuture.failedFuture(actorCreateLocationFailure(actorId, status));
        });
    }

    CompletionStage<Void> setActorRef(
        String actorType,
        String actorId,
        ActorRef actorRef) {
        if (lifecycle == null) {
            return CompletableFuture.completedFuture(null);
        }
        return lifecycle.setActorRef(actorType, actorId, actorRef);
    }

    CompletionStage<Optional<ActorRef>> findStoredActorRef(String actorId) {
        if (resolvers == null) {
            return CompletableFuture.completedFuture(Optional.empty());
        }
        return resolvers.resolveActor(actorId)
            .thenApply(row -> row == null
                ? Optional.empty()
                : Optional.of(row.actorRef()));
    }

    //  Resolves the Spot an Actor currently belongs to. The Actor authority row
    //  already carries the current Spot id, so this reads the same row `find`
    //  uses instead of taking a second lookup path.
    CompletionStage<Optional<systems.zlink.framework.spots.SpotRef>>
        findStoredSpotRef(String actorId) {
        if (resolvers == null) {
            return CompletableFuture.completedFuture(Optional.empty());
        }
        return resolvers.resolveActor(actorId)
            .thenApply(row -> row == null
                    || row.spotId() == null
                    || row.spotId().isEmpty()
                ? Optional.empty()
                : Optional.of(new systems.zlink.framework.spots.SpotRef(
                    row.spotId(),
                    row.authorityOwnerGeneration(),
                    row.meshName(),
                    row.nodeRid())));
    }

    CompletionStage<ZLinkStoreLocationResolvers.ActorRoute>
        resolveStoredActorRoute(String actorId) {
        if (resolvers == null) {
            return CompletableFuture.failedFuture(
                new IllegalStateException(
                    "Location Store resolver is unavailable"));
        }
        resolvers.invalidateActorRoute(actorId);
        return resolvers.resolveActor(actorId);
    }

    CompletionStage<Optional<ActorRef>> findStoredActorRefExact(
        String actorId) {
        if (resolvers == null) {
            return CompletableFuture.completedFuture(Optional.empty());
        }
        resolvers.invalidateActorRoute(actorId);
        return findStoredActorRef(actorId);
    }

    CompletionStage<systems.zlink.framework.runtime.locations
        .ZLinkStoreLocationResolvers.DirectJoinSessionFence>
        directJoinSessionFence(
            String actorId,
            RoutingId sessionOwnerNodeRid,
            RoutingId targetNodeRid) {
        if (resolvers == null) {
            return CompletableFuture.failedFuture(
                new IllegalStateException(
                    "Location Store resolver is unavailable"));
        }
        return resolvers.resolveDirectJoinSessionFence(
            actorId, sessionOwnerNodeRid, targetNodeRid);
    }

    CompletionStage<Void> actorJoinedSpot(ZLinkActor actor, String spotId) {
        if (lifecycle == null) {
            return CompletableFuture.completedFuture(null);
        }
        String actorType = actorTypeFor(actor);
        if (actorType == null) {
            return CompletableFuture.completedFuture(null);
        }
        String meshName = spotMeshResolver.apply(spotId);
        if (meshName == null || meshName.isBlank()) {
            return CompletableFuture.completedFuture(null);
        }
        CompletionStage<Void> joined = lifecycle.notifyActorJoinedSpot(
            actorType, actor.context().actorId(), meshName, spotId);
        return joined.thenRun(() -> {
            if (resolvers != null) {
                resolvers.invalidateActorRoute(actor.context().actorId());
            }
        });
    }

    CompletionStage<Void> actorLeftSpot(ZLinkActor actor) {
        if (lifecycle == null) {
            return CompletableFuture.completedFuture(null);
        }
        String actorType = actorTypeFor(actor);
        if (actorType == null) {
            return CompletableFuture.completedFuture(null);
        }
        return lifecycle.notifyActorLeftSpot(actorType, actor.context().actorId());
    }

    CompletionStage<Void> actorMovedToEntrySpot(ZLinkActor actor, RoutingId nodeRid) {
        if (lifecycle == null) {
            return CompletableFuture.completedFuture(null);
        }
        String actorType = actorTypeFor(actor);
        if (actorType == null) {
            return CompletableFuture.completedFuture(null);
        }
        return lifecycle.notifyActorMovedToEntrySpot(actorType, actor.context().actorId(), nodeRid);
    }

    CompletionStage<Void> releaseActor(String actorType, String actorId) {
        if (lifecycle == null) {
            return CompletableFuture.completedFuture(null);
        }
        return lifecycle.releaseActor(actorType == null ? "" : actorType, actorId);
    }

    void abandonActor(String actorId) {
        if (lifecycle != null) {
            lifecycle.abandonActor(actorId);
        }
    }

    void bindSessionRoute(RoutingId routeSessionRid, String actorId, RoutingId sourceNodeRid) {
        if (lifecycle == null || routeSessionRid == null || sourceNodeRid == null) {
            return;
        }
        lifecycle.bindActorSessionRoute(routeSessionRid, actorId, sourceNodeRid)
            .exceptionally(error -> null);
    }

    void removeSessionRoute(RoutingId routeSessionRid) {
        if (lifecycle == null || routeSessionRid == null) {
            return;
        }
        lifecycle.removeActorSessionRoute(routeSessionRid)
            .exceptionally(error -> null);
    }

    private String actorTypeFor(ZLinkActor actor) {
        String actorType = actorTypeResolver.apply(actor.context().actorId());
        return actorType == null || actorType.isBlank() ? null : actorType;
    }

    private static RuntimeException actorCreateLocationFailure(
        String actorId,
        ZLinkLocationWriteStatus status) {
        if (status == ZLinkLocationWriteStatus.REJECTED_CONFLICT) {
            return new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.REJECTED,
                "Actor '" + actorId + "' location is owned by another runtime.");
        }
        return new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.INTERNAL_FAILURE,
            "Actor '" + actorId + "' location claim failed because the location store is unavailable.");
    }
}
