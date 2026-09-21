package systems.zlink.framework.runtime.internal.backend;

import systems.zlink.contracts.messaging.Message;

import java.util.List;

public record ZLinkBackendActorJoinRequest(
        ZLinkBackendActorRef sourceActor,
        ZLinkBackendActorRef targetActor,
        List<Message> parts,
        Object nativeRequest) {}
