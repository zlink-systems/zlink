package Handlers;
import systems.zlink.e2e.registrationcodec.jsononlypeer.Handlers;
import systems.zlink.e2e.registrationcodec.jsononlypeer.Infrastructure;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

import systems.zlink.e2e.registrationcodec.shared.Contracts;
import systems.zlink.e2e.registrationcodec.jsononlypeer.Infrastructure.EvidenceStore;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;

public final class JsonRequestHandler
    implements ZLinkRequestHandler<Contracts.JsonEchoReq, Contracts.EchoRes> {
    private final EvidenceStore state;

    public JsonRequestHandler(EvidenceStore state) {
        this.state = state;
    }

    @Override
    public CompletionStage<Contracts.EchoRes> handle(
        Contracts.JsonEchoReq request,
        ZLinkMessageContext context) {
        state.record("Request", "JsonEcho", request.value());
        state.record("ContentType", "JsonEcho", context.contentType().orElse("missing"));
        return CompletableFuture.completedFuture(
            new Contracts.EchoRes("echo:" + request.value(), "json"));
    }
}
