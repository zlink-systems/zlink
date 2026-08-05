package systems.zlink.framework.spots;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

public interface ZLinkInstanceSpot {
    ZLinkInstanceSpotContext context();

    default void configure() {
    }

    default CompletionStage<Void> onInitialize() {
        return CompletableFuture.completedFuture(null);
    }

    default CompletionStage<Void> onClosing(
        ZLinkSpotClosingContext context) {
        return CompletableFuture.completedFuture(null);
    }
}
