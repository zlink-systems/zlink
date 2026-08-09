package Handlers;
import systems.zlink.e2e.registrationcodec.main.Handlers;
import systems.zlink.e2e.registrationcodec.main.Infrastructure;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

import com.google.protobuf.StringValue;
import systems.zlink.e2e.registrationcodec.main.Infrastructure.EvidenceStore;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkSendHandler;

public final class ProtobufSendHandler
    implements ZLinkSendHandler<StringValue> {
    private final EvidenceStore state;

    public ProtobufSendHandler(EvidenceStore state) {
        this.state = state;
    }

    @Override
    public CompletionStage<Void> handle(
        StringValue message,
        ZLinkMessageContext context) {
        state.record("Send", "ProtobufEcho", message.getValue());
        state.record("ContentType", "ProtobufEcho", context.contentType().orElse(""));
        return CompletableFuture.completedFuture(null);
    }
}
