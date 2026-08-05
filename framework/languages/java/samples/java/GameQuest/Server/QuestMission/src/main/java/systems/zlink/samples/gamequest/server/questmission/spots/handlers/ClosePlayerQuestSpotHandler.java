package systems.zlink.samples.gamequest.server.questmission.spots.handlers;

import java.util.concurrent.CompletionStage;
import systems.zlink.framework.spots.ZLinkSpotPacketHandler;
import systems.zlink.samples.gamequest.server.questmission.spots.ClosePlayerQuestMsg;
import systems.zlink.samples.gamequest.server.questmission.spots.PlayerQuestSpot;

public final class ClosePlayerQuestSpotHandler
    implements ZLinkSpotPacketHandler<PlayerQuestSpot, ClosePlayerQuestMsg> {
    @Override
    public CompletionStage<Void> handle(
        PlayerQuestSpot spot,
        ClosePlayerQuestMsg message) {
        return spot.context().close().thenApply(ignored -> null);
    }
}
