package systems.zlink.framework.runtime.internal.backend;

import java.util.List;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;

public record ZLinkBackendActorJoinResult(
    ZLinkBackendRequestResult result,
    int joinResultCode,
    ZLinkBackendActorRef actor,
    String joinedSpotId,
    long joinEpoch,
    int flags,
    List<Message> replyParts) {
}
