package systems.zlink.samples.bingo.server.play.infrastructure.zlink.actors;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;

public final class PlayerActorFactory implements ZLinkActorFactory {
    @Override
    public CompletionStage<ZLinkActor> create(
        ZLinkActorContext context) {
        return CompletableFuture.completedFuture(
            new PlayerActor(context.actorId(), context));
    }
}
