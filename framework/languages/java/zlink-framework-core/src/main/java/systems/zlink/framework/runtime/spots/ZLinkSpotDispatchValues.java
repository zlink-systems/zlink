package systems.zlink.framework.runtime.spots;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorReceived;

record ActorDispatchReply(Message message, boolean streamFrame) {}

record ActorMessageRead(
        boolean complete,
        boolean fromPendingHeader,
        ZLinkBackendActorReceived headerPart,
        ZLinkBackendActorReceived bodyPart,
        ZLinkBackendActorReceived nextPendingHeader,
        int nextIndex) {}

/** Parsed inbound SPOT route packet and its ChannelHeader. */
record ParsedPacket(
        String packetName,
        Message payload,
        systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.Header header) {}
