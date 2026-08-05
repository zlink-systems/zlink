package systems.zlink.e2e.kotlin.automaticturn;

import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.channels.ZLinkRouteRequestContext;
import systems.zlink.framework.channels.ZLinkRouteRequestHandler;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.ZLinkSpotManager;

public final class EnsureSpotRouteRequestHandler
    implements ZLinkRouteRequestHandler<Contracts.EnsureSpotReq, Contracts.EnsureSpotRes> {
    private final ZLinkSpotManager spots;
    private final PlayEvidenceStore evidence;

    public EnsureSpotRouteRequestHandler(
        ZLinkSpotManager spots,
        PlayEvidenceStore evidence) {
        this.spots = spots;
        this.evidence = evidence;
    }

    @Override
    public CompletionStage<Contracts.EnsureSpotRes> handle(
        Contracts.EnsureSpotReq request,
        ZLinkRouteRequestContext context) {
        return spots.getOrCreate(
                ProbeSpot.class,
                RoutingId.from(request.spotRid()),
                ZLinkMessage.of("ensure"))
            .thenApply(ignored -> {
                String nodeRid = Env.get("nodeRid", "play-a");
                evidence.record(request.spotRid(), "spot-ensured", "node=" + nodeRid);
                return new Contracts.EnsureSpotRes(request.spotRid(), nodeRid);
            });
    }
}
