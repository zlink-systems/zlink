package systems.zlink.e2e.kotlin.automaticturn;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.channels.ZLinkRequestContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;

public final class DelayHandler
    implements ZLinkRequestHandler<Contracts.DelayReq, Contracts.DelayRes> {
    @Override
    public CompletionStage<Contracts.DelayRes> handle(
        Contracts.DelayReq request,
        ZLinkRequestContext context) {
        try {
            Thread.sleep(request.millis());
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException("delay interrupted", error);
        }
        return CompletableFuture.completedFuture(
            new Contracts.DelayRes("delay:" + request.value()));
    }
}
