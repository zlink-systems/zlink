package systems.zlink.e2e.kotlin.automaticturn;

import systems.zlink.framework.channels.ZLinkRouteClient;

public final class AwaitTimeoutMsgRouteHandler
    extends SpotMsgRouteHandler<Contracts.AwaitTimeoutMsg> {
    public AwaitTimeoutMsgRouteHandler(ZLinkRouteClient routes) {
        super(routes, Contracts.AwaitTimeoutMsg.class);
    }

}
