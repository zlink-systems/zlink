package systems.zlink.samples.gamequest.server.gameapi.actors;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;

public final class GameQuestPlayerActorFactory implements ZLinkActorFactory {
    @Override
    public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
        return CompletableFuture.completedFuture(
            new GameQuestPlayerActor(context.actorId(), context));
    }
}
