package systems.zlink.e2e.kotlin.automaticturn;

import systems.zlink.framework.channels.ZLinkRouteClient;

public final class ProbeMsgRouteHandler
    extends SpotMsgRouteHandler<Contracts.ProbeMsg> {
    public ProbeMsgRouteHandler(ZLinkRouteClient routes) {
        super(routes, Contracts.ProbeMsg.class);
    }

}
