package systems.zlink.samples.gamequest.server.gameapi.spots;

import java.util.concurrent.CompletionStage;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.spots.ZLinkEntrySpotActorSendHandler;
import systems.zlink.samples.gamequest.server.gameapi.actors.GameQuestPlayerActor;
import systems.zlink.samples.gamequest.server.gameapi.store.GameQuestStore;
import systems.zlink.samples.gamequest.shared.contracts.Messages;

public final class QuestProcessingActorHandler
    implements ZLinkEntrySpotActorSendHandler<
        GameQuestEntrySpot,
        GameQuestPlayerActor,
        Messages.QuestProcessingMsg> {
    private final GameQuestStore store;

    public QuestProcessingActorHandler(GameQuestStore store) {
        this.store = store;
    }

    @Override
    public CompletionStage<Void> handle(
        GameQuestEntrySpot entrySpot,
        GameQuestPlayerActor actor,
        ZLinkMessageContext context,
        Messages.QuestProcessingMsg message) {
        store.mergeProjection(message.playerId(), message.projection());
        return actor.push(message);
    }
}
