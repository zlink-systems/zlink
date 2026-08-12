package systems.zlink.e2e.registrationcodec.main.Handlers;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

import systems.zlink.e2e.registrationcodec.shared.protobuf.ProtobufEchoMsg;
import systems.zlink.e2e.registrationcodec.main.Infrastructure.EvidenceStore;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkSendHandler;

public final class ProtobufSendHandler
    implements ZLinkSendHandler<ProtobufEchoMsg> {
    private final EvidenceStore state;

    public ProtobufSendHandler(EvidenceStore state) {
        this.state = state;
    }

    @Override
    public CompletionStage<Void> handle(
        ProtobufEchoMsg message,
        ZLinkMessageContext context) {
        state.record("Send", "ProtobufEchoMsg", message.getValue());
        state.record("ContentType", "ProtobufEchoMsg", context.contentType().orElse(""));
        return CompletableFuture.completedFuture(null);
    }
}
