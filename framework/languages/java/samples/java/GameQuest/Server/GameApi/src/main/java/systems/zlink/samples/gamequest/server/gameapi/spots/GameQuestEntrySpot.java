package systems.zlink.samples.gamequest.server.gameapi.spots;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.samples.gamequest.server.gameapi.actors.GameQuestPlayerActor;

public final class GameQuestEntrySpot implements ZLinkEntrySpot<GameQuestPlayerActor> {
    private final ZLinkEntrySpotContext context;

    public GameQuestEntrySpot(ZLinkEntrySpotContext context) {
        this.context = context;
    }

    @Override
    public ZLinkEntrySpotContext context() {
        return context;
    }

    @Override
    public CompletionStage<Void> onJoinedActor(GameQuestPlayerActor actor) {
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onLeaveActor(GameQuestPlayerActor actor) {
        return CompletableFuture.completedFuture(null);
    }
}
