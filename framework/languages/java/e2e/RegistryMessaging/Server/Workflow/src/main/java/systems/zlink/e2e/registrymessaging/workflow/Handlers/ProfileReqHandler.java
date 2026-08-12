package systems.zlink.e2e.registrymessaging.workflow.Handlers;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

import systems.zlink.e2e.registrymessaging.workflow.Infrastructure.ScenarioState;
import systems.zlink.e2e.registrymessaging.shared.Contracts;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;

@ZLinkHandlerGroup(Contracts.HANDLER_GROUP)
public final class ProfileReqHandler
    implements ZLinkRequestHandler<Contracts.ProfileReq, Contracts.ProfileRes> {
    private final ScenarioState state;

    public ProfileReqHandler(ScenarioState state) {
        this.state = state;
    }

    @Override
    public CompletionStage<Contracts.ProfileRes> handle(
        Contracts.ProfileReq request,
        ZLinkMessageContext context) {
        if (request.value().startsWith("slow")) {
            try {
                Thread.sleep(1000);
            } catch (InterruptedException error) {
                Thread.currentThread().interrupt();
                throw new IllegalStateException("interrupted", error);
            }
        }
        state.record("ProfileReq", request.value());
        return CompletableFuture.completedFuture(new Contracts.ProfileRes(
            "profile:" + request.value(),
            state.providerRid(),
            state.instanceId()));
    }
}
