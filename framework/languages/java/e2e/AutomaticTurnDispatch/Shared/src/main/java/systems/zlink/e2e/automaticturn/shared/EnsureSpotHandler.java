package systems.zlink.e2e.automaticturn.shared;
import java.util.concurrent.CompletableFuture;

import java.time.Duration;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.spots.ZLinkSpotCreateResult;
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
    private static final int MAX_PLACEMENT_ATTEMPTS = 64;
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
            return ensureOnThisNode(request, 0);
        }

        private CompletionStage<Contracts.EnsureSpotRes> ensureOnThisNode(
            Contracts.EnsureSpotReq request,
            int attempt) {
            return spots.getOrCreate(request.spotRid(), Contracts.TARGET_SPOT)
                .request(ZLinkMessage.of("ensure"))
                .submit()
                .thenCompose(created -> {
                    String actualNode = created.spot().nodeRid().toString();
                    if (evidence.nodeRid().equals(actualNode)) {
                        evidence.record("spot-ensured", request.spotRid(), "node=" + actualNode);
                        return CompletableFuture.completedFuture(
                            new Contracts.EnsureSpotRes(request.spotRid(), actualNode));
                    }
                    if (attempt + 1 >= MAX_PLACEMENT_ATTEMPTS) {
                        return CompletableFuture.failedFuture(
                            new IllegalStateException(
                                "Spot placement did not reach route node " + evidence.nodeRid()
                                    + ": actual=" + actualNode));
                    }
                    return spots.close(created.spot()).thenCompose(closed -> {
                        if (!closed) {
                            return CompletableFuture.failedFuture(
                                new IllegalStateException(
                                    "Spot placement cleanup was rejected: "
                                        + request.spotRid()));
                        }
                        return ensureOnThisNode(request, attempt + 1);
                    });
                });
        }
    }
}
