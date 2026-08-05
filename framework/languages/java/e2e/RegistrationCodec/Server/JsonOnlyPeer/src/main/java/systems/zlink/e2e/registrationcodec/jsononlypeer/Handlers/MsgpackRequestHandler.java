package systems.zlink.e2e.registrationcodec.jsononlypeer.Handlers;

import systems.zlink.e2e.registrationcodec.shared.Contracts;
import systems.zlink.e2e.registrationcodec.jsononlypeer.Infrastructure.EvidenceStore;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;

public final class MsgpackRequestHandler
    implements ZLinkRequestHandler<Contracts.PackedEchoReq, Contracts.PackedEchoRes> {
    private final EvidenceStore state;

    public MsgpackRequestHandler(EvidenceStore state) {
        this.state = state;
    }

    @Override
    public java.util.concurrent.CompletionStage<Contracts.PackedEchoRes> handle(
        Contracts.PackedEchoReq request,
        ZLinkMessageContext context) {
        state.record("Request", "MsgpackEcho", request.value());
        state.record("ContentType", "MsgpackEcho", context.contentType().orElse(""));
        return java.util.concurrent.CompletableFuture.completedFuture(
            new Contracts.PackedEchoRes("echo:" + request.value()));
    }
}
