package systems.zlink.e2e.registrationcodec.main.Handlers;

import systems.zlink.e2e.registrationcodec.shared.Contracts;
import systems.zlink.e2e.registrationcodec.main.Infrastructure.EvidenceStore;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;

@ZLinkHandlerGroup(Contracts.AUTO_GROUP)
public final class AutoRequestHandler
    implements ZLinkRequestHandler<Contracts.EchoAutoReq, Contracts.EchoRes> {
    private final EvidenceStore state;

    public AutoRequestHandler(EvidenceStore state) {
        this.state = state;
    }

    @Override
    public java.util.concurrent.CompletionStage<Contracts.EchoRes> handle(
        Contracts.EchoAutoReq request,
        ZLinkMessageContext context) {
        state.record("Request", "EchoAuto", request.value());
        return java.util.concurrent.CompletableFuture.completedFuture(
            new Contracts.EchoRes("echo:" + request.value(), "auto"));
    }
}
