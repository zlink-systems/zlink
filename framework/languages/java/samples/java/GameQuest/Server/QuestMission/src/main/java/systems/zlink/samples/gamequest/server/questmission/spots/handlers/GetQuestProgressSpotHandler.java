package systems.zlink.samples.gamequest.server.questmission.spots.handlers;

import systems.zlink.framework.spots.ZLinkSpotRequestHandler;
import systems.zlink.samples.gamequest.server.questmission.spots.PlayerQuestSpot;
import systems.zlink.samples.gamequest.shared.contracts.Messages;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

public final class GetQuestProgressSpotHandler
        implements ZLinkSpotRequestHandler<
                PlayerQuestSpot, Messages.GetQuestProgressReq, Messages.GetQuestProgressRes> {
    @Override
    public CompletionStage<Messages.GetQuestProgressRes> handle(
            PlayerQuestSpot spot, Messages.GetQuestProgressReq request) {
        return CompletableFuture.completedFuture(spot.progress(request));
    }
}
