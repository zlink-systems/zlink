package systems.zlink.e2e.kotlin.automaticturn;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.channels.ZLinkRouteRequestContext;
import systems.zlink.framework.channels.ZLinkRouteRequestHandler;

public final class EvidenceRouteRequestHandler
    implements ZLinkRouteRequestHandler<Contracts.EvidenceReq, Contracts.EvidenceRes> {
    private final PlayEvidenceStore evidence;

    public EvidenceRouteRequestHandler(PlayEvidenceStore evidence) {
        this.evidence = evidence;
    }

    @Override
    public CompletionStage<Contracts.EvidenceRes> handle(
        Contracts.EvidenceReq request,
        ZLinkRouteRequestContext context) {
        return CompletableFuture.completedFuture(evidence.evidence(request.requestId()));
    }
}
