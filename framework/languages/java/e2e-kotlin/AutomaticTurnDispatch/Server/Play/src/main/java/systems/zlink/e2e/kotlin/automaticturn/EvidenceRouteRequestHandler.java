package systems.zlink.e2e.kotlin.automaticturn;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.channels.ZLinkRouteMessageContext;
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
        ZLinkRouteMessageContext context) {
        return CompletableFuture.completedFuture(evidence.evidence(request.requestId()));
    }
}

