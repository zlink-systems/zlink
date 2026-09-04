package systems.zlink.framework.runtime.internal.backend;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;

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
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceMessageFollowWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceRelocationWireCodec;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue;

public interface ZLinkInternalMeshNode extends ZLinkBackendObject {
    default void setApplicationJobQueue(ZLinkApplicationJobQueue value) {
        // Alternate backends may provide their own ordinary-claim boundary.
    }

    default RoutingId routingId() {
        return null;
    }

    default long lifecycleGeneration() {
        return 0L;
    }

    default String localAuthorityOwnerId() {
        RoutingId current = routingId();
        return current == null ? name() : current.toString();
    }

    void setBind(String endpoint);

    default void setAdvertiseHost(String host) {
        // Alternate backends may use the bind address as their advertised
        // endpoint and do not need a separate listener identity.
    }

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
        ZLinkMeshNodeObjectRole role) {
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

    default void setRouterReceiveHighWaterMark(long value) {
        // Optional for test and alternate backends that do not expose Core
        // RouteMesh receive admission yet.
    }

    default void setRouterSendTimeout(Duration value) {
        // Optional for test and alternate backends that do not expose Core
        // RouteMesh admission yet.
    }

    default void setRouterPendingAdmissionCapacity(int value) {
        // Optional for test and alternate backends that do not expose bounded
        // asynchronous RouteMesh admission yet.
    }

    void setRoutingId(RoutingId routingId);

    void start();

    /**
     * Defers the local descriptor's serving transition until the host calls
     * {@link #markServiceReady()} after Framework startup preparation.
     */
    default void deferServiceReadyPublication() {
        // Alternate backends may not publish a separate service descriptor.
    }

    /**
     * Publishes the local service descriptor as available for application
     * selection after the Framework host has finished preparing its handlers,
     * objects, and peer connections.
     */
    default void markServiceReady() {
        // Alternate backends may publish readiness as part of their own host
        // lifecycle and do not expose a separate descriptor transition.
    }

    long connectPeer(String endpoint);

    long connectPeer(String endpoint, RoutingId expectedRoutingId);

    default long connectPeer(
        String endpoint,
        RoutingId expectedRoutingId,
        long expectedLifecycleGeneration,
        String expectedSecurityIdentity) {
        return connectPeer(endpoint, expectedRoutingId);
    }

    default long replacePeerConnection(
        String endpoint,
        RoutingId expectedRoutingId,
        long expectedLifecycleGeneration,
        String expectedSecurityIdentity) {
        return connectPeer(
            endpoint,
            expectedRoutingId,
            expectedLifecycleGeneration,
            expectedSecurityIdentity);
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

    default boolean isPeerConnectionClosing(long connectionIntentId) {
        // Optional for alternate and test backends that do not expose the
        // connection-intent close state.
        return false;
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

    /**
     * Whether the exact admitted transport for a relocation target is
     * available.  Canonical relocation's Prepare is the target-readiness
     * handshake; it must not wait for an independent liveness round-trip
     * before that handshake can be sent.
     */
    default boolean isCanonicalRelocationTargetAdmitted(
        RoutingId peerRid,
        long lifecycleGeneration) {
        return peers().stream().anyMatch(peer ->
            peer.routingId().equals(peerRid)
                && peer.lifecycleGeneration() == lifecycleGeneration
                && peer.state()
                    == systems.zlink.framework.runtime.internal.binding.spot
                        .MeshPeerState.ADMITTED);
    }

    default PeerChannels peerChannels(RoutingId peerRid, long lifecycleGeneration) {
        return new PeerChannels(List.of(), List.of());
    }

    default Map<String, Integer> channelWeights() {
        return Map.of();
    }

    default MeshNodeMonitor openMonitor() {
        throw new UnsupportedOperationException("MeshNode monitor is not available");
    }

    List<Long> connectionIntentIds();

    void startDispatch(Consumer<ZLinkMeshDispatchRecord> receiver);

    default void setApplicationReceiver(ZLinkMeshApplicationReceiver receiver) {
        // Alternate backends may not support process-local Node direct dispatch.
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

    /** Installs the canonical service-wire actorJoin(28) admission owner. */
    default void setCanonicalActorJoinHandler(
        CanonicalActorJoinHandler handler) {
        // Alternate backends may not accept canonical actorJoin service records.
    }

    /**
     * Returns whether the exact observed Spot authority and its admitted peer
     * can carry canonical service-wire actorJoin(28).  A false result keeps
     * the caller on the established private transfer path.
     */
    default boolean canRequestCanonicalActorJoin(
        CanonicalActorJoinRequest request) {
        return false;
    }

    /**
     * Sends one canonical actorJoin(28) request to its exact admitted peer.
     * Transfer and relocation bookkeeping deliberately do not appear in this
     * transport boundary (spec 51 section 9).
     */
    default CompletionStage<CanonicalActorJoinReply> requestCanonicalActorJoin(
        CanonicalActorJoinRequest request,
        Duration timeout) {
        return CompletableFuture.failedFuture(new UnsupportedOperationException(
            "Canonical actorJoin transport is unavailable"));
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
        return CompletableFuture.completedFuture(null);
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
        return CompletableFuture.failedFuture(
            new UnsupportedOperationException(
                "Remote relocation control is unavailable"));
    }

    /** Installs the canonical service-wire command 30-35/40-41 endpoint. */
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
        return CompletableFuture.failedFuture(
            new UnsupportedOperationException(
                "Canonical relocation control is unavailable"));
    }

    /**
     * Sends command 40 as a request and completes with its command 30 or 53
     * reply. Other canonical relocation controls remain one-way.
     */
    default CompletionStage<byte[]> requestCanonicalRelocationPrepare(
        RoutingId targetNodeRid,
        byte[] command,
        Duration timeout) {
        return CompletableFuture.failedFuture(
            new UnsupportedOperationException(
                "Canonical relocation prepare request is unavailable"));
    }

    default void setActorLeftHandler(ActorLeftHandler handler) {
        // Alternate backends may not yet support command 29 dispatch.
    }

    /** Submits one command 29 source-leave record; no reply or ACK exists. */
    default CompletionStage<Void> sendActorLeft(
        RoutingId sourceNodeRid,
        ZLinkServiceM6BWireCodec.ActorLeft left) {
        return CompletableFuture.failedFuture(
            new UnsupportedOperationException(
                "Actor Left notification is unavailable"));
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
        return CompletableFuture.failedFuture(
            new UnsupportedOperationException(
                "Message Follow notification is unavailable"));
    }

    default void setRelocationReplyRelayHandler(
        RelocationReplyRelayHandler handler) {
        // Alternate backends may not support command 33/46 dispatch.
    }

    /**
     * Sends one canonical command 33 record plus its application payload and
     * returns the exact command 46 closed acknowledgement. The landing node is
     * the relocation source that owns the reply capability; it may differ from
     * the request-source fence carried in {@code expectedSource}.
     */
    default CompletionStage<byte[]> requestRelocationReplyRelay(
        RoutingId landingNodeRid,
        ZLinkServiceRelocationWireCodec.RequestSourceFence expectedSource,
        byte[] command33,
        List<byte[]> payload,
        Duration timeout) {
        return CompletableFuture.failedFuture(
            new UnsupportedOperationException(
                "Relocation reply relay is unavailable"));
    }

    default void setSessionRelocationRouteHandler(
        SessionRelocationRouteHandler handler) {
        // Alternate backends may not support command 44 dispatch.
    }

    default void setSessionRelocationSealHandler(
        SessionRelocationSealHandler handler) {
        // Alternate backends may not support command 42/43 dispatch.
    }

    default void setBoundSessionSendHandler(
        BoundSessionSendHandler handler) {
        // Alternate backends may not support command 36 dispatch.
    }

    /** Submits one exact command 42 record and returns its command 43 ACK. */
    default CompletionStage<byte[]> requestSessionRelocationSeal(
        RoutingId sessionOwnerNodeRid,
        byte[] command42,
        Duration timeout) {
        return CompletableFuture.failedFuture(
            new UnsupportedOperationException(
                "Session relocation seal is unavailable"));
    }

    /** Submits the one-way command 44 to the exact Session owner. */
    default CompletionStage<Void> sendSessionRelocationRoute(
        RoutingId sessionOwnerNodeRid,
        byte[] command44) {
        return CompletableFuture.failedFuture(
            new UnsupportedOperationException(
                "Session relocation routing is unavailable"));
    }

    /**
     * Installs the one-way command 51 endpoint. The record is infrastructure
     * state and must not enter the application mailbox.
     */
    default void setBoundSessionReplacedHandler(
        BoundSessionReplacedHandler handler) {
        // Alternate backends may not support bound-session replacement.
    }

    /**
     * Sends command 51 to the exact retired Session owner. Admission failure
     * is owned by the sender's bounded asynchronous retry queue; no ACK is
     * expected for this one-way record.
     */
    default CompletionStage<Void> sendBoundSessionReplaced(
        RoutingId sessionOwnerNodeRid,
        ZLinkServiceM6BWireCodec.BoundSessionReplaced replacement) {
        return CompletableFuture.failedFuture(
            new UnsupportedOperationException(
                "Bound Session replacement notification is unavailable"));
    }


    default CompletionStage<ActorCreateResponse> requestActorCreate(
        RoutingId targetNodeRid,
        ActorCreateIntent intent,
        Duration timeout) {
        return CompletableFuture.failedFuture(
            new UnsupportedOperationException(
                "Remote Actor create is unavailable"));
    }

    default CompletionStage<UserSpotCreateResponse>
        requestUserSpotCreate(
            RoutingId targetNodeRid,
            UserSpotCreateIntent intent,
            Duration timeout) {
        return CompletableFuture.failedFuture(
            new UnsupportedOperationException(
                "Remote User Spot create is unavailable"));
    }

    default CompletionStage<UserSpotCloseResponse>
        requestUserSpotClose(
            RoutingId targetNodeRid,
            UserSpotCloseIntent intent,
            Duration timeout) {
        return CompletableFuture.failedFuture(
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
        List<Message> parts) {
        return CompletableFuture.failedFuture(
            new UnsupportedOperationException(
                "Remote Instance Spot send is unavailable"));
    }

    default CompletionStage<Void> submitInstanceSpotSend(
        ZLinkServiceM6BWireCodec.InstanceRouteFence route,
        String stableType,
        String sourceSpotId,
        byte[] metadata,
        List<Message> parts,
        Duration timeout) {
        return submitInstanceSpotSend(
            route, stableType, sourceSpotId, metadata, parts);
    }

    default CompletionStage<List<Message>>
        requestInstanceSpot(
        ZLinkServiceM6BWireCodec.InstanceRouteFence route,
        String stableType,
        String sourceSpotId,
            byte[] metadata,
            List<Message> parts,
            Duration timeout) {
        return CompletableFuture.failedFuture(
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
    interface CanonicalActorJoinHandler {
        CompletionStage<CanonicalActorJoinResponse> admit(
            RoutingId sourceNodeRid,
            systems.zlink.framework.runtime.protocol.ServiceWirePilotCodec
                .ActorJoin28 join);
    }

    record CanonicalActorJoinRequest(
        ZLinkBackendActorRef actor,
        long actorNodeGeneration,
        long actorAuthorityOwnerGeneration,
        long actorOwnerLeaseGeneration,
        RoutingId targetNodeRid,
        long targetNodeGeneration,
        String targetSpotId,
        long targetSpotGeneration,
        long targetAuthorityOwnerGeneration,
        long targetOwnerLeaseGeneration,
        boolean entry,
        String packetName,
        String contentType,
        byte[] applicationPayload) {
        public CanonicalActorJoinRequest {
            Objects.requireNonNull(actor, "actor");
            Objects.requireNonNull(targetNodeRid, "targetNodeRid");
            Objects.requireNonNull(targetSpotId, "targetSpotId");
            Objects.requireNonNull(packetName, "packetName");
            Objects.requireNonNull(contentType, "contentType");
            applicationPayload = Objects.requireNonNull(
                applicationPayload, "applicationPayload").clone();
        }
    }

    record CanonicalActorJoinReply(
        boolean accepted,
        long receiveChunkLimitBytes,
        List<Message> applicationReply,
        java.util.UUID handoffId,
        String replyContentType) {
        public CanonicalActorJoinReply(
            boolean accepted,
            long receiveChunkLimitBytes,
            List<Message> applicationReply) {
            this(
                accepted,
                receiveChunkLimitBytes,
                applicationReply,
                null,
                "application/json");
        }

        public CanonicalActorJoinReply {
            applicationReply = List.copyOf(Objects.requireNonNull(
                applicationReply, "applicationReply"));
            Objects.requireNonNull(replyContentType, "replyContentType");
        }
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
            Objects.requireNonNull(
                sourceNodeRid, "sourceNodeRid");
            if (sourceNodeGeneration == 0
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
        CompletionStage<byte[]> handle(
            RoutingId sourceNodeRid,
            Long requestSequence,
            byte[] command);
    }

    /** Marks a node that can carry command 40's required reply leg. */
    interface CanonicalRelocationPrepareRequestReplySupport {
    }

    @FunctionalInterface
    interface ActorLeftHandler {
        CompletionStage<Void> handle(
            RoutingId targetNodeRid,
            ZLinkServiceM6BWireCodec.ActorLeft left);
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
        CompletionStage<Void> handle(
            RoutingId sourceNodeRid,
            byte[] command44);
    }

    @FunctionalInterface
    interface SessionRelocationSealHandler {
        CompletionStage<byte[]> handle(
            RoutingId sourceNodeRid,
            byte[] command42);
    }

    @FunctionalInterface
    interface BoundSessionSendHandler {
        CompletionStage<Boolean> handle(
            RoutingId sourceNodeRid,
            long sourceNodeGeneration,
            ZLinkServiceM6BWireCodec.BoundSessionSend command,
            ZLinkServiceM6AWireCodec.ApplicationPayload payload);
    }

    @FunctionalInterface
    interface BoundSessionReplacedHandler {
        void handle(
            RoutingId sourceNodeRid,
            ZLinkServiceM6BWireCodec.BoundSessionReplaced replacement);
    }

    record UserSpotCreateIntent(
        String spotId,
        String stableType,
        ZLinkServiceM6BWireCodec.ReservationFence reservation,
        long deadlineUnixMs) {
        public UserSpotCreateIntent {
            Objects.requireNonNull(spotId, "spotId");
            Objects.requireNonNull(stableType, "stableType");
            Objects.requireNonNull(
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
            Objects.requireNonNull(actorId, "actorId");
            Objects.requireNonNull(stableType, "stableType");
            Objects.requireNonNull(
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
            Objects.requireNonNull(target, "target");
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
        List<Message>
            applicationReply) {
        public UserSpotCreateResponse {
            Objects.requireNonNull(result, "result");
            Objects.requireNonNull(spotId, "spotId");
            applicationReply = List.copyOf(
                Objects.requireNonNull(
                    applicationReply, "applicationReply"));
        }
    }

    record ActorCreateResponse(byte[] terminalEnvelope) {
        public ActorCreateResponse {
            terminalEnvelope = Objects.requireNonNull(
                terminalEnvelope, "terminalEnvelope").clone();
        }

        @Override
        public byte[] terminalEnvelope() {
            return terminalEnvelope.clone();
        }
    }

    record CanonicalActorJoinResponse(
        boolean accepted,
        long membershipEpoch,
        List<Message> applicationReply) {
        public CanonicalActorJoinResponse {
            if (accepted && membershipEpoch <= 0) {
                throw new IllegalArgumentException(
                    "accepted canonical Actor Join requires membership epoch");
            }
            applicationReply = List.copyOf(Objects.requireNonNull(
                applicationReply, "applicationReply"));
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
            Objects.requireNonNull(spotId, "spotId");
            Objects.requireNonNull(
                targetNodeRid, "targetNodeRid");
            Objects.requireNonNull(ownerId, "ownerId");
            Objects.requireNonNull(meshName, "meshName");
            Objects.requireNonNull(
                storeVersion, "storeVersion");
        }
    }
}
