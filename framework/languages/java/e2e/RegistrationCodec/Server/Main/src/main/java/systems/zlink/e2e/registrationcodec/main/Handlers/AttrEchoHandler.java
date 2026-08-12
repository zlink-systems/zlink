package systems.zlink.e2e.registrationcodec.main.Handlers;
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

    @ZLinkRequest(packetName = "EchoAttrReq")
    public CompletionStage<Contracts.EchoRes> request(
        Contracts.EchoAttrReq request,
        ZLinkMessageContext context) {
        state.record("Request", "EchoAttrReq", request.value());
        return CompletableFuture.completedFuture(
            new Contracts.EchoRes("echo:" + request.value(), "attr"));
    }

    @ZLinkSend(packetName = "EchoAttrMsg")
    public CompletionStage<Void> send(
        Contracts.EchoAttrMsg message,
        ZLinkMessageContext context) {
        state.record("Send", "EchoAttrMsg", message.value());
        return CompletableFuture.completedFuture(null);
    }
}
