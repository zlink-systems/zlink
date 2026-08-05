package systems.zlink.framework.runtime.internal.backend;

import java.util.List;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;

public record ZLinkBackendActorJoinEntrySpotResult(
    ZLinkBackendRequestResult result,
    int joinResultCode,
    ZLinkBackendActorRef actor,
    RoutingId targetNodeRid,
    String joinedSpotId,
    long joinEpoch,
    int flags,
    List<Message> replyParts) {
}
