package systems.zlink.e2e.kotlin.automaticturn;

import java.time.Duration;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkTypedSessionPacketHandler;

public final class EnsureSpotReqHandler
    implements ZLinkTypedSessionPacketHandler<ZLinkSessionContext, Contracts.EnsureSpotReq> {
    private final ZLinkRouteClient routes;

    public EnsureSpotReqHandler(ZLinkRouteClient routes) {
        this.routes = routes;
    }

    @Override
    public Class<Contracts.EnsureSpotReq> messageType() {
        return Contracts.EnsureSpotReq.class;
    }

    @Override
    public CompletionStage<Void> handle(
        ZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        Contracts.EnsureSpotReq request) {
        return routes.requestToNode(
                Contracts.SPOT_MESH,
                SpotMsgRouteHandler.targetNode(dispatch),
                request)
            .timeout(Duration.ofSeconds(30))
            .submit(Contracts.EnsureSpotRes.class)
            .thenAccept(reply -> context.client().reply(reply).submit());
    }
}
