package systems.zlink.e2e.registrationcodec.invalidduplicate.Handlers;

import systems.zlink.e2e.registrationcodec.shared.Contracts;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;

public final class ManualRequestHandler
    implements ZLinkRequestHandler<Contracts.EchoManualReq, Contracts.EchoRes> {
    @Override
    public java.util.concurrent.CompletionStage<Contracts.EchoRes> handle(
        Contracts.EchoManualReq request,
        ZLinkMessageContext context) {
        return java.util.concurrent.CompletableFuture.completedFuture(
            new Contracts.EchoRes("echo:" + request.value(), "manual"));
    }
}
