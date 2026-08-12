package systems.zlink.e2e.spotservice.shared;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;

public final class ScenarioActorFactory implements ZLinkActorFactory {
    @Override
    public CompletionStage<ZLinkActor> create(
        ZLinkActorContext context) {
        return CompletableFuture.completedFuture(new ScenarioActor(context));
    }
}
