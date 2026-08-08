package systems.zlink.e2e.kotlin.automaticturn;

import java.util.concurrent.CompletionStage;
import java.util.concurrent.CompletableFuture;
import systems.zlink.framework.channels.ZLinkRouteMessageContext;
import systems.zlink.framework.channels.ZLinkRouteRequestHandler;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.ZLinkSpotManager;

public final class EnsureSpotRouteRequestHandler
    implements ZLinkRouteRequestHandler<Contracts.EnsureSpotReq, Contracts.EnsureSpotRes> {
    private static final int MAX_PLACEMENT_ATTEMPTS = 64;
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
        ZLinkRouteMessageContext context) {
        return ensureOnThisNode(request, 0);
    }

    private CompletionStage<Contracts.EnsureSpotRes> ensureOnThisNode(
        Contracts.EnsureSpotReq request,
        int attempt) {
        return spots.getOrCreate(request.spotRid(), "probe")
            .request(ZLinkMessage.of("ensure"))
            .submit()
            .thenCompose(created -> {
                String nodeRid = Env.get("nodeRid", "play-a");
                String actualNodeRid = created.spot().nodeRid().toString();
                if (!nodeRid.equals(actualNodeRid)) {
                    if (attempt + 1 >= MAX_PLACEMENT_ATTEMPTS) {
                        return CompletableFuture.failedFuture(new IllegalStateException(
                            "Spot placement did not reach route node " + nodeRid
                                + ": actual=" + actualNodeRid));
                    }
                    return spots.close(created.spot()).thenCompose(closed -> {
                        if (!closed) {
                            return CompletableFuture.failedFuture(new IllegalStateException(
                                "Spot placement cleanup was rejected: " + request.spotRid()));
                        }
                        return ensureOnThisNode(request, attempt + 1);
                    });
                }
                evidence.record(request.spotRid(), "spot-ensured", "node=" + nodeRid);
                return CompletableFuture.completedFuture(
                    new Contracts.EnsureSpotRes(request.spotRid(), nodeRid));
            });
    }
}
