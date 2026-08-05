package systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.actors;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;

public final class PlayActorFactory implements ZLinkActorFactory {
    @Override
    public java.util.concurrent.CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
        return java.util.concurrent.CompletableFuture.completedFuture(
            new PlayActor(context.actorId(), context));
    }
}
