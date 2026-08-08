package systems.zlink.e2e.kotlin.automaticturn;

import systems.zlink.framework.channels.ZLinkRouteClient;

public final class TimerStopMsgRouteHandler
    extends SpotMsgRouteHandler<Contracts.TimerStopMsg> {
    public TimerStopMsgRouteHandler(ZLinkRouteClient routes) {
        super(routes, Contracts.TimerStopMsg.class);
    }

}
