package systems.zlink.framework.runtime.spots;

import systems.zlink.framework.spots.ZLinkInstanceSpotContext;
import systems.zlink.framework.spots.ZLinkSpotManager;

import java.util.concurrent.CompletionStage;

public final class ZLinkSpotRuntimeTesting {
    private ZLinkSpotRuntimeTesting() {}

    public static CompletionStage<Boolean> closeInstanceSpot(
            ZLinkSpotManager manager, String spotId, long objectGeneration) {
        return ((ZLinkSpotRuntime) manager).closeInstanceSpot(spotId, objectGeneration);
    }

    public static CompletionStage<Boolean> hasPendingMessages(ZLinkInstanceSpotContext context) {
        return ((DefaultInstanceSpotContext) context)
                .ownerQueue()
                .pendingMessages()
                .thenApply(messages -> !messages.isEmpty());
    }
}
