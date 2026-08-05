package systems.zlink.e2e.registrationcodec.main.Handlers;

import systems.zlink.e2e.registrationcodec.shared.Contracts;
import systems.zlink.e2e.registrationcodec.main.Infrastructure.EvidenceStore;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkSendHandler;

public final class JsonSendHandler
    implements ZLinkSendHandler<Contracts.JsonEchoMsg> {
    private final EvidenceStore state;

    public JsonSendHandler(EvidenceStore state) {
        this.state = state;
    }

    @Override
    public java.util.concurrent.CompletionStage<Void> handle(
        Contracts.JsonEchoMsg message,
        ZLinkMessageContext context) {
        state.record("Send", "JsonEcho", message.value());
        state.record("ContentType", "JsonEcho", context.contentType().orElse("missing"));
        return java.util.concurrent.CompletableFuture.completedFuture(null);
    }
}
