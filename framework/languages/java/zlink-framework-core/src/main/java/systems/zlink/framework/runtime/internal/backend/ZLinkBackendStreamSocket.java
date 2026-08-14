package systems.zlink.framework.runtime.internal.backend;

import java.util.List;
import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;

public interface ZLinkBackendStreamSocket
    extends ZLinkBackendSocket, ZLinkBackendReceiveSocket {
    /** Returns the actual endpoint selected by the most recent bind. */
    default String lastEndpoint() {
        return "";
    }

    void setTlsServer(String certificatePath, String keyPath, boolean requireClientCertificate);

    void setMaxMessageSize(long value);

    /** Enables Core STREAM connect/disconnect notifications before bind. */
    void enableNotifications();

    /** Receives one raw STREAM record, or {@code null} when no record is ready. */
    ZLinkBackendStreamReceived recv();

    default void disconnectPeer(RoutingId routingId) {
        // Alternate test backends may not own a native STREAM transport.
    }

    /**
     * Allocates the next ingress sequence owned by this physical Session.
     * A zero result means that an alternate backend leaves allocation to the
     * Framework Session ingress gate.
     */
    default long allocateBoundSessionIngressSequence() {
        return 0;
    }

    /** Returns the exact generation of the current bound-Actor route. */
    default long boundActorBindingGeneration(
        RoutingId sessionRid,
        String actorId) {
        return 0;
    }

    void onTransportError(ZLinkBackendStreamErrorHandler handler);

    void startSessionService();

    boolean send(RoutingId routingId, List<Message> parts, SendFlags flags);

    /** Delivers an Actor-to-Session frame without adding another STREAM header. */
    default boolean sendBoundSessionPush(
        RoutingId routingId,
        List<Message> parts,
        SendFlags flags) {
        return send(routingId, parts, flags);
    }

    /**
     * Completes when one already-framed Actor-to-Session record is physically
     * accepted. Native backends own any backpressure wait.
     */
    default CompletionStage<Void> sendBoundSessionPushAsync(
        RoutingId routingId,
        List<Message> parts) {
        try {
            return sendBoundSessionPush(
                    routingId, parts, SendFlags.DONT_WAIT)
                ? CompletableFuture.completedFuture(null)
                : CompletableFuture.failedFuture(
                    new ZlinkSubmitException(SubmitResult.BACKPRESSURED));
        } catch (RuntimeException failure) {
            return CompletableFuture.failedFuture(failure);
        }
    }

    boolean send(RoutingId routingId, String packetName, List<Message> parts, SendFlags flags);

    boolean send(RoutingId routingId, ZLinkStreamHeader header, List<Message> parts, SendFlags flags);

    /**
     * Completes when the backend has accepted one physical STREAM send.
     * Implementations backed by a native binding own any admission wait.
     */
    default CompletionStage<Void> sendAsync(
        RoutingId routingId,
        ZLinkStreamHeader header,
        List<Message> parts) {
        try {
            return send(routingId, header, parts, SendFlags.DONT_WAIT)
                ? CompletableFuture.completedFuture(null)
                : CompletableFuture.failedFuture(
                    new ZlinkSubmitException(SubmitResult.BACKPRESSURED));
        } catch (RuntimeException failure) {
            return CompletableFuture.failedFuture(failure);
        }
    }

    default CompletionStage<Void> sendAsync(
        RoutingId routingId,
        ZLinkStreamHeader header,
        List<Message> parts,
        Duration timeout) {
        return sendAsync(routingId, header, parts);
    }

    boolean reply(
        RoutingId routingId,
        long requestSeq,
        String packetName,
        List<Message> parts,
        SendFlags flags);

    boolean reply(RoutingId routingId, ZLinkStreamHeader header, List<Message> parts, SendFlags flags);

    /**
     * Completes when the backend has accepted one physical STREAM reply.
     * Implementations backed by a native binding own any admission wait.
     */
    default CompletionStage<Void> replyAsync(
        RoutingId routingId,
        ZLinkStreamHeader header,
        List<Message> parts) {
        try {
            return reply(routingId, header, parts, SendFlags.DONT_WAIT)
                ? CompletableFuture.completedFuture(null)
                : CompletableFuture.failedFuture(
                    new ZlinkSubmitException(SubmitResult.BACKPRESSURED));
        } catch (RuntimeException failure) {
            return CompletableFuture.failedFuture(failure);
        }
    }

    ZLinkBackendActorBindOperation bindActor(RoutingId sessionRid, ZLinkBackendActorRef actor);

    ZLinkBackendActorUnbindOperation unbindActor(RoutingId sessionRid, String actorId);

    boolean sendBoundActor(RoutingId sessionRid, String actorId, List<Message> parts, SendFlags flags);

    boolean relayBoundActor(
        RoutingId sessionRid,
        String actorId,
        ZLinkStreamHeader header,
        List<Message> parts,
        SendFlags flags);

    /**
     * Completes when the backend has accepted one physical bound-Actor relay.
     * Logical route lookup and protocol retransmission remain caller-owned.
     */
    default CompletionStage<Void> relayBoundActorAsync(
        RoutingId sessionRid,
        String actorId,
        ZLinkStreamHeader header,
        List<Message> parts) {
        try {
            return relayBoundActor(
                    sessionRid,
                    actorId,
                    header,
                    parts,
                    SendFlags.DONT_WAIT)
                ? CompletableFuture.completedFuture(null)
                : CompletableFuture.failedFuture(
                    new ZlinkSubmitException(SubmitResult.BACKPRESSURED));
        } catch (RuntimeException failure) {
            return CompletableFuture.failedFuture(failure);
        }
    }

    /** Relays one admitted ingress record without reallocating its sequence. */
    default CompletionStage<Void> relayBoundActorAsync(
        RoutingId sessionRid,
        String actorId,
        long sourceSessionSequence,
        ZLinkStreamHeader header,
        List<Message> parts) {
        try {
            return relayBoundActor(
                    sessionRid,
                    actorId,
                    sourceSessionSequence,
                    header,
                    parts,
                    SendFlags.DONT_WAIT)
                ? CompletableFuture.completedFuture(null)
                : CompletableFuture.failedFuture(
                    new ZlinkSubmitException(SubmitResult.BACKPRESSURED));
        } catch (RuntimeException failure) {
            return CompletableFuture.failedFuture(failure);
        }
    }

    /** Relays ingress with the sequence already accepted by the owner gate. */
    default boolean relayBoundActor(
        RoutingId sessionRid,
        String actorId,
        long sourceSessionSequence,
        ZLinkStreamHeader header,
        List<Message> parts,
        SendFlags flags) {
        return relayBoundActor(sessionRid, actorId, header, parts, flags);
    }

    /**
     * Changes the physical Actor route without creating a new logical Session
     * binding generation.
     */
    default CompletionStage<Void> relocateBoundActor(
        RoutingId sessionRid,
        String actorId,
        long bindingGeneration,
        ZLinkBackendActorRef targetActor,
        Duration timeout) {
        return unbindActor(sessionRid, actorId).submit(timeout)
            .thenCompose(ignored -> bindActor(sessionRid, targetActor)
                .submit(timeout));
    }

    /**
     * Completes after an internal bound-Actor request has been handled by the
     * target Framework runtime. This is not an application-facing API.
     */
    default CompletionStage<List<Message>> requestBoundActor(
        RoutingId sessionRid,
        String actorId,
        ZLinkStreamHeader header,
        List<Message> parts,
        Duration timeout) {
        return CompletableFuture.failedFuture(
            new UnsupportedOperationException(
                "bound Actor request is unavailable"));
    }

    /** Sends an internal request to the supplied exact Actor identity. */
    default CompletionStage<List<Message>> requestExactActor(
        ZLinkBackendActorRef actor,
        ZLinkStreamHeader header,
        List<Message> parts,
        Duration timeout) {
        return CompletableFuture.failedFuture(
            new UnsupportedOperationException(
                "exact Actor request is unavailable"));
    }
}
