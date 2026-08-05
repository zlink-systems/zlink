package systems.zlink.e2e.kotlin.automaticturn;

import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.spots.SpotHandleResolver;

public final class ProbeMsgRouteHandler
    extends SpotMsgRouteHandler<Contracts.ProbeMsg> {
    public ProbeMsgRouteHandler(ZLinkRouteClient routes, SpotHandleResolver spots) {
        super(routes, spots, Contracts.ProbeMsg.class);
    }

}
