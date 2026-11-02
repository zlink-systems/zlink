package systems.zlink.framework.actors;

import systems.zlink.contracts.core.RoutingId;

import java.util.Objects;

/** Read-only identity of one Session binding presented to a duplicate-bind decision. */
public record ZLinkSessionBindingIdentity(
        RoutingId sessionOwnerNodeRid, long sessionOwnerNodeGeneration, long bindingGeneration) {
    public ZLinkSessionBindingIdentity {
        Objects.requireNonNull(sessionOwnerNodeRid, "sessionOwnerNodeRid");
        if (sessionOwnerNodeGeneration == 0 || bindingGeneration == 0) {
            throw new IllegalArgumentException("Session binding generations must be nonzero");
        }
    }
}
