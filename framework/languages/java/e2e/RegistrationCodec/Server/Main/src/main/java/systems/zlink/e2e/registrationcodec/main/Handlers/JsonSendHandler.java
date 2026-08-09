package Handlers;
import systems.zlink.e2e.registrationcodec.main.Handlers;
import systems.zlink.e2e.registrationcodec.main.Infrastructure;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

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
    public CompletionStage<Void> handle(
        Contracts.JsonEchoMsg message,
        ZLinkMessageContext context) {
        state.record("Send", "JsonEcho", message.value());
        state.record("ContentType", "JsonEcho", context.contentType().orElse("missing"));
        return CompletableFuture.completedFuture(null);
    }
}
