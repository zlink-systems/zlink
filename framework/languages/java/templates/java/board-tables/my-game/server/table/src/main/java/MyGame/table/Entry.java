package MyGame.table;

import systems.zlink.framework.spots.*;

import java.util.concurrent.*;

public final class Entry implements ZLinkEntrySpot<Player> {
    private final ZLinkEntrySpotContext context;

    public Entry(ZLinkEntrySpotContext context) {
        this.context = context;
    }

    public ZLinkEntrySpotContext context() {
        return context;
    }

    public CompletionStage<Void> onJoinedActor(Player actor) {
        return CompletableFuture.completedFuture(null);
    }

    public CompletionStage<Void> onLeaveActor(Player actor) {
        return CompletableFuture.completedFuture(null);
    }
}
