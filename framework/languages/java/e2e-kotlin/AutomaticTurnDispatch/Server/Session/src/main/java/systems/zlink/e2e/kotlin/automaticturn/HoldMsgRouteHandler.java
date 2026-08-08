package systems.zlink.e2e.kotlin.automaticturn;

import systems.zlink.framework.channels.ZLinkRouteClient;

public final class HoldMsgRouteHandler
    extends SpotMsgRouteHandler<Contracts.HoldMsg> {
    public HoldMsgRouteHandler(ZLinkRouteClient routes) {
        super(routes, Contracts.HoldMsg.class);
    }

}
