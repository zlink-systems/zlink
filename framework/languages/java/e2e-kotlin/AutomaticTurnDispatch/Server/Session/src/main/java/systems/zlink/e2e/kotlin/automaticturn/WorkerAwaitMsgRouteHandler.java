package systems.zlink.e2e.kotlin.automaticturn;

import systems.zlink.framework.channels.ZLinkRouteClient;

public final class WorkerAwaitMsgRouteHandler
    extends SpotMsgRouteHandler<Contracts.WorkerAwaitMsg> {
    public WorkerAwaitMsgRouteHandler(ZLinkRouteClient routes) {
        super(routes, Contracts.WorkerAwaitMsg.class);
    }

}
