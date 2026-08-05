package systems.zlink.e2e.kotlin.automaticturn;

import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.spots.SpotHandleResolver;

public final class TimerStopMsgRouteHandler
    extends SpotMsgRouteHandler<Contracts.TimerStopMsg> {
    public TimerStopMsgRouteHandler(ZLinkRouteClient routes, SpotHandleResolver spots) {
        super(routes, spots, Contracts.TimerStopMsg.class);
    }

}
