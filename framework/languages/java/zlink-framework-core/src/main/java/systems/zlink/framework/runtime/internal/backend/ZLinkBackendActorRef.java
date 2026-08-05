package systems.zlink.framework.runtime.internal.backend;

import systems.zlink.contracts.core.RoutingId;

public record ZLinkBackendActorRef(RoutingId nodeRid, String actorId, long generation) {
}
