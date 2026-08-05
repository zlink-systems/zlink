package systems.zlink.samples.supportchat.server.support.spots.conversationspot;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.spots.ZLinkSpotTimerHandler;
import systems.zlink.framework.spots.ZLinkTimerTick;

public final class ConversationIdleTimerHandler
    implements ZLinkSpotTimerHandler<ConversationSpot> {
    @Override
    public CompletionStage<Void> handle(ConversationSpot spot, ZLinkTimerTick tick) {
        spot.checkIdle();
        return CompletableFuture.completedFuture(null);
    }
}
