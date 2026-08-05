package systems.zlink.testfixtures.handlerasync;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.handlers.ZLinkRequest;

public final class CompletionStageAttributedHandler {
    @ZLinkRequest(packetName = "CompletionStageRequest")
    public CompletionStage<String> handle(String request) {
        return CompletableFuture.completedFuture(request);
    }
}
