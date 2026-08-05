package systems.zlink.samples.bingo.server.play.infrastructure.zlink.actors;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;

public final class PlayerActorFactory implements ZLinkActorFactory {
    @Override
    public java.util.concurrent.CompletionStage<ZLinkActor> create(
        ZLinkActorContext context) {
        return java.util.concurrent.CompletableFuture.completedFuture(
            new PlayerActor(context.actorId(), context));
    }
}
