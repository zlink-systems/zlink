package systems.zlink.e2e.spotservice.shared;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;

public final class ScenarioActorFactory implements ZLinkActorFactory {
    @Override
    public java.util.concurrent.CompletionStage<ZLinkActor> create(
        ZLinkActorContext context) {
        return java.util.concurrent.CompletableFuture.completedFuture(new ScenarioActor(context));
    }
}
