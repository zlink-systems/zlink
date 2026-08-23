package systems.zlink.framework.runtime.spots;
import java.util.UUID;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentMap;
import java.util.function.BooleanSupplier;
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue;
import systems.zlink.framework.spots.ZLinkEntrySpot;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Function;
import java.util.function.Supplier;
import java.util.List;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.binding.spot.ActorRef;
import systems.zlink.framework.runtime.internal.binding.spot.ActorTransferId;
import systems.zlink.framework.runtime.internal.binding.spot.ActorTransferPrepare;
import systems.zlink.framework.runtime.internal.binding.spot.ActorTransferRole;
import systems.zlink.framework.runtime.internal.binding.spot.PrepareActorTransferResult;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.actors.ZLinkActorRuntime;
import systems.zlink.framework.runtime.actors.ZLinkActorSpotRoutePackets;
import systems.zlink.framework.runtime.actors.ZLinkSessionRelocationPeerClient;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorJoinRequest;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;
import systems.zlink.framework.runtime.internal.relocation
    .ZLinkActorJoinAdmissionProfileCodec;
import systems.zlink.framework.runtime.internal.relocation
    .ZLinkActorJoinRelocationPort;

final class ZLinkActorSpotAdmission {
    /** Owns the target-side ordering required before Ready is observable. */
    static <T> CompletionStage<T> completeTargetBeforeReady(
        Supplier<CompletionStage<Void>> lifecycle,
        Supplier<CompletionStage<Void>> joinCompletion,
        Supplier<CompletionStage<T>> replay,
        Runnable dispatchSwitch,
        Supplier<CompletionStage<Void>> publishReady) {
        return lifecycle.get()
            .thenCompose(ignored -> joinCompletion.get())
            .thenCompose(ignored -> replay.get())
            .thenApply(result -> {
                dispatchSwitch.run();
                return result;
            })
            .thenCompose(result -> publishReady.get()
                .thenApply(ignored -> result));
    }

    private ZLinkActorRuntime actors;
    private ZLinkSessionRelocationPeerClient sessionRoutes;
    private BooleanSupplier draining = () -> false;
    private final ConcurrentMap<String, CompletableFuture<Void>>
        pendingEntryJoins = new ConcurrentHashMap<>();
    private final ConcurrentMap<String, CompletableFuture<Void>>
        pendingLeaves = new ConcurrentHashMap<>();
    private final ConcurrentMap<String, LocalJoin> pendingLocalJoins =
        new ConcurrentHashMap<>();

    private record LocalJoin(
        ZLinkBackendActorRef actorRef,
        String spotId,
        ZLinkSpot<?> spot,
        Function<ZLinkActor, CompletionStage<Void>> joinedCallback) {
    }

    void attach(
        ZLinkActorRuntime actors,
        BooleanSupplier draining,
        ZLinkSessionRelocationPeerClient sessionRoutes) {
        this.actors = actors;
        this.draining = draining == null ? () -> false : draining;
        this.sessionRoutes = sessionRoutes;
    }

    void traceTransferMarker(String marker, String actorId, long arrivalIndex) {
        requireActors().traceActorTransferMarker(marker, actorId, Long.toString(arrivalIndex));
    }

    boolean isActorAtSpot(String actorId, String spotId) {
        return requireActors().isActorAtSpot(actorId, spotId);
    }

    Object deferredJoinRuntimeScope() {
        ZLinkActorRuntime runtime = actors;
        return runtime == null ? this : runtime;
    }

    CompletionStage<Void> destroyFromEntry(RoutingId nodeRid, ZLinkActor actor) {
        return requireActors().destroyFromEntrySpot(nodeRid, actor);
    }

    CompletionStage<Void> leaveSpot(
        ZLinkInternalSpotNode node,
        ZLinkActor actor,
        String fallbackSpotId,
        RoutingId entryNodeRid,
        Duration timeout) {
        ZLinkActorRuntime runtime = requireActors();
        String currentSpotId = runtime.spotId(actor).orElse(fallbackSpotId);
        ZLinkBackendActorRef actorRef = runtime.actorRef(actor);
        CompletableFuture<Void> entryJoined = entryNodeRid == null
            ? null
            : new CompletableFuture<>();
        CompletableFuture<Void> actorLeft = new CompletableFuture<>();
        if (entryJoined != null) {
            CompletableFuture<Void> previous = pendingEntryJoins.putIfAbsent(
                actor.context().actorId(), entryJoined);
            if (previous != null) {
                return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                    "actor Entry Spot join is already pending: " + actor.context().actorId()));
            }
        }
        CompletableFuture<Void> previousLeave = pendingLeaves.putIfAbsent(
            actor.context().actorId(), actorLeft);
        if (previousLeave != null) {
            if (entryJoined != null) {
                pendingEntryJoins.remove(actor.context().actorId(), entryJoined);
            }
            return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                "actor leave is already pending: " + actor.context().actorId()));
        }
        CompletionStage<Void> leaving = node.leaveActor(actorRef, currentSpotId, timeout)
            .whenComplete((replyParts, error) -> {
                if (replyParts != null) {
                    replyParts.forEach(Message::close);
                }
                if (error != null && entryJoined != null
                    && pendingEntryJoins.remove(actor.context().actorId(), entryJoined)) {
                    entryJoined.completeExceptionally(error);
                }
                if (error != null && pendingLeaves.remove(actor.context().actorId(), actorLeft)) {
                    actorLeft.completeExceptionally(error);
                }
            })
            .thenCompose(ignored -> actorLeft)
            .thenCompose(ignored -> entryNodeRid == null
                ? CompletableFuture.completedFuture(null)
                : joinEntrySpotAfterLeave(runtime, actor, entryNodeRid, timeout)
                    .thenCompose(joined -> entryJoined));
        return ZLinkAsyncSerialQueue.yieldCurrent(leaving);
    }

    private static CompletionStage<Void> joinEntrySpotAfterLeave(
        ZLinkActorRuntime runtime,
        ZLinkActor actor,
        RoutingId entryNodeRid,
        Duration timeout) {
        try (systems.zlink.framework.runtime.internal.handlers
                 .ZLinkSuspendInvocationContext.Scope ignored =
                 systems.zlink.framework.runtime.internal.handlers
                     .ZLinkSuspendInvocationContext.enterApplicationExecution(null)) {
            return runtime.joinEntrySpot(actor, entryNodeRid, timeout);
        }
    }

    CompletionStage<Void> markLeft(ZLinkActor actor) {
        return requireActors().markLeft(actor);
    }

    CompletionStage<Void> leaveRoutedActorToLocalEntry(
        ZLinkActor actor,
        RoutingId entryNodeRid,
        Function<String, CompletionStage<ZLinkSpotActorJoinResult>> admissionCallback,
        Function<ZLinkActor, CompletionStage<Void>> joinedCallback) {
        ZLinkActorRuntime runtime = requireActors();
        return invokeAdmissionCallback(admissionCallback, actor.context().actorId())
            .thenCompose(response -> effectiveResponse(response).accepted()
                ? runtime.leaveSourceForLocalMove(actor)
                : CompletableFuture.failedFuture(new ZLinkConfigurationException(
                    "actor Entry Spot join was rejected: " + actor.context().actorId())))
            .thenCompose(ignored -> runtime.commitEntryLocation(actor, entryNodeRid))
            .thenRun(() -> runtime.completeRemoteMove(actor))
            .thenCompose(ignored -> runtime.invokeActorLifecycle(
                actor,
                () -> joinedCallback.apply(actor)));
    }

    CompletionStage<Void> markJoined(
        ZLinkActor actor,
        ZLinkBackendActorRef actorRef,
        String spotId,
        ZLinkSpot<?> spot) {
        return requireActors().markJoined(actor, actorRef, spotId, spot);
    }

    CompletionStage<ZLinkSpotActorJoinResult> admitEntryActor(
        ZLinkBackendActorJoinRequest request,
        String spotId,
        Function<String, CompletionStage<ZLinkSpotActorJoinResult>> callback) {
        if (draining.getAsBoolean()) {
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.reject());
        }
        String actorId = request.targetActor().actorId();
        return invokeAdmissionCallback(callback, actorId)
            .thenApply(ZLinkActorSpotAdmission::effectiveResponse)
            .whenComplete((response, error) -> {
                if (error != null || response == null || !response.accepted()) {
                    completeEntryJoin(actorId, error == null
                        ? new ZLinkConfigurationException(
                            "actor Entry Spot join was rejected: " + actorId)
                        : error);
                }
            });
    }

    CompletionStage<Void> completeEntryActorJoin(
        ZLinkBackendActorJoinRequest request,
        RoutingId entryNodeRid,
        Function<ZLinkActor, CompletionStage<Void>> joinedCallback) {
        String actorId = request.targetActor().actorId();
        ZLinkActorRuntime runtime = requireActors();
        return runtime.getOrCreateLocalActor(actorId, ZLinkActor.class)
            .thenCompose(actor -> actor
                .map(value -> {
                    // Entry Spot membership is framework infrastructure state;
                    // it must not publish a durable user-Spot join.
                    runtime.markJoinedEntrySpot(
                        value, request.targetActor(), entryNodeRid);
                    return joinedCallback.apply(value);
                })
                .orElseGet(() -> CompletableFuture.failedFuture(
                    new ZLinkConfigurationException(
                        "Entry Spot actor is not available: " + actorId))))
            .whenComplete((ignored, error) -> completeEntryJoin(actorId, error));
    }

    void completeEntryJoin(String actorId, Throwable error) {
        CompletableFuture<Void> pending = pendingEntryJoins.remove(actorId);
        if (pending == null) {
            return;
        }
        if (error == null) {
            pending.complete(null);
        } else {
            pending.completeExceptionally(error);
        }
    }

    void completeLeave(String actorId, Throwable error) {
        CompletableFuture<Void> pending = pendingLeaves.remove(actorId);
        if (pending == null) {
            return;
        }
        if (error == null) {
            pending.complete(null);
        } else {
            pending.completeExceptionally(error);
        }
    }

    boolean isLeavePending(String actorId) {
        return pendingLeaves.containsKey(actorId);
    }

    CompletionStage<ZLinkSpotActorJoinResult> admitSpotActor(
        ZLinkBackendActorJoinRequest request,
        String spotId,
        ZLinkSpot<?> spotSurface,
        Function<String, CompletionStage<ZLinkSpotActorJoinResult>> callback,
        Function<ZLinkActor, CompletionStage<Void>> joinedCallback) {
        return admitNativeActor(request, spotId, spotSurface, callback, joinedCallback);
    }

    private CompletionStage<ZLinkSpotActorJoinResult> admitNativeActor(
        ZLinkBackendActorJoinRequest request,
        String spotId,
        ZLinkSpot<?> spotSurface,
        Function<String, CompletionStage<ZLinkSpotActorJoinResult>> callback,
        Function<ZLinkActor, CompletionStage<Void>> joinedCallback) {
        if (draining.getAsBoolean()) {
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.reject());
        }
        String actorId = request.targetActor().actorId();
        return invokeAdmissionCallback(callback, actorId)
            .thenCompose(response -> {
                ZLinkSpotActorJoinResult effective = effectiveResponse(response);
                if (!effective.accepted()) {
                    return CompletableFuture.completedFuture(effective);
                }
                LocalJoin pending = new LocalJoin(
                    request.targetActor(), spotId, spotSurface, joinedCallback);
                LocalJoin previous = pendingLocalJoins.putIfAbsent(actorId, pending);
                if (previous != null) {
                    return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                        "local actor Spot join is already pending: " + actorId));
                }
                return CompletableFuture.completedFuture(effective);
            });
    }

    CompletionStage<Void> completeLocalJoinFromCaller(ZLinkActor actor) {
        LocalJoin pending = pendingLocalJoins.remove(actor.context().actorId());
        if (pending == null) {
            return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                "local actor Spot admission is missing: " + actor.context().actorId()));
        }
        ZLinkActorRuntime runtime = requireActors();
        ZLinkActorRuntime.LocalMoveSource source = runtime.beginLocalMove(actor);
        return runtime.commitJoinedLocation(actor, pending.actorRef(), pending.spotId())
            .thenCompose(ignored -> runtime.markJoined(
                actor, pending.actorRef(), pending.spotId(), pending.spot()))
            .thenCompose(ignored -> pending.joinedCallback().apply(actor))
            .thenRun(() -> {
                runtime.notifySourceForLocalMove(actor, source);
                runtime.completeRemoteMove(actor);
            })
            .whenComplete((ignored, error) -> {
                if (error != null) {
                    runtime.failRemoteMove(actor, error);
                }
            });
    }

    void cancelLocalJoin(ZLinkActor actor) {
        if (actor != null) {
            pendingLocalJoins.remove(actor.context().actorId());
        }
    }

    CompletionStage<ZLinkSpotActorJoinResult> prepareCanonicalRoutedActor(
        ZLinkActorSpotRoutePackets.TransferRequest request,
        String routeChannelName,
        RoutingId sourcePeerRid,
        String targetSpotId,
        Object targetSpot,
        systems.zlink.framework.runtime.protocol.ServiceWirePilotCodec.ActorJoin28
            canonicalJoin,
        String requestContentType,
        byte[] rawRequest,
        Function<ZLinkActor, CompletionStage<Void>> joinedCallback,
        Function<String, CompletionStage<ZLinkSpotActorJoinResult>> callback) {
        if (!request.admission()) {
            return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                "canonical actor Join request has the wrong phase"));
        }
        if (draining.getAsBoolean()) {
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.reject());
        }
        return requireActors().readActorJoinAuthority(request.actorId())
            .thenCompose(authority -> {
                validateAuthorityFence(request, authority);
                ZLinkActorSpotRoutePackets.TransferRequest storeResolved =
                    request.withActorType(authority.stableType());
                try {
                    requireActors().resolveActorFactoryType(
                        storeResolved.actorType());
                } catch (ZLinkConfigurationException noFactory) {
                    return CompletableFuture.completedFuture(
                        ZLinkSpotActorJoinResult.reject());
                }
                return invokeAdmissionCallback(callback, request.actorId())
                    .thenApply(ZLinkActorSpotAdmission::effectiveResponse)
                    .thenApply(response -> {
                        if (response.accepted() && canonicalJoin != null) {
                            var actor = canonicalJoin.actor();
                            var target = canonicalJoin.targetSpot();
                            requireActors().admitCanonicalActorJoin(
                                new ZLinkActorJoinRelocationPort
                                    .CanonicalAdmission(
                                    UUID.fromString(request.transferId()),
                                    request.actorId(),
                                    storeResolved.actorType(),
                                    request.actorGeneration(),
                                    RoutingId.from(actor.targetNodeRid()),
                                    actor.targetNodeGeneration(),
                                    actor.expectedAuthorityOwnerGeneration(),
                                    actor.expectedOwnerLeaseGeneration(),
                                    targetSpotId,
                                    target.generation(),
                                    RoutingId.from(target.targetNodeRid()),
                                    target.targetNodeGeneration(),
                                    target.expectedAuthorityOwnerGeneration(),
                                    target.expectedOwnerLeaseGeneration(),
                                    targetSpot,
                                    joinedCallback,
                                    response.reply() == null
                                        ? ZLinkMessage.empty()
                                        : response.reply(),
                                    requestContentType,
                                    rawRequest,
                                    Duration.ofMillis(Math.max(
                                        1L, request.timeoutMillis()))));
                        }
                        return response;
                    });
            });
    }

    private static void validateAuthorityFence(
        ZLinkActorSpotRoutePackets.TransferRequest request,
        ZLinkActorRuntime.ActorJoinAuthority authority) {
        // Object/owner generations are bounded counters. The node lifecycle
        // generation is an opaque full-range equality token, where only zero
        // means absent (spec 01 glossary; spec 51 §9).
        if (request.actorGeneration() <= 0
            || request.authorityOwnerGeneration() <= 0
            || request.ownerLeaseGeneration() <= 0
            || request.actorNodeGeneration() == 0
            || request.actorGeneration() != authority.objectGeneration()
            || !request.actorNodeRid().equals(authority.ownerNodeRid())
            || request.actorNodeGeneration() != authority.ownerNodeGeneration()
            || request.authorityOwnerGeneration()
                != authority.authorityOwnerGeneration()
            || request.ownerLeaseGeneration() != authority.ownerLeaseGeneration()) {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                "Actor Join Authority row does not exactly match its route fence: "
                    + request.actorId());
        }
    }

    private static CompletionStage<ZLinkSpotActorJoinResult> invokeAdmissionCallback(
        Function<String, CompletionStage<ZLinkSpotActorJoinResult>> callback,
        String actorId) {
        try {
            return callback.apply(actorId);
        } catch (RuntimeException error) {
            return CompletableFuture.failedFuture(error);
        }
    }

    private static ZLinkSpotActorJoinResult effectiveResponse(
        ZLinkSpotActorJoinResult response) {
        return response == null ? ZLinkSpotActorJoinResult.reject() : response;
    }

    private ZLinkActorRuntime requireActors() {
        if (actors == null) {
            throw new ZLinkConfigurationException(
                "actor runtime is required for Spot actor admission");
        }
        return actors;
    }

    ZLinkActorRuntime runtime() {
        return requireActors();
    }
}
