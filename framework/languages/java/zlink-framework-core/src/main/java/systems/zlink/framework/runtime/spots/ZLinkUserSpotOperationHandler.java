package systems.zlink.framework.runtime.spots;

import java.security.MessageDigest;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkLocationRepository;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkUserSpotOperationException;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.locations.ZLinkServiceAuthorityPayloadCodec;
import systems.zlink.framework.runtime.mesh.MeshNodeRegistration;
import systems.zlink.framework.runtime.internal.configuration
    .ZLinkObjectFactoryRegistration.RelocatableSpotFactory;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.spots.ZLinkSpot;

final class ZLinkUserSpotOperationHandler
    implements ZLinkInternalMeshNode.UserSpotOperationHandler {
    private static final int TERMINAL_REQUEST_FAILED = 105;
    private static final int TERMINAL_INVALID_STATE = 107;
    private static final int FAILURE_REQUEST_FAILED = 17;
    private static final int FAILURE_SPOT_GENERATION_STALE = 33;
    private static final int FAILURE_SPOT_MOVING = 34;
    private static final ZLinkStoreCancellation OPEN = () -> false;
    private final String meshName;
    private final ZLinkInternalMeshNode node;
    private final ZLinkLocationRepository authorityStore;
    private final ZLinkSpotLifecycle lifecycle;
    private final ZLinkMessageSerializer serializer;
    private final Map<String, RelocatableSpotFactory<?>> factories;
    private final ZLinkServiceAuthorityPayloadCodec authorities =
        new ZLinkServiceAuthorityPayloadCodec();
    private final ZLinkServiceM6AWireCodec payloads =
        new ZLinkServiceM6AWireCodec();

    ZLinkUserSpotOperationHandler(
        String meshName,
        ZLinkInternalMeshNode node,
        ZLinkLocationRepository authorityStore,
        ZLinkSpotLifecycle lifecycle,
        ZLinkMessageSerializer serializer,
        Map<String, RelocatableSpotFactory<?>> factories) {
        this.meshName = meshName;
        this.node = node;
        this.authorityStore = authorityStore;
        this.lifecycle = lifecycle;
        this.serializer = serializer;
        this.factories = Map.copyOf(factories);
    }

    @Override
    public CompletionStage<ZLinkInternalMeshNode.UserSpotCreateResponse> create(
        ZLinkInternalMeshNode.UserSpotCreateRequest request) {
        String key = ZLinkAuthorityKeyCodec.spot(request.intent().spotId());
        return authorityStore.read(key, OPEN)
            .thenCompose(read -> {
                if (!(read instanceof ZLinkAuthoritySnapshot snapshot)) {
                    return failed(
                        TERMINAL_INVALID_STATE,
                        FAILURE_SPOT_GENERATION_STALE,
                        "User Spot reservation is missing");
                }
                Admission admission = validate(request, key, snapshot);
                byte[] stored;
                try {
                    stored = decodeInlineCreationIntent(
                        admission.pending().requestContentReference());
                } catch (RuntimeException invalid) {
                    return abort(admission.reservation()).thenCompose(
                        ignored -> failed(
                            TERMINAL_REQUEST_FAILED,
                            FAILURE_REQUEST_FAILED,
                            "User Spot create payload is invalid"));
                }
                if (stored.length != admission.pending().requestEncodedSize()
                    || !MessageDigest.isEqual(
                        sha256(stored), admission.pending().requestSha256())) {
                    return abort(admission.reservation()).thenCompose(
                        ignored -> failed(
                            TERMINAL_REQUEST_FAILED,
                            FAILURE_REQUEST_FAILED,
                            "User Spot create payload failed integrity validation"));
                }
                var application = payloads.decodeApplicationPayload(stored);
                ZLinkMessage createRequest = ZLinkMessage.fromEncoded(
                    ZLinkEncodedPayload.from(application.payload()), serializer);
                @SuppressWarnings("unchecked")
                Class<? extends ZLinkSpot<?>> spotType =
                    (Class<? extends ZLinkSpot<?>>) admission.factory().spotType();
                return lifecycle.prepareReserved(
                        spotType,
                        request.intent().spotId(),
                        snapshot.objectGeneration(),
                        createRequest)
                    .thenCompose(prepared -> finish(
                        request, snapshot, admission, prepared))
                    .exceptionallyCompose(failure ->
                        abort(admission.reservation())
                            .thenCompose(ignored -> failed(
                                    terminal(failure),
                                    failureCode(failure),
                                    message(failure))));
            });
    }

    @Override
    public CompletionStage<ZLinkInternalMeshNode.UserSpotCloseResponse> close(
        ZLinkInternalMeshNode.UserSpotCloseRequest request) {
        var fence = request.intent().target();
        String key = ZLinkAuthorityKeyCodec.spot(fence.spotId());
        return authorityStore.read(key, OPEN)
            .thenCompose(read -> {
                if (!(read instanceof ZLinkAuthoritySnapshot snapshot)) {
                    return CompletableFuture.completedFuture(
                        new ZLinkInternalMeshNode.UserSpotCloseResponse(false));
                }
                var authority = authorities.decode(snapshot.payload())
                    .orElseThrow(() -> stale("invalid User Spot authority"));
                require(
                    authority.kind() == ZLinkServiceAuthorityPayloadCodec.Kind.USER
                        && authority.state() == ZLinkServiceAuthorityPayloadCodec.State.READY
                        && authority.spotId().equals(fence.spotId())
                        && authority.meshName().equals(meshName)
                        && authority.nodeRid().equals(node.status().routingId())
                        && authority.nodeGeneration()
                            == node.status().lifecycleGeneration()
                        && snapshot.allocation().state()
                            == ZLinkPlacementAllocationState.ACTIVE
                        && snapshot.allocation().objectKind()
                            == ZLinkPlacementObjectKind.USER_SPOT
                        && snapshot.objectGeneration() == fence.objectGeneration()
                        && snapshot.authorityOwnerGeneration()
                            == fence.authorityOwnerGeneration()
                        && snapshot.storeVersion().equals(fence.storeVersion()),
                    "stale User Spot close fence");
                ZLinkSpotLifecycle.CloseReadiness readiness =
                    lifecycle.closeReadiness(
                        fence.spotId(), fence.objectGeneration());
                if (readiness == ZLinkSpotLifecycle.CloseReadiness.HAS_ACTORS) {
                    return CompletableFuture.completedFuture(
                        new ZLinkInternalMeshNode.UserSpotCloseResponse(false));
                }
                if (readiness == ZLinkSpotLifecycle.CloseReadiness.LOCAL_MISSING) {
                    return failed(
                        TERMINAL_INVALID_STATE,
                        FAILURE_SPOT_MOVING,
                        "Ready User Spot is missing local admission");
                }
                if (readiness
                    == ZLinkSpotLifecycle.CloseReadiness.GENERATION_STALE) {
                    return failed(
                        TERMINAL_INVALID_STATE,
                        FAILURE_SPOT_GENERATION_STALE,
                        "User Spot local generation is stale");
                }
                byte[] closing = authorities.encodeUser(
                    ZLinkServiceAuthorityPayloadCodec.State.CLOSING,
                    authority.stableType(),
                    fence.spotId(),
                    snapshot.ownerId(),
                    snapshot.ownerLeaseGeneration(),
                    meshName,
                    node.status().routingId(),
                    node.status().lifecycleGeneration());
                return authorityStore.compareExchange(
                        key,
                        new ZLinkAuthorityExpectFound(snapshot.storeVersion()),
                        new ZLinkAuthorityPut(
                            closing,
                            ZLinkAuthorityGenerationTransition.PRESERVE,
                            java.util.Optional.empty(),
                            java.util.Optional.empty()),
                        OPEN)
                    .thenCompose(closingWrite -> {
                        if (!(closingWrite instanceof ZLinkAuthorityStored stored)) {
                            throw stale(
                                "User Spot authority changed before Closing");
                        }
                        return lifecycle.closeReserved(
                                fence.spotId(), fence.objectGeneration())
                            .handle((closed, failure) ->
                                new CloseAttempt(
                                    failure == null
                                        && Boolean.TRUE.equals(closed),
                                    failure))
                            .thenCompose(attempt -> {
                                if (!attempt.closed()) {
                                    return rollbackClosing(
                                        key,
                                        stored.storeVersion(),
                                        snapshot,
                                        authority,
                                        attempt.failure());
                                }
                                return authorityStore.compareExchange(
                                        key,
                                        new ZLinkAuthorityExpectFound(
                                            stored.storeVersion()),
                                        new ZLinkAuthorityDelete(),
                                        OPEN)
                                    .thenCompose(deleted -> {
                                        if (!(deleted
                                            instanceof ZLinkAuthorityDeleted)) {
                                            return failed(
                                                TERMINAL_INVALID_STATE,
                                                FAILURE_SPOT_MOVING,
                                                "User Spot authority changed while closing");
                                        }
                                        return CompletableFuture.completedFuture(
                                            new ZLinkInternalMeshNode
                                                .UserSpotCloseResponse(true));
                                    });
                            });
                    });
            });
    }

    private CompletionStage<ZLinkInternalMeshNode.UserSpotCreateResponse> finish(
        ZLinkInternalMeshNode.UserSpotCreateRequest request,
        ZLinkAuthoritySnapshot snapshot,
        Admission admission,
        ZLinkSpotLifecycle.PreparedUserSpot prepared) {
        if (prepared.existing()) {
            return CompletableFuture.completedFuture(response(
                ZLinkServiceM6BWireCodec.UserSpotCreateResult.EXISTING,
                request, snapshot.objectGeneration(), null));
        }
        ZLinkMessage reply = prepared.created().response().reply();
        if (!prepared.created().response().accepted()) {
            lifecycle.discardReserved(prepared);
            return abort(admission.reservation()).thenApply(ignored -> response(
                ZLinkServiceM6BWireCodec.UserSpotCreateResult.REJECTED,
                request, snapshot.objectGeneration(), reply));
        }
        byte[] ready = authorities.encodeUser(
            ZLinkServiceAuthorityPayloadCodec.State.READY,
            request.intent().stableType(),
            request.intent().spotId(),
            snapshot.ownerId(),
            snapshot.ownerLeaseGeneration(),
            meshName,
            node.status().routingId(),
            node.status().lifecycleGeneration());
        return authorityStore.commit(
                admission.reservation(), ready, OPEN)
            .thenApply(result -> {
                if (result != ZLinkObjectCommitResult.COMMITTED
                    && result != ZLinkObjectCommitResult.ALREADY_COMMITTED) {
                    lifecycle.discardReserved(prepared);
                    throw stale("User Spot Ready commit lost its reservation");
                }
                lifecycle.publishReserved(prepared);
                node.rememberSpotAuthority(
                    new ZLinkInternalMeshNode.SpotAuthorityRoute(
                        request.intent().spotId(),
                        snapshot.objectGeneration(),
                        node.status().routingId(),
                        node.status().lifecycleGeneration(),
                        snapshot.authorityOwnerGeneration(),
                        snapshot.ownerLeaseGeneration(),
                        snapshot.ownerId(),
                        meshName,
                        snapshot.storeVersion()));
                return response(
                    ZLinkServiceM6BWireCodec.UserSpotCreateResult.CREATED,
                    request, snapshot.objectGeneration(), reply);
            });
    }

    private Admission validate(
        ZLinkInternalMeshNode.UserSpotCreateRequest request,
        String key,
        ZLinkAuthoritySnapshot snapshot) {
        var fence = request.intent().reservation();
        var allocation = snapshot.allocation();
        var pending = snapshot.pendingCreation().orElseThrow(
            () -> stale("User Spot creation projection is missing"));
        var authority = authorities.decode(snapshot.payload()).orElseThrow(
            () -> stale("invalid User Spot authority"));
        var factory = factories.get(request.intent().stableType());
        require(
            factory != null
                && authority.kind() == ZLinkServiceAuthorityPayloadCodec.Kind.USER
                && authority.state() == ZLinkServiceAuthorityPayloadCodec.State.CREATING
                && authority.spotId().equals(request.intent().spotId())
                && authority.stableType().equals(request.intent().stableType())
                && authority.meshName().equals(meshName)
                && authority.nodeRid().equals(node.status().routingId())
                && authority.nodeGeneration()
                    == node.status().lifecycleGeneration()
                && allocation.state() == ZLinkPlacementAllocationState.PENDING
                && allocation.objectKind() == ZLinkPlacementObjectKind.USER_SPOT
                && allocation.stableType().equals(request.intent().stableType())
                && allocation.descriptor().meshName().equals(meshName)
                && allocation.descriptor().rid().equals(node.status().routingId())
                && allocation.descriptorLifecycleGeneration()
                    == node.status().lifecycleGeneration()
                && pending.reservationId().equals(fence.reservationId())
                && snapshot.storeVersion().equals(fence.storeVersion())
                && snapshot.objectGeneration() == fence.objectGeneration()
                && snapshot.authorityOwnerGeneration()
                    == fence.authorityOwnerGeneration()
                && snapshot.ownerId().equals(fence.targetOwnerId())
                && snapshot.ownerLeaseGeneration()
                    == fence.targetOwnerLeaseGeneration()
                && fence.targetNodeRid().equals(node.status().routingId())
                && fence.targetNodeGeneration()
                    == node.status().lifecycleGeneration()
                && allocation.capacityBundle().equals(
                    systems.zlink.framework.runtime.internal.locations.ZLinkPlacementCapacityBundle.spot(
                            systems.zlink.framework.locations.ZLinkPlacementObjectKind.USER_SPOT,
                            request.intent().stableType(),
                            Math.toIntExact(
                                fence.pendingCapacityDelta()))),
            "stale User Spot create reservation");
        return new Admission(
            new ZLinkObjectReservation(
                key,
                snapshot.storeVersion(),
                snapshot.objectGeneration(),
                snapshot.authorityOwnerGeneration(),
                fence.reservationId(),
                allocation.descriptor(),
                allocation.descriptorLifecycleGeneration(),
                new ZLinkLocationOwnerToken(
                    snapshot.ownerId(), snapshot.ownerLeaseGeneration())),
            pending,
            factory);
    }

    private CompletionStage<ZLinkObjectAbortResult> abort(
        ZLinkObjectReservation reservation) {
        return authorityStore.abort(reservation, OPEN);
    }

    private ZLinkInternalMeshNode.UserSpotCreateResponse response(
        ZLinkServiceM6BWireCodec.UserSpotCreateResult result,
        ZLinkInternalMeshNode.UserSpotCreateRequest request,
        long generation,
        ZLinkMessage reply) {
        List<Message> parts = reply == null ? List.of() : List.of(
            Message.from(reply.toEncodedPayload(serializer).bytes()));
        return new ZLinkInternalMeshNode.UserSpotCreateResponse(
            result, request.intent().spotId(), generation, parts);
    }

    private static byte[] sha256(byte[] value) {
        try {
            return MessageDigest.getInstance("SHA-256").digest(value);
        } catch (java.security.NoSuchAlgorithmException impossible) {
            throw new AssertionError(impossible);
        }
    }

    private static byte[] decodeInlineCreationIntent(String reference) {
        String prefix = "inline-v1:";
        if (reference == null || !reference.startsWith(prefix)) {
            throw new IllegalArgumentException(
                "unsupported User Spot creation intent reference");
        }
        return java.util.Base64.getUrlDecoder().decode(
            reference.substring(prefix.length()));
    }

    private static void require(boolean condition, String message) {
        if (!condition) {
            throw stale(message);
        }
    }

    private CompletionStage<ZLinkInternalMeshNode.UserSpotCloseResponse>
        rollbackClosing(
            String key,
            String closingStoreVersion,
            ZLinkAuthoritySnapshot snapshot,
            ZLinkServiceAuthorityPayloadCodec.SpotAuthority authority,
            Throwable failure) {
        byte[] ready = authorities.encodeUser(
            ZLinkServiceAuthorityPayloadCodec.State.READY,
            authority.stableType(),
            authority.spotId(),
            snapshot.ownerId(),
            snapshot.ownerLeaseGeneration(),
            meshName,
            node.status().routingId(),
            node.status().lifecycleGeneration());
        return authorityStore.compareExchange(
                key,
                new ZLinkAuthorityExpectFound(closingStoreVersion),
                new ZLinkAuthorityPut(
                    ready,
                    ZLinkAuthorityGenerationTransition.PRESERVE,
                    java.util.Optional.empty(),
                    java.util.Optional.empty()),
                OPEN)
            .thenCompose(ignored -> failed(
                TERMINAL_INVALID_STATE,
                FAILURE_SPOT_MOVING,
                failure == null
                    ? "User Spot local admission changed after Closing"
                    : message(failure)));
    }

    private static ZLinkUserSpotOperationException stale(String message) {
        return new ZLinkUserSpotOperationException(
            TERMINAL_INVALID_STATE,
            FAILURE_SPOT_GENERATION_STALE,
            message);
    }

    private static <T> CompletionStage<T> failed(
        int terminalResult,
        int failureCode,
        String message) {
        return CompletableFuture.failedFuture(
            new ZLinkUserSpotOperationException(
                terminalResult, failureCode, message));
    }

    private static int terminal(Throwable failure) {
        Throwable current = unwrap(failure);
        return current instanceof ZLinkUserSpotOperationException typed
            ? typed.terminalResult()
            : TERMINAL_REQUEST_FAILED;
    }

    private static int failureCode(Throwable failure) {
        Throwable current = unwrap(failure);
        return current instanceof ZLinkUserSpotOperationException typed
            ? typed.failureCode()
            : FAILURE_REQUEST_FAILED;
    }

    private static String message(Throwable failure) {
        Throwable current = unwrap(failure);
        return current.getMessage() == null
            ? "User Spot operation failed"
            : current.getMessage();
    }

    private static Throwable unwrap(Throwable failure) {
        Throwable current = failure;
        while ((current instanceof java.util.concurrent.CompletionException
            || current instanceof java.util.concurrent.ExecutionException)
            && current.getCause() != null) {
            current = current.getCause();
        }
        return current;
    }

    private record Admission(
        ZLinkObjectReservation reservation,
        ZLinkPendingObjectCreation pending,
        RelocatableSpotFactory<?> factory) {
    }

    private record CloseAttempt(boolean closed, Throwable failure) {
    }
}
