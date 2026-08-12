package systems.zlink.e2e.registrationcodec.main.Handlers;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

import systems.zlink.e2e.registrationcodec.shared.protobuf.ProtobufEchoReq;
import systems.zlink.e2e.registrationcodec.shared.protobuf.ProtobufEchoRes;
import systems.zlink.e2e.registrationcodec.main.Infrastructure.EvidenceStore;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;

public final class ProtobufRequestHandler
    implements ZLinkRequestHandler<ProtobufEchoReq, ProtobufEchoRes> {
    private final EvidenceStore state;

    public ProtobufRequestHandler(EvidenceStore state) {
        this.state = state;
    }

    @Override
    public CompletionStage<ProtobufEchoRes> handle(
        ProtobufEchoReq request,
        ZLinkMessageContext context) {
        state.record("Request", "ProtobufEchoReq", request.getValue());
        state.record("ContentType", "ProtobufEchoReq", context.contentType().orElse(""));
        return CompletableFuture.completedFuture(
            ProtobufEchoRes.newBuilder()
                .setValue("echo:" + request.getValue())
                .build());
    }
}
