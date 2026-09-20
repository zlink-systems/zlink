package systems.zlink.samples.gamequest.server.questmission.spots;

import systems.zlink.framework.spots.ZLinkInstanceSpot;
import systems.zlink.framework.spots.ZLinkInstanceSpotContext;
import systems.zlink.samples.gamequest.server.questmission.store.QuestStore;
import systems.zlink.samples.gamequest.shared.contracts.Messages;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

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

    // --8<-- [start:doc-gq-spot-init]
    @Override
    public CompletionStage<Void> onInitialize() {
        store.activate(playerId);
        if (store.hasEvents(playerId)) {
            System.out.printf(
                    "gamequest-mission replayed player=%s generation=%d%n",
                    playerId, context.objectGeneration());
        }
        if ("player-owner-unavailable".equals(playerId)) {
            System.out.printf(
                    "gamequest-owner-ready player=%s node=%s%n", playerId, store.nodeId());
        }
        return CompletableFuture.completedFuture(null);
    }

    // --8<-- [end:doc-gq-spot-init]

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
