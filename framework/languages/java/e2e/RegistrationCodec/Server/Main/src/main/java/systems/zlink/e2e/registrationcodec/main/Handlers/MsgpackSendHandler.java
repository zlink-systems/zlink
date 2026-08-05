package systems.zlink.e2e.registrationcodec.main.Handlers;

import systems.zlink.e2e.registrationcodec.shared.Contracts;
import systems.zlink.e2e.registrationcodec.main.Infrastructure.EvidenceStore;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkSendHandler;

public final class MsgpackSendHandler
    implements ZLinkSendHandler<Contracts.PackedEchoMsg> {
    private final EvidenceStore state;

    public MsgpackSendHandler(EvidenceStore state) {
        this.state = state;
    }

    @Override
    public java.util.concurrent.CompletionStage<Void> handle(
        Contracts.PackedEchoMsg message,
        ZLinkMessageContext context) {
        state.record("Send", "MsgpackEcho", message.value());
        state.record("ContentType", "MsgpackEcho", context.contentType().orElse(""));
        return java.util.concurrent.CompletableFuture.completedFuture(null);
    }
}
