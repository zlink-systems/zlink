package systems.zlink.framework.runtime.actors;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicReference;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkBoundSession;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.spots.ZLinkSpot;

final class ZLinkActorContextState {
    private ZLinkActor actor;
    private final String actorId;
    private final String meshName;
    private ZLinkBackendActorRef actorRef;
    private ZLinkBoundSession boundSession;
    private long sessionBindingToken;
    private long sessionSourceSequence;
    private RoutingId boundSessionSourceNodeRid;
    private RoutingId boundSessionSourceSessionRid;
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

    ZLinkActorContextState(
        ZLinkBackendActorRef actorRef,
        String meshName,
        String entrySpotId) {
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

    RoutingId boundSessionSourceNodeRid() {
        return boundSessionSourceNodeRid;
    }

    RoutingId boundSessionSourceSessionRid() {
        return boundSessionSourceSessionRid;
    }

    RoutingId boundSessionSourceSessionRid(long bindingToken) {
        return bindingToken == sessionBindingToken ? boundSessionSourceSessionRid : null;
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
            throw new ZLinkConfigurationException("actor is already moving: " + actorId);
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

    ZLinkBoundSession requireBoundSession() {
        if (boundSession == null) {
            throw new ZLinkConfigurationException("actor has no bound session: " + actorId);
        }
        return boundSession;
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

    void markJoined(
        ZLinkBackendActorRef actorRef,
        String spotId,
        ZLinkSpot<?> spot) {
        this.actorRef = actorRef;
        updateNativeBoundSessionActorRef(actorRef);
        this.spotId = spotId;
        this.spot = spot;
        this.joined = true;
    }

    void markMovedToEntrySpot(
        ZLinkBackendActorRef actorRef,
        RoutingId entrySpotNodeRid,
        String entrySpotId) {
        this.actorRef = actorRef;
        updateNativeBoundSessionActorRef(actorRef);
        this.entrySpotNodeRid = entrySpotNodeRid;
        this.entrySpotId = entrySpotId;
        this.spotId = entrySpotId;
        this.spot = null;
        this.joined = true;
    }

    void markNativeActorRef(
        ZLinkBackendActorRef actorRef,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid) {
        this.actorRef = actorRef;
        this.boundSessionSourceNodeRid = sourceNodeRid;
        this.boundSessionSourceSessionRid = sourceSessionRid;
    }

    void markLeft() {
        spotId = null;
        spot = null;
        joined = false;
    }

    long bindSession(
        ZLinkBoundSession boundSession,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid) {
        boundSessionSourceNodeRid = sourceNodeRid;
        boundSessionSourceSessionRid = sourceSessionRid;
        sessionBindingToken++;
        sessionSourceSequence = 0;
        this.boundSession = boundSession;
        return sessionBindingToken;
    }

    BoundSessionSource nextBoundSessionSource() {
        if (boundSession == null
            || boundSessionSourceNodeRid == null
            || boundSessionSourceSessionRid == null
            || sessionBindingToken <= 0
            || sessionSourceSequence == Long.MAX_VALUE) {
            return null;
        }
        sessionSourceSequence++;
        return new BoundSessionSource(
            boundSessionSourceNodeRid,
            boundSessionSourceSessionRid,
            sessionBindingToken,
            sessionSourceSequence);
    }

    BoundSessionSource boundSessionSourceSnapshot() {
        if (boundSession == null
            || boundSessionSourceNodeRid == null
            || boundSessionSourceSessionRid == null
            || sessionBindingToken <= 0) {
            return null;
        }
        return new BoundSessionSource(
            boundSessionSourceNodeRid,
            boundSessionSourceSessionRid,
            sessionBindingToken,
            sessionSourceSequence);
    }

    boolean clearBoundSession(long bindingToken) {
        if (bindingToken == sessionBindingToken) {
            boundSession = null;
            boundSessionSourceNodeRid = null;
            boundSessionSourceSessionRid = null;
            sessionSourceSequence = 0;
            return true;
        }
        return false;
    }

    record BoundSessionSource(
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        long bindingGeneration,
        long sessionSequence) {
    }

    CompletionStage<Void> rebindNativeActor(
        ZLinkBackendActorRef targetActor,
        Duration timeout) {
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

    boolean hasBoundSession(
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid) {
        return boundSession != null
            && java.util.Objects.equals(boundSessionSourceNodeRid, sourceNodeRid)
            && java.util.Objects.equals(boundSessionSourceSessionRid, sourceSessionRid);
    }

    CompletionStage<Boolean> sendBoundSessionFrame(byte[] frameBytes) {
        if (boundSession instanceof ZLinkNativeBoundSessionRuntime runtime) {
            return runtime.sendFrame(frameBytes).thenApply(ignored -> true);
        }
        if (boundSession instanceof ZLinkRoutedBoundSessionRuntime runtime) {
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
        boundSessionSourceNodeRid = null;
        boundSessionSourceSessionRid = null;
        entrySpotNodeRid = null;
        entrySpotId = null;
        entryRouterChannelId = null;
        sessionBindingToken++;
        spotId = null;
        spot = null;
        joined = false;
        destroying = false;
        moving = false;
        moveCompletion.completeExceptionally(new ZLinkConfigurationException(
            "actor was destroyed while moving: " + actorId));
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
