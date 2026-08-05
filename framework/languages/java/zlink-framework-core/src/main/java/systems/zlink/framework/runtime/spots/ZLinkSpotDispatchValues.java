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

record ParsedPacket(String packetName, Message payload) {
}
