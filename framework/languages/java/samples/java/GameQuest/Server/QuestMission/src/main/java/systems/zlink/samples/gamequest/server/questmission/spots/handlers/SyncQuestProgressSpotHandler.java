package systems.zlink.samples.gamequest.server.questmission.spots.handlers;

import systems.zlink.framework.spots.ZLinkSpotRequestHandler;
import systems.zlink.samples.gamequest.server.questmission.spots.PlayerQuestSpot;
import systems.zlink.samples.gamequest.shared.contracts.Messages;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

public final class SyncQuestProgressSpotHandler
        implements ZLinkSpotRequestHandler<
                PlayerQuestSpot, Messages.SyncQuestProgressReq, Messages.SyncQuestProgressRes> {
    @Override
    public CompletionStage<Messages.SyncQuestProgressRes> handle(
            PlayerQuestSpot spot, Messages.SyncQuestProgressReq request) {
        return CompletableFuture.completedFuture(spot.sync(request));
    }
}
