package systems.zlink.framework.runtime.spots;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorReceived;

record ActorDispatchReply(Message message, boolean streamFrame) {
}

record ActorMessageRead(
    boolean complete,
    boolean fromPendingHeader,
    ZLinkBackendActorReceived headerPart,
    ZLinkBackendActorReceived bodyPart,
    ZLinkBackendActorReceived nextPendingHeader,
    int nextIndex) {
}

/**
 * Parsed inbound SPOT route packet. {@code header} is the decoded shared
 * cross-language envelope header when the message arrived as an envelope, or
 * {@code null} for legacy internal raw-parts packets.
 */
record ParsedPacket(
    String packetName,
    Message payload,
    systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.Header header) {

    ParsedPacket(String packetName, Message payload) {
        this(packetName, payload, null);
    }
}
