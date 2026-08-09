package Handlers;
import systems.zlink.e2e.registrationcodec.main.Handlers;
import systems.zlink.e2e.registrationcodec.main.Infrastructure;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

import systems.zlink.e2e.registrationcodec.shared.Contracts;
import systems.zlink.e2e.registrationcodec.main.Infrastructure.EvidenceStore;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;
import systems.zlink.framework.handlers.ZLinkRequest;
import systems.zlink.framework.handlers.ZLinkSend;

@ZLinkHandlerGroup(Contracts.ATTR_GROUP)
public final class AttrEchoHandler {
    private final EvidenceStore state;

    public AttrEchoHandler(EvidenceStore state) {
        this.state = state;
    }

    @ZLinkRequest(packetName = "EchoAttr")
    public CompletionStage<Contracts.EchoRes> request(
        Contracts.EchoAttrReq request,
        ZLinkMessageContext context) {
        state.record("Request", "EchoAttr", request.value());
        return CompletableFuture.completedFuture(
            new Contracts.EchoRes("echo:" + request.value(), "attr"));
    }

    @ZLinkSend(packetName = "EchoAttr")
    public CompletionStage<Void> send(
        Contracts.EchoAttrMsg message,
        ZLinkMessageContext context) {
        state.record("Send", "EchoAttr", message.value());
        return CompletableFuture.completedFuture(null);
    }
}
