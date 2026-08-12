package systems.zlink.e2e.spotservice.shared;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;

public final class NoopIngressHandler
    implements ZLinkRequestHandler<Contracts.StateReq, Contracts.StateRes> {
    private final ScenarioState state;

    public NoopIngressHandler(ScenarioState state) {
        this.state = state;
    }

    @Override
    public CompletionStage<Contracts.StateRes> handle(
        Contracts.StateReq request,
        ZLinkMessageContext context) {
        state.record("IngressReq", "channel", request.op());
        return CompletableFuture.completedFuture(
            new Contracts.StateRes("", state.nodeRid(), request.op()));
    }
}
