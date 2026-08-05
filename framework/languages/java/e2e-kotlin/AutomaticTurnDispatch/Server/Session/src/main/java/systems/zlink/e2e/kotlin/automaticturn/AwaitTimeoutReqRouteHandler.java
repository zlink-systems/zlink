package systems.zlink.e2e.kotlin.automaticturn;

import java.time.Duration;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.spots.SpotHandleResolver;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkTypedSessionPacketHandler;

public final class AwaitTimeoutReqRouteHandler
    implements ZLinkTypedSessionPacketHandler<ZLinkSessionContext, Contracts.AwaitTimeoutReq> {
    private final ZLinkRouteClient routes;
    private final SpotHandleResolver spots;

    public AwaitTimeoutReqRouteHandler(ZLinkRouteClient routes, SpotHandleResolver spots) {
        this.routes = routes;
        this.spots = spots;
    }

    @Override
    public Class<Contracts.AwaitTimeoutReq> messageType() {
        return Contracts.AwaitTimeoutReq.class;
    }

    @Override
    public CompletionStage<Void> handle(
        ZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        Contracts.AwaitTimeoutReq request) {
        var targetSpotRid = SpotMsgRouteHandler.targetSpot(dispatch);
        return spots.resolveSpotHandle(targetSpotRid)
            .thenCompose(handle -> routes.requestToSpot(
                handle.orElseThrow(() -> new IllegalStateException("spot not found: " + targetSpotRid)),
                request)
            .timeout(Duration.ofSeconds(15))
            .submit(Contracts.AwaitTimeoutRes.class))
            .thenAccept(reply -> context.client().reply(reply).submit());
    }
}
