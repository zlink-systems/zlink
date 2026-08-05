package systems.zlink.samples.gamequest.server.questmission.spots.handlers;

import java.util.concurrent.CompletionStage;
import systems.zlink.framework.actors.ZLinkActorClient;
import systems.zlink.framework.spots.ZLinkSpotPacketHandler;
import systems.zlink.samples.gamequest.server.configuration.SampleNames;
import systems.zlink.samples.gamequest.server.questmission.spots.PlayerQuestSpot;
import systems.zlink.samples.gamequest.shared.contracts.Messages;

public final class ApplyGameplaySpotHandler
    implements ZLinkSpotPacketHandler<PlayerQuestSpot, Messages.GameplayMsg> {
    private final ZLinkActorClient actors;

    public ApplyGameplaySpotHandler(ZLinkActorClient actors) {
        this.actors = actors;
    }

    @Override
    public CompletionStage<Void> handle(
        PlayerQuestSpot spot,
        Messages.GameplayMsg request) {
        Messages.QuestProcessingMsg result = spot.apply(request);
        return actors.sendToActor(request.playerId(), result).submit();
    }
}
