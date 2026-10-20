package systems.zlink.framework.runtime.actors;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkBoundSession;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.spots.ZLinkSpot;

import java.time.Duration;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkActorContextState {
    private ZLinkActor actor;
    private final String actorId;
    private final String meshName;
    private ZLinkBackendActorRef actorRef;
    private ZLinkBoundSession boundSession;
    // The local token identifies this context attachment for cleanup. The SpotNode record
    // owns the Session-issued binding fence, including across relocation.
    private long sessionBindingToken;
    private RoutingId entrySpotNodeRid;
    private String entrySpotId;
    private String entryRouterChannelId;
    private String spotId;
    private ZLinkSpot<?> spot;
    private boolean joined;
    private boolean destroying;
    private boolean moving;
    private final AtomicReference<Object> deferredJoinClaim = new AtomicReference<>();
    private CompletableFuture<Void> moveCompletion = CompletableFuture.completedFuture(null);

    ZLinkActorContextState(ZLinkBackendActorRef actorRef, String meshName, String entrySpotId) {
        this.actorId = actorRef.actorId();
        this.meshName = meshName;
        this.actorRef = actorRef;
        this.entrySpotNodeRid = actorRef.nodeRid();
        this.entrySpotId = entrySpotId;
    }

    ZLinkActor actor() {
        return actor;
    }

    void setActor(ZLinkActor actor) {
        this.actor = actor;
    }

    String actorId() {
        return actorId;
    }

    long objectGeneration() {
        return actorRef.generation();
    }

    String meshName() {
        return meshName;
    }

    ZLinkBackendActorRef actorRef() {
        return actorRef;
    }

    RoutingId boundSessionSourceSessionRid() {
        var route = boundSessionRoute();
        return route == null ? null : route.sessionRid();
    }

    String spotId() {
        return spotId;
    }

    RoutingId entrySpotNodeRid() {
        return entrySpotNodeRid;
    }

    String entrySpotId() {
        return entrySpotId;
    }

    String entryRouterChannelId() {
        return entryRouterChannelId;
    }

    void setEntrySpotNodeRid(RoutingId entrySpotNodeRid) {
        if (entrySpotNodeRid == null) {
            throw new ZLinkConfigurationException("entrySpotNodeRid is required");
        }
        this.entrySpotNodeRid = entrySpotNodeRid;
    }

    void setEntrySpotId(String entrySpotId) {
        if (entrySpotId == null) {
            throw new ZLinkConfigurationException("entrySpotId is required");
        }
        this.entrySpotId = entrySpotId;
    }

    void setEntryRouterChannelId(String entryRouterChannelId) {
        if (entryRouterChannelId != null && !entryRouterChannelId.isBlank()) {
            this.entryRouterChannelId = entryRouterChannelId;
        }
    }

    boolean joined() {
        return joined;
    }

    boolean moving() {
        return moving;
    }

    void beginMove() {
        if (moving) {
            //  Spec 15-spot-actor:374 — the Actor is already moving: Unavailable.
            throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.UNAVAILABLE, "actor is already moving: " + actorId);
        }
        moving = true;
        moveCompletion = new CompletableFuture<>();
    }

    void endMove() {
        moving = false;
        moveCompletion.complete(null);
    }

    void failMove(Throwable error) {
        moving = false;
        moveCompletion.completeExceptionally(error);
    }

    CompletionStage<Void> moveCompletion() {
        return moveCompletion;
    }

    boolean tryClaimDeferredJoin(Object claim) {
        return claim != null && deferredJoinClaim.compareAndSet(null, claim);
    }

    void releaseDeferredJoin(Object claim) {
        if (claim != null) {
            deferredJoinClaim.compareAndSet(claim, null);
        }
    }

    /**
     * The SpotNode that holds this Actor's current binding record, or {@code null} when the binding
     * has no SpotNode record. That record owns the binding facts, including the Session owner
     * identity (Session-Actor binding §4).
     */
    ZLinkInternalSpotNode bindingSpotNode() {
        if (boundSession instanceof ZLinkNativeBoundSessionRuntime runtime) {
            return runtime.spotNode();
        }
        if (boundSession instanceof ZLinkBoundSessionRuntime runtime) {
            return runtime.spotNode();
        }
        return null;
    }

    ZLinkBoundSession requireBoundSession() {
        if (boundSession == null) {
            //  Spec 32-framework-error-model:41 — an operation that requires a
            //  bound session in a state that has none is InvalidOperation.
            throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.INVALID_OPERATION,
                    "actor has no bound session: " + actorId);
        }
        return boundSession;
    }

    /**
     * The session the Actor context hands to application code.
     *
     * <p>Spec 04-actor-model §8.1 puts a bound-session failure at the call's terminal, like every
     * other call failure, rather than in the accessor that creates the call. With no binding this
     * returns a session whose send and disconnect complete with {@code InvalidOperation}, so the
     * caller observes it through the completion stage instead of a throw at the accessor.
     */
    ZLinkBoundSession boundSessionOrUnbound() {
        return boundSession == null ? new ZLinkUnboundSession(actorId) : boundSession;
    }

    ZLinkSpot<?> requireSpot() {
        if (spot == null) {
            throw new ZLinkConfigurationException("actor has not joined a user Spot");
        }
        return spot;
    }

    ZLinkSpot<?> spot() {
        return spot;
    }

    void markJoined(ZLinkBackendActorRef actorRef, String spotId, ZLinkSpot<?> spot) {
        this.actorRef = actorRef;
        updateNativeBoundSessionActorRef(actorRef);
        this.spotId = spotId;
        this.spot = spot;
        this.joined = true;
    }

    void markMovedToEntrySpot(
            ZLinkBackendActorRef actorRef, RoutingId entrySpotNodeRid, String entrySpotId) {
        this.actorRef = actorRef;
        updateNativeBoundSessionActorRef(actorRef);
        this.entrySpotNodeRid = entrySpotNodeRid;
        this.entrySpotId = entrySpotId;
        this.spotId = entrySpotId;
        this.spot = null;
        this.joined = true;
    }

    void markNativeActorRef(ZLinkBackendActorRef actorRef) {
        this.actorRef = actorRef;
    }

    void markLeft() {
        spotId = null;
        spot = null;
        joined = false;
    }

    long bindSession(ZLinkBoundSession boundSession) {
        this.boundSession = boundSession;
        return ++sessionBindingToken;
    }

    ZLinkInternalSpotNode.BoundSessionRoute boundSessionRoute() {
        var node = bindingSpotNode();
        return node == null || actorRef == null
                ? null
                : node.boundSessionRoute(actorRef).orElse(null);
    }

    boolean clearBoundSession(long bindingToken) {
        if (boundSession != null && bindingToken == sessionBindingToken) {
            boundSession = null;
            return true;
        }
        return false;
    }

    long sessionBindingToken() {
        return sessionBindingToken;
    }

    CompletionStage<Void> rebindNativeActor(ZLinkBackendActorRef targetActor, Duration timeout) {
        if (boundSession instanceof ZLinkBoundSessionRuntime runtime) {
            return runtime.rebindNativeActor(targetActor, timeout);
        }
        updateNativeBoundSessionActorRef(targetActor);
        return CompletableFuture.completedFuture(null);
    }

    void updateNativeBoundSessionActorRef(ZLinkBackendActorRef targetActor) {
        if (boundSession instanceof ZLinkBoundSessionRuntime runtime) {
            runtime.markNativeRebound(targetActor);
        }
        if (boundSession instanceof ZLinkNativeBoundSessionRuntime runtime) {
            runtime.updateActorRef(targetActor);
        }
    }

    boolean hasBoundSession() {
        return boundSession != null;
    }

    boolean hasBoundSession(RoutingId sourceNodeRid, RoutingId sourceSessionRid) {
        var route = boundSessionRoute();
        return route != null
                && Objects.equals(route.sessionOwnerNodeRid(), sourceNodeRid)
                && Objects.equals(route.sessionRid(), sourceSessionRid);
    }

    CompletionStage<Boolean> sendBoundSessionFrame(byte[] frameBytes) {
        if (boundSession instanceof ZLinkNativeBoundSessionRuntime runtime) {
            return runtime.sendFrame(frameBytes).thenApply(ignored -> true);
        }
        return CompletableFuture.completedFuture(false);
    }

    CompletionStage<Void> disconnectBoundSessionForDestroy() {
        ZLinkBoundSession current = boundSession;
        if (current == null) {
            return CompletableFuture.completedFuture(null);
        }
        return current.disconnect();
    }

    void clearAfterDestroy() {
        actorRef = null;
        boundSession = null;
        entrySpotNodeRid = null;
        entrySpotId = null;
        entryRouterChannelId = null;
        sessionBindingToken++;
        spotId = null;
        spot = null;
        joined = false;
        destroying = false;
        moving = false;
        moveCompletion.completeExceptionally(
                new ZLinkConfigurationException("actor was destroyed while moving: " + actorId));
    }

    ZLinkBackendActorRef beginDestroy(RoutingId entryNodeRid, String actorId) {
        if (destroying) {
            return null;
        }
        if (actorRef == null) {
            throw new ZLinkConfigurationException(
                    "actor does not have a native Actor ref: " + actorId);
        }
        if (spot != null) {
            throw new ZLinkConfigurationException(
                    "actor must leave its current Spot before destroy: " + actorId);
        }
        if (!actorRef.nodeRid().equals(entryNodeRid)) {
            throw new ZLinkConfigurationException(
                    "actor is not owned by this Entry Spot: " + actorId);
        }

        destroying = true;
        return actorRef;
    }

    void resetDestroying() {
        destroying = false;
    }
}
