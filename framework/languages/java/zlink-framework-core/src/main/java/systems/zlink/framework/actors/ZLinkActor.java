package systems.zlink.framework.actors;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

public interface ZLinkActor {
    ZLinkActorContext context();

    default void configure() {
    }

    default CompletionStage<Void> onJoinCompleted(
        ZLinkActorJoinCompletion completion) {
        return CompletableFuture.completedFuture(null);
    }
}
