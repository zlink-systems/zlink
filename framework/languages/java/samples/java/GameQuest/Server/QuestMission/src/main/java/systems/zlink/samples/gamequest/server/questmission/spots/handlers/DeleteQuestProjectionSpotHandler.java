package systems.zlink.samples.gamequest.server.questmission.spots.handlers;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.spots.ZLinkSpotRequestHandler;
import systems.zlink.samples.gamequest.server.questmission.spots.PlayerQuestSpot;
import systems.zlink.samples.gamequest.shared.contracts.Messages;

public final class DeleteQuestProjectionSpotHandler
    implements ZLinkSpotRequestHandler<PlayerQuestSpot, Messages.DeleteQuestProjectionReq, Messages.DeleteQuestProjectionRes> {
    @Override
    public CompletionStage<Messages.DeleteQuestProjectionRes> handle(
        PlayerQuestSpot spot,
        Messages.DeleteQuestProjectionReq request) {
        return CompletableFuture.completedFuture(spot.delete(request));
    }
}
