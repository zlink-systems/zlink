package systems.zlink.framework.runtime.internal.backend;

import java.util.List;
import java.util.Optional;
import java.time.Duration;
import java.util.function.Consumer;
import java.util.function.Function;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerEntry;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeStatus;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeMonitor;
import systems.zlink.framework.runtime.internal.binding.spot.PeerChannels;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendObject;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceMessageFollowWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceRelocationWireCodec;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkInboundDispatchBudget;
import systems.zlink.framework.streams.ZLinkStreamCodec;

public interface ZLinkInternalMeshNode extends ZLinkBackendObject {
    void setBind(String endpoint);

    void addChannel(String channelName);

    void setChannelWeight(String channelName, int weight);

    default int placementWeight() {
        return 100;
    }

    default void setPlacementWeight(int weight) {
        // Optional for test and alternate backends that do not publish
        // Framework-owned service descriptors.
    }

    default void setObjectRole(
        systems.zlink.framework.locations.ZLinkMeshNodeObjectRole role) {
        // Optional for alternate backends that do not publish Framework
        // service admission descriptors.
    }

    default long maxMessageSize() {
        return 0;
    }

    default void setMaxMessageSize(long value) {
        throw new UnsupportedOperationException("MeshNode max message size is not available");
    }

    default void setRouterHighWaterMark(long value) {
        // Optional for test and alternate backends that do not expose Core
        // RouteMesh admission yet.
    }

    default void setRouterSendTimeout(Duration value) {
        // Optional for test and alternate backends that do not expose Core
        // RouteMesh admission yet.
    }

    default void setRouterPendingAdmissionCapacity(int value) {
        // Optional for test and alternate backends that do not expose bounded
        // asynchronous RouteMesh admission yet.
    }

    default void setMailboxMessageBudget(long value) {
        // Optional for test and alternate backends that do not expose Core
        // mailbox admission yet.
    }

    void setRoutingId(RoutingId routingId);

    void start();

    long connectPeer(String endpoint);

    long connectPeer(String endpoint, RoutingId expectedRoutingId);

    default long connectPeer(
        String endpoint,
        RoutingId expectedRoutingId,
        long expectedLifecycleGeneration,
        String expectedSecurityIdentity) {
        return connectPeer(endpoint, expectedRoutingId);
    }

    default void observePeerAdmissionExpectation(
        RoutingId peerRid,
        String endpoint,
        long lifecycleGeneration,
        String securityIdentity) {
        // Optional for alternate backends without descriptor admission.
    }

    default void forgetPeerAdmissionExpectation(RoutingId peerRid) {
        // Optional for alternate backends without descriptor admission.
    }

    default void removePeerConnection(long connectionIntentId) {
        // Optional for alternate and test backends that do not retain connection intents.
    }

    default void markPeerConnectionNotRequired(
        RoutingId peerRid,
        String endpoint,
        long lifecycleGeneration) {
        // Optional for alternate backends without descriptor-backed peer monitoring.
    }

    default void clearPeerConnectionNotRequired(RoutingId peerRid) {
        // Optional for alternate backends without descriptor-backed peer monitoring.
    }

    MeshNodeStatus status();

    List<MeshPeerEntry> peers();

    default PeerChannels peerChannels(RoutingId peerRid, long lifecycleGeneration) {
        return new PeerChannels(List.of(), List.of());
    }

    default java.util.Map<String, Integer> channelWeights() {
        return java.util.Map.of();
    }

    default MeshNodeMonitor openMonitor() {
        throw new UnsupportedOperationException("MeshNode monitor is not available");
    }

    List<Long> connectionIntentIds();

    void startDispatch(Consumer<ZLinkMeshDispatchRecord> receiver);

    default void setApplicationReceiver(ZLinkMeshApplicationReceiver receiver) {
        // Alternate backends may not support process-local Node direct dispatch.
    }

    default void setApplicationDispatchBudget(
        ZLinkInboundDispatchBudget budget) {
        // Alternate backends may not retain Framework application admission.
    }

    /**
     * Installs the immutable content-type to stream-codec mapping used by
     * typed Actor ingress. Unknown incoming content types must return an
     * empty result so the service boundary can report a protocol failure
     * without attempting a JSON fallback.
     */
    default void setApplicationStreamCodecResolver(
        Function<String, Optional<ZLinkStreamCodec>> resolver) {
        // Alternate backends may decode the application envelope elsewhere.
    }

    default ZLinkInternalSpotNode spotNode() {
        throw new UnsupportedOperationException("MeshNode Spot backend is not available");
    }

    default Optional<RoutingId> selectPlacementTarget() {
        return Optional.empty();
    }

    default void setUserSpotOperationHandler(
        UserSpotOperationHandler handler) {
        // Alternate backends may not yet own Framework service operations.
    }

    default void setActorCreateOperationHandler(
        ActorCreateOperationHandler handler) {
        // Alternate backends may not yet own Framework Actor creation.
    }

    /**
     * Installs the durable source-owner resolver used before a remote Spot or
     * Actor operation enters an application queue.
     */
    default void setPeerAuthorityResolver(
        PeerAuthorityResolver resolver) {
        // Alternate backends may not accept stateful service operations.
    }

    default CompletionStage<Void> refreshLocalAuthorityFence() {
        return java.util.concurrent.CompletableFuture.completedFuture(null);
    }

    /**
     * Installs the infrastructure-only relocation command endpoint. Commands
     * use the admitted RouteMesh peer and never enter an application mailbox.
     */
    default void setRelocationControlHandler(
        RelocationControlHandler handler) {
        // Alternate backends may not yet support remote relocation control.
    }

    /**
     * Sends one relocation command to the exact target node. The transport
     * performs one submission only; a stale or failed route is not resolved
     * and retried inside the same operation.
     */
    default CompletionStage<byte[]> requestRelocationControl(
        RoutingId targetNodeRid,
        byte[] command,
        Duration timeout) {
        return java.util.concurrent.CompletableFuture.failedFuture(
            new UnsupportedOperationException(
                "Remote relocation control is unavailable"));
    }

    /**
     * Installs the canonical service-wire command 30-35/40-41 endpoint.
     * These infrastructure records are one-way state-machine transitions and
     * do not use the legacy relocation request/reply wrapper.
     */
    default void setCanonicalRelocationControlHandler(
        CanonicalRelocationControlHandler handler) {
        // Alternate backends may not yet support canonical relocation control.
    }

    /**
     * Accepts one canonical relocation control record for infrastructure
     * delivery to the exact admitted peer.
     */
    default CompletionStage<Void> sendCanonicalRelocationControl(
        RoutingId targetNodeRid,
        byte[] command) {
        return java.util.concurrent.CompletableFuture.failedFuture(
            new UnsupportedOperationException(
                "Canonical relocation control is unavailable"));
    }

    /**
     * Installs the infrastructure-only Message Follow notification endpoint.
     * The notice carries only route fences and queue accounting; it never
     * enters a user Spot or Actor mailbox.
     */
    default void setMessageFollowHandler(MessageFollowHandler handler) {
        // Alternate backends may not yet support the command 50 route fence.
    }

    /**
     * Sends one exact command 50 notice to the admitted target node. Route
     * resolution and retry are owned by the caller; this method submits the
     * notice to the supplied node only.
     */
    default CompletionStage<Void> sendMessageFollow(
        RoutingId targetNodeRid,
        ZLinkServiceMessageFollowWireCodec.Notice notice) {
        return java.util.concurrent.CompletableFuture.failedFuture(
            new UnsupportedOperationException(
                "Message Follow notification is unavailable"));
    }

    default void setRelocationReplyRelayHandler(
        RelocationReplyRelayHandler handler) {
        // Alternate backends may not support command 33/46 dispatch.
    }

    /**
     * Sends one canonical command 33 record plus its application payload and
     * returns the exact command 46 closed acknowledgement.
     */
    default CompletionStage<byte[]> requestRelocationReplyRelay(
        RoutingId sourceNodeRid,
        ZLinkServiceRelocationWireCodec.RequestSourceFence expectedSource,
        byte[] command33,
        List<byte[]> payload,
        Duration timeout) {
        return java.util.concurrent.CompletableFuture.failedFuture(
            new UnsupportedOperationException(
                "Relocation reply relay is unavailable"));
    }

    default void setSessionRelocationRouteHandler(
        SessionRelocationRouteHandler handler) {
        // Alternate backends may not support command 44/45 dispatch.
    }

    /**
     * Submits one exact command 44 record and returns its command 45 ACK.
     * The same operation is never re-resolved or submitted to another owner.
     */
    default CompletionStage<byte[]> requestSessionRelocationRoute(
        RoutingId sessionOwnerNodeRid,
        byte[] command44,
        Duration timeout) {
        return java.util.concurrent.CompletableFuture.failedFuture(
            new UnsupportedOperationException(
                "Session relocation routing is unavailable"));
    }


    default CompletionStage<ActorCreateResponse> requestActorCreate(
        RoutingId targetNodeRid,
        ActorCreateIntent intent,
        Duration timeout) {
        return java.util.concurrent.CompletableFuture.failedFuture(
            new UnsupportedOperationException(
                "Remote Actor create is unavailable"));
    }

    default CompletionStage<UserSpotCreateResponse>
        requestUserSpotCreate(
            RoutingId targetNodeRid,
            UserSpotCreateIntent intent,
            Duration timeout) {
        return java.util.concurrent.CompletableFuture.failedFuture(
            new UnsupportedOperationException(
                "Remote User Spot create is unavailable"));
    }

    default CompletionStage<UserSpotCloseResponse>
        requestUserSpotClose(
            RoutingId targetNodeRid,
            UserSpotCloseIntent intent,
            Duration timeout) {
        return java.util.concurrent.CompletableFuture.failedFuture(
            new UnsupportedOperationException(
                "Remote User Spot close is unavailable"));
    }

    default void rememberSpotAuthority(
        SpotAuthorityRoute route) {
        // Alternate backends may resolve the durable route on each call.
    }

    default long localAuthorityLeaseGeneration() {
        return 0L;
    }

    default void forgetSpotAuthority(
        SpotAuthorityRoute route) {
        // Alternate backends may resolve the durable route on each call.
    }

    default void registerInstanceIntent(
        String stableType,
        ZLinkServiceM6BWireCodec.InstanceRouteFence route) {
        // Alternate backends may materialize Instance Spot elsewhere.
    }

    default void registerInstanceSpotType(String stableType) {
        // Alternate backends may materialize Instance Spot elsewhere.
    }

    default void registerInstanceSpotType(
        String stableType,
        InstanceSpotActivationHandler handler) {
        registerInstanceSpotType(stableType);
    }

    default void forgetInstanceIntent(
        ZLinkServiceM6BWireCodec.InstanceRouteFence route) {
        // Alternate backends may materialize Instance Spot elsewhere.
    }

    @FunctionalInterface
    interface InstanceSpotActivationHandler {
        CompletionStage<Void> activate(
            String stableType,
            ZLinkServiceM6BWireCodec.InstanceRouteFence route,
            ZLinkBackendSpot backendSpot);
    }

    default CompletionStage<Void> submitInstanceSpotSend(
        ZLinkServiceM6BWireCodec.InstanceRouteFence route,
        String stableType,
        String sourceSpotId,
        byte[] metadata,
        List<systems.zlink.contracts.messaging.Message> parts) {
        return java.util.concurrent.CompletableFuture.failedFuture(
            new UnsupportedOperationException(
                "Remote Instance Spot send is unavailable"));
    }

    default CompletionStage<Void> submitInstanceSpotSend(
        ZLinkServiceM6BWireCodec.InstanceRouteFence route,
        String stableType,
        String sourceSpotId,
        byte[] metadata,
        List<systems.zlink.contracts.messaging.Message> parts,
        Duration timeout) {
        return submitInstanceSpotSend(
            route, stableType, sourceSpotId, metadata, parts);
    }

    default CompletionStage<List<systems.zlink.contracts.messaging.Message>>
        requestInstanceSpot(
        ZLinkServiceM6BWireCodec.InstanceRouteFence route,
        String stableType,
        String sourceSpotId,
            byte[] metadata,
            List<systems.zlink.contracts.messaging.Message> parts,
            Duration timeout) {
        return java.util.concurrent.CompletableFuture.failedFuture(
            new UnsupportedOperationException(
                "Remote Instance Spot request is unavailable"));
    }

    interface UserSpotOperationHandler {
        CompletionStage<UserSpotCreateResponse> create(
            UserSpotCreateRequest request);

        CompletionStage<UserSpotCloseResponse> close(
            UserSpotCloseRequest request);
    }

    interface ActorCreateOperationHandler {
        CompletionStage<ActorCreateResponse> create(
            ActorCreateRequest request);
    }

    @FunctionalInterface
    interface PeerAuthorityResolver {
        CompletionStage<Optional<PeerAuthorityFence>> resolve(
            String meshName,
            RoutingId sourceNodeRid,
            long sourceNodeGeneration);
    }

    record PeerAuthorityFence(
        RoutingId sourceNodeRid,
        long sourceNodeGeneration,
        String ownerId,
        long ownerLeaseGeneration) {
        public PeerAuthorityFence {
            java.util.Objects.requireNonNull(
                sourceNodeRid, "sourceNodeRid");
            if (sourceNodeGeneration <= 0
                || ownerLeaseGeneration <= 0) {
                throw new IllegalArgumentException(
                    "source node and owner lease generations must be positive");
            }
            if (ownerId == null || ownerId.isBlank()) {
                throw new IllegalArgumentException(
                    "ownerId must be non-blank");
            }
        }
    }

    @FunctionalInterface
    interface RelocationControlHandler {
        CompletionStage<byte[]> handle(
            RoutingId sourceNodeRid,
            byte[] command);
    }

    @FunctionalInterface
    interface CanonicalRelocationControlHandler {
        CompletionStage<Void> handle(
            RoutingId sourceNodeRid,
            byte[] command);
    }

    @FunctionalInterface
    interface MessageFollowHandler {
        void handle(
            RoutingId sourceNodeRid,
            ZLinkServiceMessageFollowWireCodec.Notice notice);
    }

    @FunctionalInterface
    interface RelocationReplyRelayHandler {
        CompletionStage<byte[]> handle(
            RoutingId targetNodeRid,
            byte[] command33,
            List<byte[]> payload);
    }

    @FunctionalInterface
    interface SessionRelocationRouteHandler {
        CompletionStage<byte[]> handle(
            RoutingId sourceNodeRid,
            byte[] command44);
    }

    record UserSpotCreateIntent(
        String spotId,
        String stableType,
        ZLinkServiceM6BWireCodec.ReservationFence reservation,
        long deadlineUnixMs) {
        public UserSpotCreateIntent {
            java.util.Objects.requireNonNull(spotId, "spotId");
            java.util.Objects.requireNonNull(stableType, "stableType");
            java.util.Objects.requireNonNull(
                reservation, "reservation");
        }
    }

    record ActorCreateIntent(
        String actorId,
        String stableType,
        ZLinkServiceM6BWireCodec.ReservationFence reservation,
        long operationHigh,
        long operationLow,
        long deadlineUnixMs) {
        public ActorCreateIntent {
            java.util.Objects.requireNonNull(actorId, "actorId");
            java.util.Objects.requireNonNull(stableType, "stableType");
            java.util.Objects.requireNonNull(
                reservation, "reservation");
            if (operationHigh == 0 && operationLow == 0) {
                throw new IllegalArgumentException(
                    "operationId must not be zero");
            }
        }
    }

    record UserSpotCloseIntent(
        ZLinkServiceM6BWireCodec.UserSpotCloseFence target,
        long deadlineUnixMs) {
        public UserSpotCloseIntent {
            java.util.Objects.requireNonNull(target, "target");
        }
    }

    record UserSpotCreateRequest(
        RoutingId sourceNodeRid,
        long sourceNodeGeneration,
        long operationHigh,
        long operationLow,
        UserSpotCreateIntent intent) {
    }

    record ActorCreateRequest(
        RoutingId sourceNodeRid,
        long sourceNodeGeneration,
        long operationHigh,
        long operationLow,
        ActorCreateIntent intent) {
    }

    record UserSpotCloseRequest(
        RoutingId sourceNodeRid,
        long sourceNodeGeneration,
        long operationHigh,
        long operationLow,
        UserSpotCloseIntent intent) {
    }

    record UserSpotCreateResponse(
        ZLinkServiceM6BWireCodec.UserSpotCreateResult result,
        String spotId,
        long objectGeneration,
        List<systems.zlink.contracts.messaging.Message>
            applicationReply) {
        public UserSpotCreateResponse {
            java.util.Objects.requireNonNull(result, "result");
            java.util.Objects.requireNonNull(spotId, "spotId");
            applicationReply = List.copyOf(
                java.util.Objects.requireNonNull(
                    applicationReply, "applicationReply"));
        }
    }

    record ActorCreateResponse(byte[] terminalEnvelope) {
        public ActorCreateResponse {
            terminalEnvelope = java.util.Objects.requireNonNull(
                terminalEnvelope, "terminalEnvelope").clone();
        }

        @Override
        public byte[] terminalEnvelope() {
            return terminalEnvelope.clone();
        }
    }

    record UserSpotCloseResponse(boolean closed) {
    }

    record SpotAuthorityRoute(
        String spotId,
        long objectGeneration,
        RoutingId targetNodeRid,
        long targetNodeGeneration,
        long authorityOwnerGeneration,
        long ownerLeaseGeneration,
        String ownerId,
        String meshName,
        String storeVersion) {
        public SpotAuthorityRoute {
            java.util.Objects.requireNonNull(spotId, "spotId");
            java.util.Objects.requireNonNull(
                targetNodeRid, "targetNodeRid");
            java.util.Objects.requireNonNull(ownerId, "ownerId");
            java.util.Objects.requireNonNull(meshName, "meshName");
            java.util.Objects.requireNonNull(
                storeVersion, "storeVersion");
        }
    }
}
