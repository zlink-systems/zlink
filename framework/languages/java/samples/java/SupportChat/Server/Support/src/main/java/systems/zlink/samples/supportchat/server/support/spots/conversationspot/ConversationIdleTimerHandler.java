package systems.zlink.samples.supportchat.server.support.spots.conversationspot;

import systems.zlink.framework.spots.ZLinkSpotTimerHandler;
import systems.zlink.framework.spots.ZLinkTimerTick;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

// --8<-- [start:doc-sc-idle-timer]
public final class ConversationIdleTimerHandler implements ZLinkSpotTimerHandler<ConversationSpot> {
    @Override
    public CompletionStage<Void> handle(ConversationSpot spot, ZLinkTimerTick tick) {
        spot.checkIdle();
        return CompletableFuture.completedFuture(null);
    }
}
// --8<-- [end:doc-sc-idle-timer]
