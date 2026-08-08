package systems.zlink.e2e.kotlin.automaticturn;

import systems.zlink.framework.channels.ZLinkRouteClient;

public final class SpotProbeMsgRouteHandler
    extends SpotMsgRouteHandler<Contracts.SpotProbeMsg> {
    public SpotProbeMsgRouteHandler(ZLinkRouteClient routes) {
        super(routes, Contracts.SpotProbeMsg.class);
    }

}
