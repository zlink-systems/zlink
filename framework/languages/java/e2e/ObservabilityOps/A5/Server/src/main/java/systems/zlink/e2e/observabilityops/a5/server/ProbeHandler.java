package systems.zlink.e2e.observabilityops.a5.server;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;

public final class ProbeHandler implements ZLinkRequestHandler<
    Contracts.ProbeReq,
    Contracts.ProbeRes> {
    @Override
    public CompletionStage<Contracts.ProbeRes> handle(
        Contracts.ProbeReq request,
        ZLinkMessageContext context) {
        if (request.fail()) {
            return CompletableFuture.failedFuture(
                new IllegalStateException("observability probe handler failure"));
        }
        return CompletableFuture.completedFuture(new Contracts.ProbeRes(request.value()));
    }
}
