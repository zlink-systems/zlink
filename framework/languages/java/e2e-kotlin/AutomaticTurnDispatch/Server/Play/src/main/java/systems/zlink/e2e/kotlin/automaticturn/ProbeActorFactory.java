package systems.zlink.e2e.kotlin.automaticturn;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;

public final class ProbeActorFactory implements ZLinkActorFactory {
    @Override
    public CompletionStage<ZLinkActor> create(String actorId, ZLinkActorContext context) {
        return CompletableFuture.completedFuture(new ProbeActor(actorId, context));
    }
}
