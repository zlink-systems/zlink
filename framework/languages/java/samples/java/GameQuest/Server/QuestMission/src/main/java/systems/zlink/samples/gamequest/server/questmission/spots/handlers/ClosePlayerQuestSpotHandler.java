package systems.zlink.samples.gamequest.server.questmission.spots.handlers;

import systems.zlink.framework.spots.ZLinkSpotPacketHandler;
import systems.zlink.samples.gamequest.server.questmission.spots.ClosePlayerQuestMsg;
import systems.zlink.samples.gamequest.server.questmission.spots.PlayerQuestSpot;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

// --8<-- [start:doc-gq-close-handler]
public final class ClosePlayerQuestSpotHandler
        implements ZLinkSpotPacketHandler<PlayerQuestSpot, ClosePlayerQuestMsg> {
    @Override
    public CompletionStage<Void> handle(PlayerQuestSpot spot, ClosePlayerQuestMsg message) {
        spot.context().close();
        return CompletableFuture.completedFuture(null);
    }
}
// --8<-- [end:doc-gq-close-handler]
