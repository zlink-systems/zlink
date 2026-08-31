package systems.zlink.framework.runtime.internal.backend;
import java.util.Map;
import java.util.Optional;

import systems.zlink.framework.runtime.internal.backend.ZLinkBackendObject;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorJoinEntrySpotResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorJoinResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotRouteBridge;
import java.util.List;
import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.Consumer;
import java.util.function.Supplier;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.binding.spot.ActorTransferId;
import systems.zlink.framework.runtime.internal.binding.spot.ActorTransferPrepare;
import systems.zlink.framework.runtime.internal.binding.spot.ActorTransferPrepareResult;
import systems.zlink.framework.runtime.internal.binding.spot.ActorTransferToken;
import systems.zlink.framework.runtime.internal.binding.spot.PrepareActorTransferResult;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.sockets.SubmitResult;


public interface ZLinkInternalSpotNode extends ZLinkBackendObject {
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
     * Installs the owner-layer relay for a stale Actor route. The handler may
     * return true only after it takes ownership of parts and the dispatch
     * lease; false leaves both with the caller.
     */
    default void setMessageFollowRelayHandler(
        MessageFollowRelayHandler handler) {
        // Alternate backends may not support the raw service Actor relay.
    }

    /**
     * Installs the target-side owner for raw application ingress that reaches
     * an object while a relocation stage is still hidden. The handler takes
     * ownership only when it returns {@code true}; otherwise normal live-route
     * admission continues.
     */
    default void setRelocationStagingIngressHandler(
        RelocationStagingIngressHandler handler) {
        // Alternate backends may not expose raw relocation staging.
    }

    /**
     * Redirects an exact stale Spot route to the staged target through the
     * existing application wire. The redirect expires with Message Follow and
     * never changes a different generation or authority fence.
     */
    default void installRelocationSpotForward(
        ZLinkServiceM6BWireCodec.SpotRouteFence source,
        ZLinkServiceM6BWireCodec.SpotRouteFence target,
        Duration retention) {
        throw new UnsupportedOperationException(
            "raw Spot relocation forwarding is unavailable");
    }

    /**
     * Relays one exact stale Actor operation through the route selected by the
     * Message Follow owner. The implementation preserves the original raw
     * operation envelope and completes with the original reply frames.
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
        RoutingId targetNodeRid,
        byte[] metadata,
        List<Message> parts) {
        return Optional.empty();
    }

    default Optional<Integer> classifyNodeSendTarget(
        RoutingId targetNodeRid) {
        return Optional.empty();
    }

    /**
     * Classifies a RouteMesh ChannelName before a request or send is built.
     * Implementations return an admission status only when the channel has no
     * selectable target; an empty result preserves the normal transport path.
     */
    default Optional<Integer> classifyChannelTarget(
        String channelName) {
        return Optional.empty();
    }

    ZLinkBackendSpotRouteBridge createRouteBridge();

    ZLinkBackendSpot createSpot();

    default ZLinkBackendSpot createSpot(String spotId) {
        ZLinkBackendSpot spot = createSpot();
        spot.setRoutingId(spotId);
        return spot;
    }

    default ZLinkBackendSpot createSpot(
        String spotId,
        long objectGeneration) {
        throw new UnsupportedOperationException(
            "Exact-generation Spot creation is unavailable");
    }

    ZLinkBackendSpot entrySpot();

    default CompletionStage<Void> sendToNode(
        RoutingId targetNodeRid,
        List<Message> parts) {
        throw new UnsupportedOperationException("MeshNode node send is unavailable");
    }

    default CompletionStage<Void> sendToNode(
        RoutingId targetNodeRid,
        byte[] metadata,
        List<Message> parts) {
        if (metadata == null || metadata.length == 0) {
            return sendToNode(targetNodeRid, parts);
        }
        throw new UnsupportedOperationException("MeshNode node metadata is unavailable");
    }

    default CompletionStage<ZLinkBackendReceived> requestToNode(
        RoutingId targetNodeRid,
        List<Message> parts,
        Duration timeout) {
        throw new UnsupportedOperationException("MeshNode node request is unavailable");
    }

    default CompletionStage<ZLinkBackendReceived> requestToNode(
        RoutingId targetNodeRid,
        byte[] metadata,
        List<Message> parts,
        Duration timeout) {
        if (metadata == null || metadata.length == 0) {
            return requestToNode(targetNodeRid, parts, timeout);
        }
        throw new UnsupportedOperationException("MeshNode node request metadata is unavailable");
    }

    default CompletionStage<Void> sendToChannel(
        String channelName,
        List<Message> parts) {
        throw new UnsupportedOperationException("MeshNode channel send is unavailable");
    }

    default CompletionStage<Void> sendToChannel(
        String channelName,
        byte[] metadata,
        List<Message> parts) {
        if (metadata == null || metadata.length == 0) {
            return sendToChannel(channelName, parts);
        }
        throw new UnsupportedOperationException("MeshNode channel metadata is unavailable");
    }

    default CompletionStage<ZLinkBackendReceived> requestToChannel(
        String channelName,
        List<Message> parts,
        Duration timeout) {
        throw new UnsupportedOperationException("MeshNode channel request is unavailable");
    }

    default CompletionStage<ZLinkBackendReceived> requestToChannel(
        String channelName,
        byte[] metadata,
        List<Message> parts,
        Duration timeout) {
        if (metadata == null || metadata.length == 0) {
            return requestToChannel(channelName, parts, timeout);
        }
        throw new UnsupportedOperationException(
            "MeshNode channel request metadata is unavailable");
    }

    default void publish(
        String channelName,
        String topic,
        List<Message> parts,
        SendFlags flags) {
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
        String actorId,
        long objectGeneration,
        Message createRequest) {
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

    default long localAuthorityLeaseGeneration() {
        return 0L;
    }

    /** Exact local owner fence used by bound-Session relocation command 42. */
    default long localNodeGeneration() {
        return 0L;
    }

    /** Exact local authority owner used by bound-Session relocation command 42. */
    default String localAuthorityOwnerId() {
        return "";
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
        ZLinkBackendActorRef actor,
        long authorityOwnerGeneration) {
    }

    default void rememberActorAuthority(
        ZLinkBackendActorRef actor,
        long authorityOwnerGeneration,
        long ownerLeaseGeneration) {
        rememberActorAuthority(actor, authorityOwnerGeneration);
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
        ZLinkBackendActorRef actor,
        RoutingId targetNodeRid,
        Message request,
        Duration timeout);

    CompletionStage<List<Message>> leaveActor(
        ZLinkBackendActorRef actor,
        String currentSpotId,
        Duration timeout);

    CompletionStage<Void> destroyActor(
        ZLinkBackendActorRef actor,
        Duration timeout);

    default PrepareActorTransferResult prepareActorTransfer(
        ActorTransferPrepare prepare, Duration timeout) {
        throw new UnsupportedOperationException("Core actor transfer is unavailable");
    }

    default void commitActorTransfer(
        ActorTransferToken token, long newMembershipEpoch) {
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

    boolean sendActorBoundSession(ZLinkBackendActorRef actor, List<Message> parts, SendFlags flags);

    /**
     * Reports the logical remote bound-Session route only. The subsequent
     * physical ROUTER admission is owned by {@link #sendRemoteActorBoundSession}.
     */
    default boolean hasRemoteActorBoundSessionRoute(
        ZLinkBackendActorRef actor) {
        return boundSessionRoute(actor).isPresent();
    }

    /** Reports whether this Node owns the Actor's physical STREAM binding. */
    default boolean hasLocalActorBoundSessionRoute(
        ZLinkBackendActorRef actor) {
        return false;
    }

    /** Retains the existing synchronous STREAM admission path. */
    default boolean sendLocalActorBoundSession(
        ZLinkBackendActorRef actor,
        List<Message> parts,
        SendFlags flags) {
        return sendActorBoundSession(actor, parts, flags);
    }

    /** Submits one local bound-Session frame through binding-owned admission. */
    default CompletionStage<Void> sendLocalActorBoundSessionAsync(
        ZLinkBackendActorRef actor,
        List<Message> parts,
        Duration timeout) {
        return CompletableFuture.failedFuture(
            new UnsupportedOperationException(
                "local bound-Session async admission is unavailable"));
    }

    /** Submits one remote bound-Session frame through binding-owned admission. */
    default CompletionStage<Void> sendRemoteActorBoundSession(
        ZLinkBackendActorRef actor,
        List<Message> parts) {
        try {
            return sendActorBoundSession(actor, parts, SendFlags.DONT_WAIT)
                ? CompletableFuture.completedFuture(null)
                : CompletableFuture.failedFuture(
                    new ZlinkSubmitException(SubmitResult.BACKPRESSURED));
        } catch (RuntimeException failure) {
            return CompletableFuture.failedFuture(failure);
        }
    }

    void replyActorNoBind(
        ZLinkBackendActorRef actor,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        long requestId,
        int flags,
        List<Message> parts);

    boolean sendToActor(ZLinkBackendActorRef actor, List<Message> parts, SendFlags flags);

    /** Submits one remote Actor send through binding-owned admission. */
    default CompletionStage<Void> sendToActorAsync(
        ZLinkBackendActorRef actor,
        List<Message> parts) {
        try {
            return sendToActor(actor, parts, SendFlags.DONT_WAIT)
                ? CompletableFuture.completedFuture(null)
                : CompletableFuture.failedFuture(
                    new ZlinkSubmitException(SubmitResult.BACKPRESSURED));
        } catch (RuntimeException failure) {
            return CompletableFuture.failedFuture(failure);
        }
    }

    CompletionStage<List<Message>> requestToActor(
        ZLinkBackendActorRef actor,
        List<Message> parts,
        SendFlags flags,
        Duration timeout);

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

    void bindRemoteActorBoundSession(
        ZLinkBackendActorRef actor,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid);

    default void installRelocatingActorBoundSession(
        ZLinkServiceM6BWireCodec.ActorRouteFence actor,
        ZLinkServiceM6BWireCodec.SessionOwnerFence session) {
        // Alternate backends may wait for the ordinary binding announcement.
    }

    default Optional<BoundSessionRoute> boundSessionRoute(
        ZLinkBackendActorRef actor) {
        return Optional.empty();
    }

    void closeActorBoundSession(ZLinkBackendActorRef actor, Duration timeout);

    record BoundSessionRoute(
        RoutingId sessionOwnerNodeRid,
        long sessionOwnerNodeGeneration,
        RoutingId sessionRid,
        long bindingGeneration) {
    }

    @FunctionalInterface
    interface MessageFollowRelayHandler {
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

}
