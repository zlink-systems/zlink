package systems.zlink.samples.gamequest.server.questmission.spots;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.spots.ZLinkInstanceSpot;
import systems.zlink.framework.spots.ZLinkInstanceSpotContext;
import systems.zlink.samples.gamequest.server.questmission.spots.handlers.ApplyGameplaySpotHandler;
import systems.zlink.samples.gamequest.server.questmission.spots.handlers.DeleteQuestProjectionSpotHandler;
import systems.zlink.samples.gamequest.server.questmission.spots.handlers.ClosePlayerQuestSpotHandler;
import systems.zlink.samples.gamequest.server.questmission.spots.handlers.GetQuestProgressSpotHandler;
import systems.zlink.samples.gamequest.server.questmission.spots.handlers.RebuildQuestProjectionSpotHandler;
import systems.zlink.samples.gamequest.server.questmission.spots.handlers.SyncQuestProgressSpotHandler;
import systems.zlink.samples.gamequest.server.questmission.store.QuestStore;
import systems.zlink.samples.gamequest.shared.contracts.Messages;

public final class PlayerQuestSpot implements ZLinkInstanceSpot {
    private final ZLinkInstanceSpotContext context;
    private final QuestStore store;
    private String playerId;

    public PlayerQuestSpot(ZLinkInstanceSpotContext context, QuestStore store) {
        this.context = context;
        this.store = store;
        this.playerId = context.spotId();
    }

    @Override
    public ZLinkInstanceSpotContext context() {
        return context;
    }

    @Override
    public CompletionStage<Void> onInitialize() {
        store.activate(playerId);
        return CompletableFuture.completedFuture(null);
    }

    private void requirePlayer(String requestedPlayerId) {
        if (!playerId.equals(requestedPlayerId)) {
            throw new IllegalArgumentException(
                "request player does not match owner Spot: " + requestedPlayerId);
        }
    }

    public Messages.QuestProcessingMsg apply(Messages.GameplayMsg message) {
        requirePlayer(message.playerId());
        return store.apply(message);
    }

    public Messages.GetQuestProgressRes progress(Messages.GetQuestProgressReq request) {
        requirePlayer(request.playerId());
        return new Messages.GetQuestProgressRes(store.projection(playerId));
    }

    public Messages.SyncQuestProgressRes sync(Messages.SyncQuestProgressReq request) {
        requirePlayer(request.playerId());
        return store.sync(playerId);
    }

    public Messages.DeleteQuestProjectionRes delete(Messages.DeleteQuestProjectionReq request) {
        requirePlayer(request.playerId());
        return store.deleteProjection(playerId, request.questId());
    }

    public Messages.QuestProgress rebuild(Messages.RebuildQuestProjectionReq request) {
        requirePlayer(request.playerId());
        return store.rebuildProjection(playerId, request.questId(), request.count());
    }

}
