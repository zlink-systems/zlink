package systems.zlink.e2e.registrymessaging.provider.Handlers;

import systems.zlink.e2e.registrymessaging.provider.Infrastructure.ScenarioState;
import systems.zlink.e2e.registrymessaging.provider.Infrastructure.ProfileGate;
import systems.zlink.e2e.registrymessaging.shared.Contracts;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;

@ZLinkHandlerGroup(Contracts.HANDLER_GROUP)
public final class ProfileReqHandler
    implements ZLinkRequestHandler<Contracts.ProfileReq, Contracts.ProfileRes> {
    private final ScenarioState state;
    private final ProfileGate gate;

    public ProfileReqHandler(ScenarioState state, ProfileGate gate) {
        this.state = state;
        this.gate = gate;
    }

    @Override
    public java.util.concurrent.CompletionStage<Contracts.ProfileRes> handle(
        Contracts.ProfileReq request,
        ZLinkMessageContext context) {
        if (request.value().startsWith("rm-b3-inflight-")) {
            state.record("ProfileReqStarted", request.value());
            gate.await();
        } else if (request.value().startsWith("slow")) {
            try {
                Thread.sleep(1000);
            } catch (InterruptedException error) {
                Thread.currentThread().interrupt();
                throw new IllegalStateException("interrupted", error);
            }
        }
        state.record("ProfileReq", request.value());
        return java.util.concurrent.CompletableFuture.completedFuture(new Contracts.ProfileRes(
            "profile:" + request.value(),
            state.providerRid(),
            state.instanceId()));
    }
}
