package systems.zlink.e2e.kotlin.automaticturn;

import java.time.Duration;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkTypedSessionPacketHandler;

public final class RemoteSpotAwaitReqRouteHandler
    implements ZLinkTypedSessionPacketHandler<ZLinkSessionContext, Contracts.RemoteSpotAwaitReq> {
    private final ZLinkRouteClient routes;

    public RemoteSpotAwaitReqRouteHandler(ZLinkRouteClient routes) {
        this.routes = routes;
    }

    @Override
    public Class<Contracts.RemoteSpotAwaitReq> messageType() {
        return Contracts.RemoteSpotAwaitReq.class;
    }

    @Override
    public CompletionStage<Void> handle(
        ZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        Contracts.RemoteSpotAwaitReq request) {
        var ownerSpotRid = SpotMsgRouteHandler.targetSpot(dispatch);
        return routes.requestToSpot(ownerSpotRid, request)
            .timeout(Duration.ofSeconds(10))
            .submit(Contracts.ScenarioRes.class)
            .thenAccept(reply -> context.client().reply(reply).submit());
    }
}
