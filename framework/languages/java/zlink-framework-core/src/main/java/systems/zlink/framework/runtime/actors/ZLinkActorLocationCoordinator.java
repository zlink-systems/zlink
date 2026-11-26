package systems.zlink.framework.runtime.actors;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteIntent;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteStatus;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectCreationDeadline;
import systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec.ActorRouteFence;
import systems.zlink.framework.runtime.locations.ZLinkActorAuthorityPayloadCodec;
import systems.zlink.framework.runtime.locations.ZLinkLocationLifecycle;
import systems.zlink.framework.runtime.locations.ZLinkStoreLocationResolvers;
import systems.zlink.framework.spots.SpotRef;

import java.time.Duration;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.Function;

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

    boolean hasStoreResolver() {
        return resolvers != null;
    }

    void setSpotMeshResolver(Function<String, String> spotMeshResolver) {
        this.spotMeshResolver = spotMeshResolver == null ? ignored -> null : spotMeshResolver;
    }

    boolean claimsActors(ZLinkLocationWriteIntent intent) {
        return lifecycle != null && intent != null;
    }

    systems.zlink.framework.runtime.locations.ZLinkLocationRuntime.Execution executionAsync(
            String key,
            java.util.function.BooleanSupplier steady,
            java.util.function.Function<
                            systems.zlink.framework.runtime.internal.locations
                                    .ZLinkAuthoritySnapshot,
                            CompletionStage<Void>>
                    apply,
            java.util.function.Supplier<String> parent,
            java.util.function.Supplier<CompletionStage<Void>> cleanup) {
        return lifecycle == null
                ? null
                : lifecycle.executionAsync(key, steady, apply, parent, cleanup);
    }

    void ensureOwnerAdmissionOpen() {
        if (lifecycle != null) {
            lifecycle.ensureOwnerAdmissionOpen();
        }
    }

    boolean isOwnerAdmissionOpen() {
        return lifecycle == null || lifecycle.isOwnerAdmissionOpen();
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
        CompletionStage<ZLinkLocationWriteStatus> claim =
                intent == ZLinkLocationWriteIntent.TAKEOVER
                        ? lifecycle.takeoverActor(actorType, actorId, ownerNodeRid, ownershipLost)
                        : lifecycle.claimActor(actorType, actorId, ownerNodeRid, ownershipLost);
        return claim.thenCompose(
                status -> {
                    if (status == ZLinkLocationWriteStatus.STORED) {
                        return CompletableFuture.completedFuture(null);
                    }
                    return CompletableFuture.failedFuture(
                            actorCreateLocationFailure(actorId, status));
                });
    }

    CompletionStage<Void> setActorRef(String actorType, String actorId, ActorRef actorRef) {
        if (lifecycle == null) {
            return CompletableFuture.completedFuture(null);
        }
        return lifecycle.setActorRef(actorType, actorId, actorRef);
    }

    CompletionStage<Optional<ActorRef>> findStoredActorRef(String actorId) {
        ensureOwnerAdmissionOpen();
        if (resolvers == null) {
            return CompletableFuture.completedFuture(Optional.empty());
        }
        return resolvers
                .resolveActor(actorId)
                .thenApply(row -> row == null ? Optional.empty() : Optional.of(row.actorRef()));
    }

    //  Resolves the Spot an Actor currently belongs to. The Actor authority row
    //  already carries the current Spot id, so this reads the same row `find`
    //  uses instead of taking a second lookup path.
    CompletionStage<Optional<SpotRef>> findStoredSpotRef(String actorId) {
        ensureOwnerAdmissionOpen();
        if (resolvers == null) {
            return CompletableFuture.completedFuture(Optional.empty());
        }
        return resolvers
                .resolveActor(actorId)
                .thenApply(
                        row ->
                                row == null || row.spotId() == null || row.spotId().isEmpty()
                                        ? Optional.empty()
                                        : Optional.of(
                                                new SpotRef(
                                                        row.spotId(),
                                                        row.spotGeneration(),
                                                        row.meshName(),
                                                        row.nodeRid())));
    }

    CompletionStage<ZLinkStoreLocationResolvers.ActorRoute> resolveStoredActorRoute(
            String actorId) {
        if (resolvers == null) {
            return CompletableFuture.failedFuture(
                    new IllegalStateException("Location Store resolver is unavailable"));
        }
        return resolvers.readActor(actorId);
    }

    CompletionStage<ZLinkStoreLocationResolvers.ActorRoute> resolveSessionBindingRoute(
            ZLinkBackendActorRef actor) {
        if (resolvers == null) {
            return CompletableFuture.failedFuture(
                    new IllegalStateException("Location Store resolver is unavailable"));
        }
        return resolvers.readActorSessionBindingRoute(actor);
    }

    CompletionStage<Optional<ActorRef>> findStoredActorRefExact(String actorId) {
        ensureOwnerAdmissionOpen();
        if (resolvers == null) {
            return CompletableFuture.completedFuture(Optional.empty());
        }
        return resolvers
                .findActor(actorId)
                .thenApply(row -> row == null ? Optional.empty() : Optional.of(row.actorRef()));
    }

    CompletionStage<Void> actorJoinedSpot(
            ZLinkActor actor,
            String spotId,
            systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation
                    cancellation) {
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
        return lifecycle.notifyActorJoinedSpot(
                actorType, actor.context().actorId(), meshName, spotId, cancellation);
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

    CompletionStage<Void> actorMovedToEntrySpot(
            ZLinkActor actor,
            RoutingId nodeRid,
            String entrySpotId,
            long entrySpotGeneration,
            systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation
                    cancellation) {
        if (lifecycle == null) {
            return CompletableFuture.completedFuture(null);
        }
        String actorType = actorTypeFor(actor);
        if (actorType == null) {
            return CompletableFuture.completedFuture(null);
        }
        return lifecycle.notifyActorMovedToEntrySpot(
                actorType,
                actor.context().actorId(),
                nodeRid,
                entrySpotId,
                entrySpotGeneration,
                cancellation);
    }

    CompletionStage<Void> releaseActor(String actorType, String actorId) {
        if (lifecycle == null) {
            return CompletableFuture.completedFuture(null);
        }
        return lifecycle.releaseActor(actorType == null ? "" : actorType, actorId);
    }

    CompletionStage<Boolean> destroy(
            ActorRef requested,
            ZLinkInternalMeshNode node,
            Duration timeout,
            Function<ActorRef, CompletionStage<Boolean>> localDestroy) {
        if (lifecycle == null || node == null) {
            return localDestroy.apply(requested);
        }
        long deadline = Math.addExact(System.currentTimeMillis(), timeout.toMillis());
        ZLinkStoreCancellation open = () -> System.currentTimeMillis() >= deadline;
        return ZLinkObjectCreationDeadline.classify(
                lifecycle
                        .readActorDestroyAuthority(requested.actorId(), open)
                        .thenCompose(
                                read -> {
                                    if (!(read instanceof ZLinkAuthoritySnapshot snapshot)) {
                                        return localDestroy.apply(requested);
                                    }
                                    if (snapshot.objectGeneration()
                                            != requested.objectGeneration()) {
                                        throw new ZLinkFrameworkException(
                                                ZLinkFrameworkErrorKind.INVALID_OPERATION,
                                                "Actor destroy generation is stale: "
                                                        + requested.actorId());
                                    }
                                    var authority = destroyAuthority(requested.actorId(), snapshot);
                                    var target = destroyFence(snapshot, authority);
                                    if (target.actor()
                                            .nodeRid()
                                            .equals(node.status().routingId())) {
                                        if (authority.state()
                                                == ZLinkActorAuthorityPayloadCodec.State.CLOSING) {
                                            return localDestroy
                                                    .apply(requested)
                                                    .thenCompose(
                                                            destroyed ->
                                                                    destroyed
                                                                            ? CompletableFuture
                                                                                    .completedFuture(
                                                                                            true)
                                                                            : lifecycle
                                                                                    .releaseEndedActorClosing(
                                                                                            requested
                                                                                                    .actorId(),
                                                                                            snapshot,
                                                                                            open));
                                        }
                                        return localDestroy.apply(requested);
                                    }
                                    var release =
                                            authority.state()
                                                            == ZLinkActorAuthorityPayloadCodec.State
                                                                    .CLOSING
                                                    ? lifecycle.releaseEndedActorClosing(
                                                            requested.actorId(), snapshot, open)
                                                    : CompletableFuture.completedFuture(false);
                                    return release.thenCompose(
                                            released ->
                                                    released
                                                            ? CompletableFuture.completedFuture(
                                                                    true)
                                                            : node.sendActorDestroy(target)
                                                                    .thenCompose(
                                                                            ignored ->
                                                                                    observeDestroy(
                                                                                            requested,
                                                                                            open)));
                                }),
                deadline);
    }

    CompletionStage<Void> receiveDestroy(
            ActorRouteFence requested, Function<ActorRef, CompletionStage<Boolean>> localDestroy) {
        String actorId = requested.actor().actorId();
        return lifecycle
                .readActorDestroyAuthority(actorId, () -> false)
                .thenCompose(
                        read -> {
                            if (!(read instanceof ZLinkAuthoritySnapshot snapshot)) {
                                return CompletableFuture.completedFuture(null);
                            }
                            if (requested.actor().generation() != snapshot.objectGeneration()) {
                                throw new ZLinkFrameworkException(
                                        ZLinkFrameworkErrorKind.INVALID_OPERATION,
                                        "Actor destroy generation is stale: " + actorId);
                            }
                            var authority = destroyAuthority(actorId, snapshot);
                            if (!requested.equals(destroyFence(snapshot, authority))) {
                                throw new ZLinkFrameworkException(
                                        ZLinkFrameworkErrorKind.UNAVAILABLE,
                                        "Actor destroy owner fence changed: " + actorId);
                            }
                            return localDestroy
                                    .apply(
                                            new ActorRef(
                                                    actorId,
                                                    snapshot.objectGeneration(),
                                                    authority.meshName(),
                                                    authority.nodeRid()))
                                    .thenApply(ignored -> null);
                        });
    }

    private CompletionStage<Boolean> observeDestroy(
            ActorRef requested, ZLinkStoreCancellation cancellation) {
        if (cancellation.isCancellationRequested()) {
            return CompletableFuture.failedFuture(ZLinkObjectCreationDeadline.exceeded(null));
        }
        return lifecycle
                .readActorDestroyAuthority(requested.actorId(), cancellation)
                .thenCompose(
                        read -> {
                            if (!(read instanceof ZLinkAuthoritySnapshot snapshot)
                                    || snapshot.objectGeneration() > requested.objectGeneration()) {
                                return CompletableFuture.completedFuture(true);
                            }
                            var released =
                                    ZLinkAuthoritySnapshot.actorClosing(
                                                    snapshot.payload(), snapshot.allocation())
                                            ? lifecycle.releaseEndedActorClosing(
                                                    requested.actorId(), snapshot, cancellation)
                                            : CompletableFuture.completedFuture(false);
                            return released.thenCompose(
                                    done ->
                                            done
                                                    ? CompletableFuture.completedFuture(true)
                                                    : ZLinkActorCreationCoordinator.awaitConflict()
                                                            .thenCompose(
                                                                    ignored ->
                                                                            observeDestroy(
                                                                                    requested,
                                                                                    cancellation)));
                        });
    }

    private static ZLinkActorAuthorityPayloadCodec.ActorAuthority destroyAuthority(
            String actorId, ZLinkAuthoritySnapshot snapshot) {
        if (snapshot.relocationInProgress()) {
            throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.UNAVAILABLE,
                    "Actor is sealed for relocation: " + actorId);
        }
        var authority =
                new ZLinkActorAuthorityPayloadCodec()
                        .decode(snapshot.payload())
                        .orElseThrow(
                                () ->
                                        new ZLinkFrameworkException(
                                                ZLinkFrameworkErrorKind.INTERNAL_FAILURE,
                                                "Actor authority payload is invalid: " + actorId));
        if (authority.state() == ZLinkActorAuthorityPayloadCodec.State.CREATING) {
            throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.UNAVAILABLE, "Actor is still creating: " + actorId);
        }
        return authority;
    }

    private static ActorRouteFence destroyFence(
            ZLinkAuthoritySnapshot snapshot,
            ZLinkActorAuthorityPayloadCodec.ActorAuthority authority) {
        return new ActorRouteFence(
                new ZLinkBackendActorRef(
                        authority.nodeRid(), authority.actorId(), snapshot.objectGeneration()),
                authority.nodeGeneration(),
                snapshot.authorityOwnerGeneration(),
                snapshot.ownerLeaseGeneration());
    }

    CompletionStage<Void> publishActorClosing(
            String actorType, ActorRef actor, long nodeGeneration) {
        return lifecycle == null
                ? CompletableFuture.completedFuture(null)
                : lifecycle.publishActorClosing(actorType, actor, nodeGeneration);
    }

    CompletionStage<Void> releaseActorExact(String actorType, ActorRef actor) {
        if (lifecycle == null) {
            return CompletableFuture.completedFuture(null);
        }
        return lifecycle.releaseActorExact(actorType == null ? "" : actorType, actor);
    }

    void abandonActor(String actorId) {
        if (lifecycle != null) {
            lifecycle.abandonActor(actorId);
        }
    }

    CompletionStage<Void> bindSessionRoute(
            RoutingId routeSessionRid, String actorId, RoutingId sourceNodeRid) {
        if (lifecycle == null || routeSessionRid == null || sourceNodeRid == null) {
            return CompletableFuture.completedFuture(null);
        }
        return lifecycle.bindActorSessionRoute(routeSessionRid, actorId, sourceNodeRid);
    }

    CompletionStage<Void> removeSessionRoute(RoutingId routeSessionRid) {
        if (lifecycle == null || routeSessionRid == null) {
            return CompletableFuture.completedFuture(null);
        }
        return lifecycle.removeActorSessionRoute(routeSessionRid);
    }

    private String actorTypeFor(ZLinkActor actor) {
        String actorType = actorTypeResolver.apply(actor.context().actorId());
        return actorType == null || actorType.isBlank() ? null : actorType;
    }

    private static RuntimeException actorCreateLocationFailure(
            String actorId, ZLinkLocationWriteStatus status) {
        if (status == ZLinkLocationWriteStatus.REJECTED_CONFLICT) {
            return new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.REJECTED,
                    "Actor '" + actorId + "' location is owned by another runtime.");
        }
        return new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.INTERNAL_FAILURE,
                "Actor '"
                        + actorId
                        + "' location claim failed because the location store is unavailable.");
    }
}
