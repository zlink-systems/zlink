package systems.zlink.samples.supportchat.server.support.actors;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

public final class SupportUserActorFactory implements ZLinkActorFactory {
    @Override
    public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
        return CompletableFuture.completedFuture(new SupportUserActor(context.actorId(), context));
    }
}
