package systems.zlink.e2e.kotlin.automaticturn;

import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.spots.SpotHandleResolver;

public final class AwaitTimeoutMsgRouteHandler
    extends SpotMsgRouteHandler<Contracts.AwaitTimeoutMsg> {
    public AwaitTimeoutMsgRouteHandler(ZLinkRouteClient routes, SpotHandleResolver spots) {
        super(routes, spots, Contracts.AwaitTimeoutMsg.class);
    }

}
