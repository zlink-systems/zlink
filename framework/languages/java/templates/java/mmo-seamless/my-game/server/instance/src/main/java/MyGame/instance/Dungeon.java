package MyGame.instance;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.spots.*;

import java.util.concurrent.*;

// A game instance is a User Spot, not an actor-free Framework Instance Spot.
public final class Dungeon implements ZLinkSpot<ZLinkActor> {
    private final ZLinkSpotContext context;

    public Dungeon(ZLinkSpotContext context) {
        this.context = context;
    }

    public ZLinkSpotContext context() {
        return context;
    }

    public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
        return CompletableFuture.completedFuture(null);
    }

    public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
        return CompletableFuture.completedFuture(null);
    }
}
