package systems.zlink.framework.runtime.internal.backend;

import java.util.List;
import systems.zlink.contracts.messaging.Message;

public record ZLinkBackendActorJoinRequest(
    ZLinkBackendActorRef sourceActor,
    ZLinkBackendActorRef targetActor,
    List<Message> parts,
    Object nativeRequest) {
}
