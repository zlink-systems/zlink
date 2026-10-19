package systems.zlink.framework.runtime.internal.backend;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.runtime.internal.binding.spot.ActorTransferPrepare;
import systems.zlink.framework.runtime.internal.binding.spot.ActorTransferToken;
import systems.zlink.framework.runtime.internal.binding.spot.PrepareActorTransferResult;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationRegistry;

import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.Consumer;
import java.util.function.Supplier;

public interface ZLinkInternalSpotNode extends ZLinkBackendObject {
    /** Node-owned JVM source turn; backends without a separate owner use the selector turn. */
    default ZLinkStateLane sourceTransportLane() {
        return null;
    }

    RoutingId routingId();

    void setRoutingId(RoutingId routingId);

    void setPublisherRoutingId(RoutingId routingId);

    void setSubscriberRoutingId(RoutingId routingId);

    void setRouterBind(String endpoint);

    void setPubBind(String endpoint);

    void connectPeer(String endpoint);

    void connectPeer(RoutingId peerRid, String endpoint);

    void disconnectPeer(String endpoint);

    void disconnectPeer(RoutingId peerRid);

    default void setApplicationReceiver(ZLinkMeshApplicationReceiver receiver) {
        // Alternate backends may not support process-local Node direct dispatch.
    }

    /**
     * Installs the owner-layer relay for a stale Actor route. The handler may return true only
     * after it takes ownership of parts and the dispatch lease; false leaves both with the caller.
     */
    default void setMessageFollowRelayHandler(MessageFollowRelayHandler handler) {
        // Alternate backends may not support the raw service Actor relay.
    }

    /**
     * Installs the target-side owner for raw application ingress that reaches an object while a
     * relocation stage is still hidden. The handler takes ownership only when it returns {@code
     * true}; otherwise normal live-route admission continues.
     */
    default void setRelocationStagingIngressHandler(RelocationStagingIngressHandler handler) {
        // Alternate backends may not expose raw relocation staging.
    }

    /** Resolves current Spot authority; the activation owns Instance application admission. */
    default void setSpotAdmissionResolver(SpotAdmissionResolver resolver) {
        // Alternate backends may not expose raw target ingress.
    }

    @FunctionalInterface
    interface SpotAdmissionResolver {
        /** A null result leaves ingress to the existing target queue admission. */
        CompletionStage<? extends RuntimeException> resolve(
                String spotId,
                long authorityOwnerGeneration,
                boolean instanceIntent,
                boolean activationPresent,
                ZLinkServiceM6BWireCodec.InstanceRouteFence instanceRoute);
    }

    /**
     * Redirects an exact stale Spot route to the staged target through the existing application
     * wire. The redirect expires with Message Follow and never changes a different generation or
     * authority fence.
     */
    default void installRelocationSpotForward(
            ZLinkServiceM6BWireCodec.SpotRouteFence source,
            ZLinkServiceM6BWireCodec.SpotRouteFence target,
            Duration retention) {
        throw new UnsupportedOperationException("raw Spot relocation forwarding is unavailable");
    }

    /**
     * Relays one exact stale Actor operation through the route selected by the Message Follow
     * owner. The implementation preserves the original raw operation envelope and completes with
     * the original reply frames.
     */
    default CompletionStage<List<Message>> forwardMessageFollowActor(
            ZLinkServiceM6BWireCodec.ActorMessage stale,
            ZLinkServiceM6BWireCodec.ActorRouteFence target,
            List<Message> parts) {
        return CompletableFuture.failedFuture(
                new UnsupportedOperationException(
                        "raw Actor Message Follow forwarding is unavailable"));
    }

    default Optional<CompletionStage<Integer>> submitLocalNodeSend(
            RoutingId targetNodeRid, byte[] metadata, List<Message> parts) {
        return Optional.empty();
    }

    default Optional<Integer> classifyNodeSendTarget(RoutingId targetNodeRid) {
        return Optional.empty();
    }

    /**
     * Classifies a RouteMesh ChannelName before a request or send is built. Implementations return
     * an admission status only when the channel has no selectable target; an empty result preserves
     * the normal transport path.
     */
    default Optional<Integer> classifyChannelTarget(String channelName) {
        return Optional.empty();
    }

    ZLinkBackendSpotRouteBridge createRouteBridge();

    ZLinkBackendSpot createSpot();

    default ZLinkBackendSpot createSpot(String spotId) {
        ZLinkBackendSpot spot = createSpot();
        spot.setRoutingId(spotId);
        return spot;
    }

    default ZLinkBackendSpot createSpot(String spotId, long objectGeneration) {
        throw new UnsupportedOperationException("Exact-generation Spot creation is unavailable");
    }

    ZLinkBackendSpot entrySpot();

    default CompletionStage<Void> sendToNode(RoutingId targetNodeRid, List<Message> parts) {
        throw new UnsupportedOperationException("MeshNode node send is unavailable");
    }

    default CompletionStage<Void> sendToNode(
            RoutingId targetNodeRid, byte[] metadata, List<Message> parts) {
        if (metadata == null || metadata.length == 0) {
            return sendToNode(targetNodeRid, parts);
        }
        throw new UnsupportedOperationException("MeshNode node metadata is unavailable");
    }

    /** Uses the caller's operation lifetime for registration and submission. */
    default CompletionStage<Void> sendToChannel(String channelName, List<Message> parts) {
        throw new UnsupportedOperationException("MeshNode channel send is unavailable");
    }

    default CompletionStage<Void> sendToChannel(
            String channelName, byte[] metadata, List<Message> parts) {
        if (metadata == null || metadata.length == 0) {
            return sendToChannel(channelName, parts);
        }
        throw new UnsupportedOperationException("MeshNode channel metadata is unavailable");
    }

    /** Uses the caller's operation lifetime; selector implementations select before this turn. */
    default void publish(String channelName, String topic, List<Message> parts, SendFlags flags) {
        throw new UnsupportedOperationException("MeshNode publish is unavailable");
    }

    default void publish(
            String channelName,
            String topic,
            byte[] metadata,
            List<Message> parts,
            SendFlags flags) {
        if (metadata == null || metadata.length == 0) {
            publish(channelName, topic, parts, flags);
            return;
        }
        throw new UnsupportedOperationException("MeshNode publish metadata is unavailable");
    }

    ZLinkBackendActorRef createActor(String actorId, Message createRequest);

    default ZLinkBackendActorRef createActor(
            String actorId, long objectGeneration, Message createRequest) {
        ZLinkBackendActorRef created = createActor(actorId, createRequest);
        if (created.generation() != objectGeneration) {
            throw new IllegalStateException(
                    "backend Actor generation does not match the reserved "
                            + "Location Store generation");
        }
        return created;
    }

    ZLinkBackendActorRef actorLookup(String actorId);

    default boolean hasPendingActorRequests() {
        return false;
    }

    default CompletionStage<Void> awaitPendingActorRequests() {
        if (hasPendingActorRequests()) {
            return CompletableFuture.failedFuture(
                    new UnsupportedOperationException(
                            "backend must expose pending Actor request terminals"));
        }
        return CompletableFuture.completedFuture(null);
    }

    default long localAuthorityLeaseGeneration() {
        return 0L;
    }

    /** Exact local owner fence used by bound-Session relocation command 42. */
    default long localNodeGeneration() {
        return 0L;
    }

    /** Node lifecycle generation currently known for this Actor route. */
    default long actorNodeGeneration(ZLinkBackendActorRef actor) {
        return 0L;
    }

    /** Authority generation currently known for this Actor route. */
    default long actorAuthorityOwnerGeneration(ZLinkBackendActorRef actor) {
        return 0L;
    }

    /** Authority lease generation currently known for this Actor route. */
    default long actorAuthorityOwnerLeaseGeneration(ZLinkBackendActorRef actor) {
        return 0L;
    }

    default void rememberActorAuthority(
            ZLinkBackendActorRef actor, long authorityOwnerGeneration) {}

    default void rememberActorAuthority(
            ZLinkBackendActorRef actor, long authorityOwnerGeneration, long ownerLeaseGeneration) {
        rememberActorAuthority(actor, authorityOwnerGeneration);
    }

    /** Observes a route for outbound use; it does not publish a local native Actor stay. */
    default void observeActorAuthority(
            ZLinkBackendActorRef actor, long authorityOwnerGeneration, long ownerLeaseGeneration) {
        rememberActorAuthority(actor, authorityOwnerGeneration, ownerLeaseGeneration);
    }

    CompletionStage<ZLinkBackendActorJoinResult> joinActor(
            ZLinkBackendActorRef actor,
            RoutingId targetNodeRid,
            String targetSpotId,
            List<Message> parts,
            Duration timeout);

    default CompletionStage<ZLinkBackendActorJoinResult> joinActor(
            ZLinkBackendActorRef actor,
            RoutingId targetNodeRid,
            String targetSpotId,
            long targetSpotGeneration,
            List<Message> parts,
            Duration timeout) {
        return joinActor(actor, targetNodeRid, targetSpotId, parts, timeout);
    }

    CompletionStage<ZLinkBackendActorJoinEntrySpotResult> joinActorEntrySpot(
            ZLinkBackendActorRef actor, RoutingId targetNodeRid, Message request, Duration timeout);

    CompletionStage<Void> destroyActor(ZLinkBackendActorRef actor, Duration timeout);

    /** Destroys only the native stay with this authority generation. */
    default CompletionStage<Void> destroyActor(
            ZLinkBackendActorRef actor, long authorityOwnerGeneration, Duration timeout) {
        return destroyActor(actor, timeout);
    }

    default PrepareActorTransferResult prepareActorTransfer(
            ActorTransferPrepare prepare, Duration timeout) {
        throw new UnsupportedOperationException("Core actor transfer is unavailable");
    }

    default void commitActorTransfer(ActorTransferToken token, long newMembershipEpoch) {
        throw new UnsupportedOperationException("Core actor transfer is unavailable");
    }

    default void activateActorTransfer(ActorTransferToken token) {
        throw new UnsupportedOperationException("Core actor transfer is unavailable");
    }

    default void abortActorTransfer(ActorTransferToken token) {
        throw new UnsupportedOperationException("Core actor transfer is unavailable");
    }

    default long actorMembershipEpoch(String actorId) {
        return 0L;
    }

    default void registerTransferredActor(
            ZLinkBackendActorRef actor, String spotId, long membershipEpoch) {
        throw new UnsupportedOperationException("Core actor transfer is unavailable");
    }

    /** Publishes one target native Actor stay, including its authority fence. */
    default void registerTransferredActor(
            ZLinkBackendActorRef actor,
            String spotId,
            long membershipEpoch,
            long authorityOwnerGeneration,
            long ownerLeaseGeneration) {
        registerTransferredActor(actor, spotId, membershipEpoch);
        rememberActorAuthority(actor, authorityOwnerGeneration, ownerLeaseGeneration);
    }

    boolean sendActorBoundSession(ZLinkBackendActorRef actor, List<Message> parts, SendFlags flags);

    /**
     * Reports the logical remote bound-Session route only. The subsequent physical ROUTER admission
     * is owned by {@link #sendRemoteActorBoundSession}.
     */
    default boolean hasRemoteActorBoundSessionRoute(ZLinkBackendActorRef actor) {
        return boundSessionRoute(actor).isPresent();
    }

    /** Reports whether this Node owns the Actor's physical STREAM binding. */
    default boolean hasLocalActorBoundSessionRoute(ZLinkBackendActorRef actor) {
        return false;
    }

    /** Retains the existing synchronous STREAM admission path. */
    default boolean sendLocalActorBoundSession(
            ZLinkBackendActorRef actor, List<Message> parts, SendFlags flags) {
        return sendActorBoundSession(actor, parts, flags);
    }

    /** Submits one local bound-Session frame through binding-owned admission. */
    default CompletionStage<Void> sendLocalActorBoundSessionAsync(
            ZLinkBackendActorRef actor, List<Message> parts) {
        return CompletableFuture.failedFuture(
                new UnsupportedOperationException(
                        "local bound-Session async admission is unavailable"));
    }

    /** Submits one remote bound-Session frame through binding-owned admission. */
    default CompletionStage<Void> sendRemoteActorBoundSession(
            ZLinkBackendActorRef actor, List<Message> parts) {
        try {
            return sendActorBoundSession(actor, parts, SendFlags.DONT_WAIT)
                    ? CompletableFuture.completedFuture(null)
                    : systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls.adaptOneWay(
                            CompletableFuture.failedFuture(
                                    new ZlinkSubmitException(SubmitResult.BACKPRESSURED)),
                            true);
        } catch (RuntimeException failure) {
            return systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls.adaptOneWay(
                    CompletableFuture.failedFuture(failure), true);
        }
    }

    void replyActorNoBind(
            ZLinkBackendActorRef actor,
            RoutingId sourceNodeRid,
            RoutingId sourceSessionRid,
            long requestId,
            int flags,
            List<Message> parts);

    boolean sendToActor(
            ZLinkServiceM6BWireCodec.ActorRouteFence actor, List<Message> parts, SendFlags flags);

    /** Submits an Actor send through its owning ingress admission. */
    default CompletionStage<Void> sendToActorAsync(
            ZLinkServiceM6BWireCodec.ActorRouteFence actor, List<Message> parts) {
        try {
            return sendToActor(actor, parts, SendFlags.DONT_WAIT)
                    ? CompletableFuture.completedFuture(null)
                    : systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls.adaptOneWay(
                            CompletableFuture.failedFuture(
                                    new ZlinkSubmitException(SubmitResult.BACKPRESSURED)),
                            true);
        } catch (RuntimeException failure) {
            return systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls.adaptOneWay(
                    CompletableFuture.failedFuture(failure), true);
        }
    }

    boolean forwardActorBoundSession(
            ZLinkBackendActorRef actor,
            RoutingId sourceNodeRid,
            RoutingId sourceSessionRid,
            List<Message> parts,
            SendFlags flags);

    default byte[] encodeLocalSessionActorAccepted(
            ZLinkBackendActorRef actor,
            RoutingId sourceNodeRid,
            RoutingId sourceSessionRid,
            long sourceBindingGeneration,
            long sourceSessionSequence,
            long requestSequence,
            String packetName,
            Map<String, String> metadata,
            byte[] payload) {
        return new byte[0];
    }

    /**
     * Installs the relocating bound Session route as one remote binding transition of the {@link
     * RemoteSessionBindingOwner}, which binds the Actor context in the same owner turn.
     */
    default CompletionStage<Void> installRelocatingActorBoundSession(
            ZLinkServiceM6BWireCodec.ActorRouteFence actor,
            ZLinkServiceM6BWireCodec.SessionOwnerFence session) {
        // Alternate backends may wait for the ordinary binding announcement.
        return CompletableFuture.completedFuture(null);
    }

    default Optional<BoundSessionRoute> boundSessionRoute(ZLinkBackendActorRef actor) {
        return Optional.empty();
    }

    default CompletionStage<Void> closeActorBoundSessionAsync(
            ZLinkBackendActorRef actor, Duration timeout) {
        return java.util.concurrent.CompletableFuture.failedFuture(
                new UnsupportedOperationException("Session close is unavailable"));
    }

    void closeActorBoundSession(ZLinkBackendActorRef actor, Duration timeout);

    /** Closes only the binding of the supplied source stay and, when supplied, Session. */
    default void closeActorBoundSession(
            ZLinkBackendActorRef actor,
            RoutingId sessionRid,
            long authorityOwnerGeneration,
            Duration timeout) {
        closeActorBoundSession(actor, timeout);
    }

    /**
     * A committed relocation moved the Actor away: retires exactly the binding record of the stay
     * that ended, matched on the Actor, its Session and that stay's authority owner generation.
     */
    default CompletionStage<Void> retireDepartedActorBinding(
            ZLinkBackendActorRef actor, RoutingId sessionRid, long authorityOwnerGeneration) {
        return CompletableFuture.completedFuture(null);
    }

    /**
     * Completes when every Instance activation this node accepted has reached its terminal,
     * including the first message's Store cleanup. Host shutdown waits for it inside its own drain
     * deadline before it closes Spots.
     */
    default CompletionStage<Void> instanceActivationTerminals() {
        return CompletableFuture.completedFuture(null);
    }

    record BoundSessionRoute(
            RoutingId sessionOwnerNodeRid,
            long sessionOwnerNodeGeneration,
            RoutingId sessionRid,
            long bindingGeneration) {}

    default void setRemoteSessionBindingOwner(RemoteSessionBindingOwner owner) {}

    /**
     * Owner of the remote bound Session fact. The node's remote route and the Actor context change
     * only inside {@link #apply}, which runs the transition in the owner turn; a caller already in
     * that turn runs it inline.
     */
    @FunctionalInterface
    interface RemoteSessionBindingOwner {
        CompletionStage<Boolean> apply(
                ZLinkInternalSpotNode node,
                ZLinkBackendActorRef actor,
                java.util.function.Function<RemoteSessionBindingTransition, Boolean> transition);
    }

    /** Actor-context side of one remote binding transition, applied in the owner turn. */
    interface RemoteSessionBindingTransition {
        RemoteSessionBindingTransition NONE =
                new RemoteSessionBindingTransition() {
                    @Override
                    public void bind(BoundSessionRoute route) {}

                    @Override
                    public void retire(BoundSessionRoute route) {}
                };

        void bind(BoundSessionRoute route);

        void retire(BoundSessionRoute route);
    }

    @FunctionalInterface
    interface MessageFollowRelayHandler {
        /**
         * An authoritative absence skips local relay setup. Owners with no concurrent existence
         * read retain the full handler path; true still requires exact admission in handle.
         */
        default boolean hasSource(String actorId) {
            return true;
        }

        boolean handle(
                RoutingId sourceNodeRid,
                long sourceNodeGeneration,
                ZLinkServiceM6BWireCodec.ActorMessage header,
                byte[] acceptedJournalRecord,
                List<Message> parts,
                String contentType,
                Consumer<List<Message>> reply,
                Consumer<Throwable> failure,
                Runnable terminalRelease);
    }

    interface RelocationStagingIngressHandler {
        boolean handleSpot(
                ZLinkInternalMeshNode.PeerAuthorityFence source,
                ZLinkServiceM6BWireCodec.SpotMessage header,
                byte[] metadata,
                Supplier<byte[]> acceptedJournalRecord,
                int acceptedJournalRecordSizeHint,
                List<Message> parts,
                String contentType,
                Consumer<List<Message>> reply,
                Consumer<Throwable> failure);

        boolean handleActor(
                ZLinkInternalMeshNode.PeerAuthorityFence source,
                ZLinkServiceM6BWireCodec.ActorMessage header,
                Supplier<byte[]> acceptedJournalRecord,
                List<Message> parts,
                String contentType,
                Consumer<List<Message>> reply,
                Consumer<Throwable> failure);
    }

    CompletionStage<ZLinkBackendReceived> requestToNode(
            RoutingId targetNodeRid,
            byte[] metadata,
            List<Message> parts,
            Duration timeout,
            ZLinkServiceOperationRegistry operations,
            UUID operationId,
            CompletableFuture<Void> requestAdmission);

    CompletionStage<ZLinkBackendReceived> requestToChannel(
            String channelName,
            byte[] metadata,
            List<Message> parts,
            Duration timeout,
            ZLinkServiceOperationRegistry operations,
            UUID operationId,
            CompletableFuture<Void> requestAdmission);

    CompletionStage<List<Message>> requestToActor(
            ZLinkServiceM6BWireCodec.ActorRouteFence actor,
            List<Message> parts,
            SendFlags flags,
            Duration timeout,
            CompletableFuture<Void> requestAdmission);
}
