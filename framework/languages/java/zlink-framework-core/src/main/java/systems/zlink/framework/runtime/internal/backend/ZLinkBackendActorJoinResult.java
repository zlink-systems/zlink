package systems.zlink.framework.runtime.internal.backend;

import systems.zlink.contracts.messaging.Message;

import java.util.List;

public record ZLinkBackendActorJoinResult(
        ZLinkBackendRequestResult result,
        int joinResultCode,
        ZLinkBackendActorRef actor,
        String joinedSpotId,
        long joinEpoch,
        int flags,
        List<Message> replyParts) {}
