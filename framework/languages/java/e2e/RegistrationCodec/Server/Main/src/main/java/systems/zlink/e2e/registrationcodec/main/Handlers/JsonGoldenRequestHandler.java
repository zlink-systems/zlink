package systems.zlink.e2e.registrationcodec.main.Handlers;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.e2e.registrationcodec.main.Infrastructure.EvidenceStore;
import systems.zlink.e2e.registrationcodec.shared.Contracts;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;

public final class JsonGoldenRequestHandler
    implements ZLinkRequestHandler<Contracts.JsonGoldenReq, Contracts.JsonGoldenRes> {
    private final EvidenceStore state;

    public JsonGoldenRequestHandler(EvidenceStore state) {
        this.state = state;
    }

    @Override
    public CompletionStage<Contracts.JsonGoldenRes> handle(
        Contracts.JsonGoldenReq request,
        ZLinkMessageContext context) {
        String contentType = context.contentType().orElse("");
        state.record("Request", "JsonGoldenReq", request.value());
        state.record("ContentType", "JsonGoldenReq", contentType);
        return CompletableFuture.completedFuture(new Contracts.JsonGoldenRes(
            "Ada Lovelace",
            "ready",
            -9_223_372_036_854_775_000L,
            new byte[] {0x00, 0x7f, (byte) 0x80, (byte) 0xff},
            2_147_000_001,
            0.125,
            null,
            contentType));
    }
}
