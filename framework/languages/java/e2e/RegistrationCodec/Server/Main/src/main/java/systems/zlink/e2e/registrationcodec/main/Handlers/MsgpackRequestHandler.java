package systems.zlink.e2e.registrationcodec.main.Handlers;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

import systems.zlink.e2e.registrationcodec.shared.Contracts;
import systems.zlink.e2e.registrationcodec.main.Infrastructure.EvidenceStore;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;

public final class MsgpackRequestHandler
    implements ZLinkRequestHandler<Contracts.PackedEchoReq, Contracts.PackedEchoRes> {
    private final EvidenceStore state;

    public MsgpackRequestHandler(EvidenceStore state) {
        this.state = state;
    }

    @Override
    public CompletionStage<Contracts.PackedEchoRes> handle(
        Contracts.PackedEchoReq request,
        ZLinkMessageContext context) {
        state.record("Request", "PackedEchoReq", request.value());
        state.record("ContentType", "PackedEchoReq", context.contentType().orElse(""));
        return CompletableFuture.completedFuture(
            new Contracts.PackedEchoRes("echo:" + request.value()));
    }
}
