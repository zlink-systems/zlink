package systems.zlink.e2e.kotlin.automaticturn;

import systems.zlink.framework.channels.ZLinkRouteClient;

public final class AwaitCancelMsgRouteHandler
    extends SpotMsgRouteHandler<Contracts.AwaitCancelMsg> {
    public AwaitCancelMsgRouteHandler(ZLinkRouteClient routes) {
        super(routes, Contracts.AwaitCancelMsg.class);
    }

}
