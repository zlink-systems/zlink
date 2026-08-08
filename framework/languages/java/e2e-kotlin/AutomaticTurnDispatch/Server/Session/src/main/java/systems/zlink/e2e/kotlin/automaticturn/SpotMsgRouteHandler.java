package systems.zlink.e2e.kotlin.automaticturn;

import systems.zlink.contracts.core.RoutingId;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkTypedSessionPacketHandler;

abstract class SpotMsgRouteHandler<TCommand>
    implements ZLinkTypedSessionPacketHandler<ZLinkSessionContext, TCommand> {
    private final ZLinkRouteClient routes;
    private final Class<TCommand> messageType;

    SpotMsgRouteHandler(
        ZLinkRouteClient routes,
        Class<TCommand> messageType) {
        this.routes = routes;
        this.messageType = messageType;
    }

    @Override
    public Class<TCommand> messageType() {
        return messageType;
    }

    @Override
    public CompletionStage<Void> handle(
        ZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        TCommand command) {
        String targetSpotRid = targetSpot(dispatch);
        return routes.sendToSpot(targetSpotRid, command)
            .submit()
            .thenApply(ignored -> null);
    }

    static RoutingId targetNode(ZLinkSessionDispatchContext dispatch) {
        return RoutingId.from(dispatch.metadata()
            .getOrDefault(Contracts.TARGET_NODE_RID_METADATA, "play-a"));
    }

    static String targetSpot(ZLinkSessionDispatchContext dispatch) {
        return dispatch.metadata()
            .getOrDefault(Contracts.SPOT_RID_METADATA, "room-a");
    }
}
