package systems.zlink.e2e.registrationcodec.jsononlypeer.Handlers;

import systems.zlink.e2e.registrationcodec.shared.protobuf.ProtobufEchoReq;
import systems.zlink.e2e.registrationcodec.shared.protobuf.ProtobufEchoRes;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.e2e.registrationcodec.jsononlypeer.Infrastructure.EvidenceStore;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;

/** Records an execution if an unsupported wire codec reaches the handler. */
public final class UnexpectedProtobufHandler
    implements ZLinkRequestHandler<ProtobufEchoReq, ProtobufEchoRes> {
    private final EvidenceStore state;

    public UnexpectedProtobufHandler(EvidenceStore state) {
        this.state = state;
    }

    @Override
    public CompletionStage<ProtobufEchoRes> handle(
        ProtobufEchoReq request,
        ZLinkMessageContext context) {
        state.record("UnexpectedHandler", "ProtobufEchoReq", request.getValue());
        return CompletableFuture.completedFuture(
            ProtobufEchoRes.newBuilder().setValue(request.getValue()).build());
    }
}
