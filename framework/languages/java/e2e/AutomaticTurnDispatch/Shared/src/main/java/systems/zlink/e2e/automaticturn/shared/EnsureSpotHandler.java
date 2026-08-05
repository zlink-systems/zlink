package systems.zlink.e2e.automaticturn.shared;

import java.time.Duration;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.channels.ZLinkRouteMessageContext;
import systems.zlink.framework.channels.ZLinkRouteRequestHandler;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.ZLinkSpotManager;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkTypedSessionPacketHandler;

public final class EnsureSpotHandler
    implements ZLinkTypedSessionPacketHandler<ZLinkSessionContext, Contracts.EnsureSpotReq> {
    private final ZLinkRouteClient routes;

    public EnsureSpotHandler(ZLinkRouteClient routes) {
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
        RoutingId targetNodeRid = RoutingId.from(dispatch.metadata()
            .getOrDefault(Contracts.TARGET_NODE_RID_METADATA, Contracts.PLAY_NODE));
        return routes.requestToNode(
                Contracts.ROUTE_CHANNEL,
                targetNodeRid,
                request)
            .timeout(Duration.ofSeconds(30))
            .submit(Contracts.EnsureSpotRes.class)
            .thenAccept(reply -> context.client().reply(reply).submit());
    }

    public static final class Play
        implements ZLinkRouteRequestHandler<Contracts.EnsureSpotReq, Contracts.EnsureSpotRes> {
        private final ZLinkSpotManager spots;
        private final EvidenceStore evidence;

        public Play(
            ZLinkSpotManager spots,
            EvidenceStore evidence) {
            this.spots = spots;
            this.evidence = evidence;
        }

        @Override
        public CompletionStage<Contracts.EnsureSpotRes> handle(
            Contracts.EnsureSpotReq request,
            ZLinkRouteMessageContext context) {
            return spots.getOrCreate(
                    AwaitProbeSpot.class,
                    RoutingId.from(request.spotRid()),
                    ZLinkMessage.of("ensure"))
                .thenApply(created -> {
                    evidence.record("spot-ensured", request.spotRid(), "node=" + evidence.nodeRid());
                    return new Contracts.EnsureSpotRes(request.spotRid(), evidence.nodeRid());
                });
        }
    }
}
