package systems.zlink.framework.runtime.internal.backend;

import java.util.List;
import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;

public interface ZLinkBackendStreamSocket
    extends ZLinkBackendSocket, ZLinkBackendReceiveSocket {
    void setTlsServer(String certificatePath, String keyPath, boolean requireClientCertificate);

    void setMaxMessageSize(long value);

    /** Enables Core STREAM connect/disconnect notifications before bind. */
    void enableNotifications();

    /** Receives one raw STREAM record, or {@code null} when no record is ready. */
    ZLinkBackendStreamReceived recv();

    void onTransportError(ZLinkBackendStreamErrorHandler handler);

    void startSessionService();

    boolean send(RoutingId routingId, List<Message> parts, SendFlags flags);

    boolean send(RoutingId routingId, String packetName, List<Message> parts, SendFlags flags);

    boolean send(RoutingId routingId, ZLinkStreamHeader header, List<Message> parts, SendFlags flags);

    boolean reply(
        RoutingId routingId,
        long requestSeq,
        String packetName,
        List<Message> parts,
        SendFlags flags);

    boolean reply(RoutingId routingId, ZLinkStreamHeader header, List<Message> parts, SendFlags flags);

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
}
