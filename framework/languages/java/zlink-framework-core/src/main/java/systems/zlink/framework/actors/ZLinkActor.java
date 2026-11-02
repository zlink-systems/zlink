package systems.zlink.framework.actors;

import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

public interface ZLinkActor {
    ZLinkActorContext context();

    default void configure() {}

    default CompletionStage<Void> onJoinCompleted(ZLinkActorJoinCompletion completion) {
        return CompletableFuture.completedFuture(null);
    }

    default CompletionStage<Optional<String>> onDuplicateBind(
            ZLinkSessionBindingIdentity current,
            ZLinkSessionBindingIdentity incoming,
            ZLinkActorCallbackCancellation cancellation) {
        return CompletableFuture.completedFuture(Optional.empty());
    }
}
