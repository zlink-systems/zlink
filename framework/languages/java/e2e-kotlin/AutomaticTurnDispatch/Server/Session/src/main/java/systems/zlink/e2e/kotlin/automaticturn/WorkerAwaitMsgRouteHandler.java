package systems.zlink.e2e.kotlin.automaticturn;

import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.spots.SpotHandleResolver;

public final class WorkerAwaitMsgRouteHandler
    extends SpotMsgRouteHandler<Contracts.WorkerAwaitMsg> {
    public WorkerAwaitMsgRouteHandler(ZLinkRouteClient routes, SpotHandleResolver spots) {
        super(routes, spots, Contracts.WorkerAwaitMsg.class);
    }

}
