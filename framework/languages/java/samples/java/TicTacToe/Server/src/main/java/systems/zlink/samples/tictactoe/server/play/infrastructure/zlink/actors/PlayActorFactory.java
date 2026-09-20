package systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.actors;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

public final class PlayActorFactory implements ZLinkActorFactory {
    @Override
    public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
        return CompletableFuture.completedFuture(new PlayActor(context.actorId(), context));
    }
}
