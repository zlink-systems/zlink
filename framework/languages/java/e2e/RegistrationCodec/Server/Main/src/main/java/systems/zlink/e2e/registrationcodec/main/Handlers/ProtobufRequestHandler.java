package systems.zlink.e2e.registrationcodec.main.Handlers;

import com.google.protobuf.StringValue;
import systems.zlink.e2e.registrationcodec.main.Infrastructure.EvidenceStore;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;

public final class ProtobufRequestHandler
    implements ZLinkRequestHandler<StringValue, StringValue> {
    private final EvidenceStore state;

    public ProtobufRequestHandler(EvidenceStore state) {
        this.state = state;
    }

    @Override
    public java.util.concurrent.CompletionStage<StringValue> handle(
        StringValue request,
        ZLinkMessageContext context) {
        state.record("Request", "ProtobufEcho", request.getValue());
        state.record("ContentType", "ProtobufEcho", context.contentType().orElse(""));
        return java.util.concurrent.CompletableFuture.completedFuture(
            StringValue.of("echo:" + request.getValue()));
    }
}
