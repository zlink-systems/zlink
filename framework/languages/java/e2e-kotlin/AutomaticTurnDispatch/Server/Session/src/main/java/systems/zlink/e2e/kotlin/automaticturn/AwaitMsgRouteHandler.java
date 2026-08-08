package systems.zlink.e2e.kotlin.automaticturn;

import systems.zlink.framework.channels.ZLinkRouteClient;

public final class AwaitMsgRouteHandler
    extends SpotMsgRouteHandler<Contracts.AwaitMsg> {
    public AwaitMsgRouteHandler(ZLinkRouteClient routes) {
        super(routes, Contracts.AwaitMsg.class);
    }

}
