package systems.zlink.e2e.registrationcodec.jsononlypeer.Handlers;

import com.google.protobuf.StringValue;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.e2e.registrationcodec.jsononlypeer.Infrastructure.EvidenceStore;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;

/** Records an execution if an unsupported wire codec reaches the handler. */
public final class UnexpectedProtobufHandler
    implements ZLinkRequestHandler<StringValue, StringValue> {
    private final EvidenceStore state;

    public UnexpectedProtobufHandler(EvidenceStore state) {
        this.state = state;
    }

    @Override
    public CompletionStage<StringValue> handle(
        StringValue request,
        ZLinkMessageContext context) {
        state.record("UnexpectedHandler", "StringValue", request.getValue());
        return CompletableFuture.completedFuture(request);
    }
}
