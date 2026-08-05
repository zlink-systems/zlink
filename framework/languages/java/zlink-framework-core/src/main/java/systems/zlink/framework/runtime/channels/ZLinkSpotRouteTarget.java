package systems.zlink.framework.runtime.channels;

import java.util.Objects;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;

interface ZLinkSpotRouteTarget {
}

record ZLinkRouteBridgeTarget() implements ZLinkSpotRouteTarget {
}

record ZLinkSpotRouterNodeTarget(ZLinkInternalSpotNode node) implements ZLinkSpotRouteTarget {
    ZLinkSpotRouterNodeTarget {
        Objects.requireNonNull(node, "node");
    }
}
