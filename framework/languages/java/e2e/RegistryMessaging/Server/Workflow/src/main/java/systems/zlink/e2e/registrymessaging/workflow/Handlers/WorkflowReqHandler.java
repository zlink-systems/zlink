package systems.zlink.e2e.registrymessaging.workflow.Handlers;

import systems.zlink.e2e.registrymessaging.shared.Contracts;
import systems.zlink.e2e.registrymessaging.workflow.Infrastructure.ScenarioState;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;

@ZLinkHandlerGroup(Contracts.HANDLER_GROUP)
public final class WorkflowReqHandler
    implements ZLinkRequestHandler<Contracts.WorkflowReq, Contracts.WorkflowRes> {
    private final ScenarioState state;

    public WorkflowReqHandler(ScenarioState state) {
        this.state = state;
    }

    @Override
    public java.util.concurrent.CompletionStage<Contracts.WorkflowRes> handle(
        Contracts.WorkflowReq request,
        ZLinkMessageContext context) {
        state.record("workflow-request", request.value());
        return java.util.concurrent.CompletableFuture.completedFuture(
            new Contracts.WorkflowRes("workflow:" + request.value(), state.providerRid()));
    }
}
