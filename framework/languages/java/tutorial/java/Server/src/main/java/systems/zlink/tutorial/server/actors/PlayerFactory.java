package systems.zlink.tutorial.server.actors;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;

// --8<-- [start:actor-factory]
// The Framework creates players through this factory rather than by calling a
// constructor, so dependencies can be injected here.
public final class PlayerFactory implements ZLinkActorFactory {

    @Override
    public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
        return CompletableFuture.completedFuture(new Player(context));
    }
}
// --8<-- [end:actor-factory]
