package systems.zlink.framework.channels;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.ZLinkMessageContext;

public interface ZLinkRouteMessageContext extends ZLinkMessageContext {
    RoutingId sourceNodeRid();
}
