package MyGame.region.infrastructure.zlink;

import systems.zlink.framework.actors.*;

import java.util.concurrent.*;

public final class PlayerFactory implements ZLinkActorFactory {
    private final ZLinkActorClient actors;

    public PlayerFactory(ZLinkActorClient actors) {
        this.actors = actors;
    }

    public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
        return CompletableFuture.completedFuture(new Player(context, actors));
    }
}
