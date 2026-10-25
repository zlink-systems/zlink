package MyGame.zone;

import systems.zlink.framework.actors.*;

import java.util.concurrent.*;

public final class PlayerFactory implements ZLinkActorFactory {
    public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
        return CompletableFuture.completedFuture(new Player(context));
    }
}
