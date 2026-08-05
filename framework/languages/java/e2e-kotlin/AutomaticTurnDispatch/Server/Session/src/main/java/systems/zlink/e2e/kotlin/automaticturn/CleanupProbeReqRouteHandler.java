package systems.zlink.e2e.kotlin.automaticturn;

import java.time.Duration;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.spots.SpotHandleResolver;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkTypedSessionPacketHandler;

public final class CleanupProbeReqRouteHandler
    implements ZLinkTypedSessionPacketHandler<ZLinkSessionContext, Contracts.CleanupProbeReq> {
    private final ZLinkRouteClient routes;
    private final SpotHandleResolver spots;

    public CleanupProbeReqRouteHandler(ZLinkRouteClient routes, SpotHandleResolver spots) {
        this.routes = routes;
        this.spots = spots;
    }

    @Override
    public Class<Contracts.CleanupProbeReq> messageType() {
        return Contracts.CleanupProbeReq.class;
    }

    @Override
    public CompletionStage<Void> handle(
        ZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        Contracts.CleanupProbeReq request) {
        var targetSpotRid = SpotMsgRouteHandler.targetSpot(dispatch);
        return spots.resolveSpotHandle(targetSpotRid)
            .thenCompose(handle -> routes.requestToSpot(
                    handle.orElseThrow(() ->
                        new IllegalStateException("spot not found: " + targetSpotRid)),
                    request)
                .timeout(Duration.ofSeconds(15))
                .submit(Contracts.CleanupProbeRes.class))
            .thenAccept(reply -> context.client().reply(reply).submit());
    }
}
