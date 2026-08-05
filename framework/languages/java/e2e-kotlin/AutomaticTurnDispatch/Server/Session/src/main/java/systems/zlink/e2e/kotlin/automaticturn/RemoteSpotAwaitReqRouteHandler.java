package systems.zlink.e2e.kotlin.automaticturn;

import java.time.Duration;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.spots.SpotHandleResolver;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkTypedSessionPacketHandler;

public final class RemoteSpotAwaitReqRouteHandler
    implements ZLinkTypedSessionPacketHandler<ZLinkSessionContext, Contracts.RemoteSpotAwaitReq> {
    private final ZLinkRouteClient routes;
    private final SpotHandleResolver spots;

    public RemoteSpotAwaitReqRouteHandler(ZLinkRouteClient routes, SpotHandleResolver spots) {
        this.routes = routes;
        this.spots = spots;
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
        var targetSpotRid = SpotMsgRouteHandler.targetSpot(dispatch);
        return spots.resolveSpotHandle(targetSpotRid)
            .thenCompose(handle -> routes.requestToSpot(
                handle.orElseThrow(() -> new IllegalStateException("spot not found: " + targetSpotRid)),
                request)
            .timeout(Duration.ofSeconds(10))
            .submit(Contracts.ScenarioRes.class))
            .thenAccept(reply -> context.client().reply(reply).submit());
    }
}
