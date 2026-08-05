package systems.zlink.e2e.registrationcodec.main.Handlers;

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
    public java.util.concurrent.CompletionStage<Void> handle(
        StringValue message,
        ZLinkMessageContext context) {
        state.record("Send", "ProtobufEcho", message.getValue());
        state.record("ContentType", "ProtobufEcho", context.contentType().orElse(""));
        return java.util.concurrent.CompletableFuture.completedFuture(null);
    }
}
