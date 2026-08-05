package systems.zlink.e2e.kotlin.automaticturn;

import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.spots.SpotHandleResolver;

public final class SpotProbeMsgRouteHandler
    extends SpotMsgRouteHandler<Contracts.SpotProbeMsg> {
    public SpotProbeMsgRouteHandler(ZLinkRouteClient routes, SpotHandleResolver spots) {
        super(routes, spots, Contracts.SpotProbeMsg.class);
    }

}
