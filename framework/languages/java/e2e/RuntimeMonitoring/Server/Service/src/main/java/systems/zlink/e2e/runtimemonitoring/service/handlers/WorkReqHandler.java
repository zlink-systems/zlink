package systems.zlink.e2e.runtimemonitoring.service.handlers;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

import systems.zlink.e2e.runtimemonitoring.service.support.EvidenceState;
import systems.zlink.e2e.runtimemonitoring.shared.Contracts;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;

@ZLinkHandlerGroup(Contracts.HANDLER_GROUP)
public final class WorkReqHandler
    implements ZLinkRequestHandler<Contracts.WorkReq, Contracts.WorkRes> {
    private final EvidenceState evidence;
    private final String routingId;

    public WorkReqHandler(EvidenceState evidence) {
        this.evidence = evidence;
        this.routingId = evidence.rid();
    }

    @Override
    public CompletionStage<Contracts.WorkRes> handle(
        Contracts.WorkReq request,
        ZLinkMessageContext context) {
        evidence.record("work", evidence.rid(), "WorkReq", request.value());
        return CompletableFuture.completedFuture(new Contracts.WorkRes(
            "work:" + request.value(),
            routingId));
    }
}
