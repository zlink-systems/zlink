package systems.zlink.e2e.kotlin.automaticturn;

import java.time.Duration;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.spots.SpotHandleResolver;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkTypedSessionPacketHandler;

public final class ProbeReqRouteHandler
    implements ZLinkTypedSessionPacketHandler<ZLinkSessionContext, Contracts.ProbeReq> {
    private final ZLinkRouteClient routes;
    private final SpotHandleResolver spots;

    public ProbeReqRouteHandler(ZLinkRouteClient routes, SpotHandleResolver spots) {
        this.routes = routes;
        this.spots = spots;
    }

    @Override
    public Class<Contracts.ProbeReq> messageType() {
        return Contracts.ProbeReq.class;
    }

    @Override
    public CompletionStage<Void> handle(
        ZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        Contracts.ProbeReq request) {
        var targetSpotRid = SpotMsgRouteHandler.targetSpot(dispatch);
        return spots.resolveSpotHandle(targetSpotRid)
            .thenCompose(handle -> routes.requestToSpot(
                handle.orElseThrow(() -> new IllegalStateException("spot not found: " + targetSpotRid)),
                request)
            .timeout(Duration.ofSeconds(15))
            .submit(Contracts.ProbeRes.class))
            .thenAccept(reply -> context.client().reply(reply).submit());
    }
}
