package systems.zlink.framework.runtime.actors;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;

/** Caller coordinates required for a target node to reply without returning through the source. */
public record ZLinkActorReplyRoute(
    ZLinkBackendActorRef actorRef,
    RoutingId sourceNodeRid,
    RoutingId sourceSessionRid,
    long requestId,
    int flags) {
}
