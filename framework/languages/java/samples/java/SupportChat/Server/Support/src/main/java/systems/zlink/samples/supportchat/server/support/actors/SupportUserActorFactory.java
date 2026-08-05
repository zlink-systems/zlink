package systems.zlink.samples.supportchat.server.support.actors;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;

public final class SupportUserActorFactory implements ZLinkActorFactory {
    @Override
    public java.util.concurrent.CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
        return java.util.concurrent.CompletableFuture.completedFuture(
            new SupportUserActor(context.actorId(), context));
    }
}
