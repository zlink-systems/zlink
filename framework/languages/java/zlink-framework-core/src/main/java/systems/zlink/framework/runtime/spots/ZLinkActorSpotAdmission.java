package systems.zlink.framework.runtime.spots;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.actors.ZLinkActorRuntime;
import systems.zlink.framework.runtime.actors.ZLinkActorSpotRoutePackets;
import systems.zlink.framework.runtime.actors.ZLinkSessionRelocationPeerClient;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.relocation.ZLinkActorJoinRelocationPort;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;

import java.time.Duration;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.BooleanSupplier;
import java.util.function.Function;
import java.util.function.Supplier;

final class ZLinkActorSpotAdmission {
    /** Owns the target-side ordering required before Ready is observable. */
    static <T> CompletionStage<T> completeTargetBeforeReady(
            Supplier<CompletionStage<Void>> lifecycle,
            Supplier<CompletionStage<Void>> joinCompletion,
            Supplier<CompletionStage<T>> replay,
            Runnable dispatchSwitch,
            Supplier<CompletionStage<Void>> publishReady) {
        return lifecycle
                .get()
                .thenCompose(ignored -> joinCompletion.get())
                .thenCompose(ignored -> replay.get())
                .thenApply(
                        result -> {
                            dispatchSwitch.run();
                            return result;
                        })
                .thenCompose(result -> publishReady.get().thenApply(ignored -> result));
    }

    private ZLinkActorRuntime actors;
    private ZLinkSessionRelocationPeerClient sessionRoutes;
    private BooleanSupplier draining = () -> false;

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

    CompletionStage<Void> markLeft(ZLinkActor actor) {
        return requireActors().markLeft(actor);
    }

    CompletionStage<Void> leaveRoutedActorToLocalEntry(
            ZLinkActor actor,
            RoutingId entryNodeRid,
            String entrySpotId,
            long entrySpotGeneration,
            Function<String, CompletionStage<ZLinkSpotActorJoinResult>> admissionCallback,
            Function<ZLinkActor, CompletionStage<Void>> joinedCallback) {
        ZLinkActorRuntime runtime = requireActors();
        return invokeAdmissionCallback(admissionCallback, actor.context().actorId())
                .thenCompose(
                        response ->
                                effectiveResponse(response).accepted()
                                        ? runtime.leaveSourceForLocalMove(actor)
                                        : CompletableFuture.failedFuture(
                                                new ZLinkConfigurationException(
                                                        "actor Entry Spot join was rejected: "
                                                                + actor.context().actorId())))
                .thenCompose(
                        ignored ->
                                runtime.commitEntryLocation(
                                        actor, entryNodeRid, entrySpotId, entrySpotGeneration))
                .thenRun(() -> runtime.completeRemoteMove(actor))
                .thenCompose(
                        ignored ->
                                runtime.invokeActorLifecycle(
                                        actor, () -> joinedCallback.apply(actor)));
    }

    CompletionStage<Void> markJoined(
            ZLinkActor actor, ZLinkBackendActorRef actorRef, String spotId, ZLinkSpot<?> spot) {
        return requireActors().markJoined(actor, actorRef, spotId, spot);
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
            return CompletableFuture.failedFuture(
                    new ZLinkConfigurationException(
                            "canonical actor Join request has the wrong phase"));
        }
        if (draining.getAsBoolean()) {
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.reject());
        }
        return requireActors()
                .readActorJoinAuthority(request.actorId())
                .thenCompose(
                        authority -> {
                            validateAuthorityFence(request, authority);
                            ZLinkActorSpotRoutePackets.TransferRequest storeResolved =
                                    request.withActorType(authority.stableType());
                            try {
                                requireActors().resolveActorFactoryType(storeResolved.actorType());
                            } catch (ZLinkConfigurationException noFactory) {
                                return CompletableFuture.completedFuture(
                                        ZLinkSpotActorJoinResult.reject());
                            }
                            return invokeAdmissionCallback(callback, request.actorId())
                                    .thenApply(ZLinkActorSpotAdmission::effectiveResponse)
                                    .thenApply(
                                            response -> {
                                                if (response.accepted() && canonicalJoin != null) {
                                                    var actor = canonicalJoin.actor();
                                                    var target = canonicalJoin.targetSpot();
                                                    requireActors()
                                                            .admitCanonicalActorJoin(
                                                                    new ZLinkActorJoinRelocationPort
                                                                            .CanonicalAdmission(
                                                                            UUID.fromString(
                                                                                    request
                                                                                            .transferId()),
                                                                            request.actorId(),
                                                                            storeResolved
                                                                                    .actorType(),
                                                                            request
                                                                                    .actorGeneration(),
                                                                            RoutingId.from(
                                                                                    actor
                                                                                            .targetNodeRid()),
                                                                            actor
                                                                                    .targetNodeGeneration(),
                                                                            actor
                                                                                    .expectedAuthorityOwnerGeneration(),
                                                                            actor
                                                                                    .expectedOwnerLeaseGeneration(),
                                                                            targetSpotId,
                                                                            target.generation(),
                                                                            RoutingId.from(
                                                                                    target
                                                                                            .targetNodeRid()),
                                                                            target
                                                                                    .targetNodeGeneration(),
                                                                            target
                                                                                    .expectedAuthorityOwnerGeneration(),
                                                                            target
                                                                                    .expectedOwnerLeaseGeneration(),
                                                                            targetSpot,
                                                                            joinedCallback,
                                                                            response.reply() == null
                                                                                    ? ZLinkMessage
                                                                                            .empty()
                                                                                    : response
                                                                                            .reply(),
                                                                            requestContentType,
                                                                            rawRequest,
                                                                            Duration.ofMillis(
                                                                                    Math.max(
                                                                                            1L,
                                                                                            request
                                                                                                    .timeoutMillis()))));
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
                || request.authorityOwnerGeneration() != authority.authorityOwnerGeneration()
                || request.ownerLeaseGeneration() != authority.ownerLeaseGeneration()) {
            throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                    "Actor Join Authority row does not exactly match its route fence: "
                            + request.actorId());
        }
    }

    private static CompletionStage<ZLinkSpotActorJoinResult> invokeAdmissionCallback(
            Function<String, CompletionStage<ZLinkSpotActorJoinResult>> callback, String actorId) {
        try {
            return callback.apply(actorId);
        } catch (RuntimeException error) {
            return CompletableFuture.failedFuture(error);
        }
    }

    private static ZLinkSpotActorJoinResult effectiveResponse(ZLinkSpotActorJoinResult response) {
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
