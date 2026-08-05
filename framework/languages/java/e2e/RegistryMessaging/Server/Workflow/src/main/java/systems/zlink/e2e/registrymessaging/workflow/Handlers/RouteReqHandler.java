package systems.zlink.e2e.registrymessaging.workflow.Handlers;

import systems.zlink.e2e.registrymessaging.workflow.Infrastructure.ScenarioState;
import systems.zlink.e2e.registrymessaging.shared.Contracts;
import systems.zlink.framework.channels.ZLinkRouteMessageContext;
import systems.zlink.framework.channels.ZLinkRouteRequestHandler;

public final class RouteReqHandler
    implements ZLinkRouteRequestHandler<Contracts.RouteReq, Contracts.RouteRes> {
    private final ScenarioState state;

    public RouteReqHandler(ScenarioState state) {
        this.state = state;
    }

    @Override
    public java.util.concurrent.CompletionStage<Contracts.RouteRes> handle(
        Contracts.RouteReq request,
        ZLinkRouteMessageContext context) {
        state.record("ScenarioRouteReq", request.value());
        return java.util.concurrent.CompletableFuture.completedFuture(new Contracts.RouteRes(
            "route:" + request.value(),
            state.providerRid(),
            context.sourceNodeRid().toString()));
    }
}
