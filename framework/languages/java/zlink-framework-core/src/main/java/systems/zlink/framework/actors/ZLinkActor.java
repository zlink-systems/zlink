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

    /**
     * Notifies this Actor after a Session binding is committed. Failure is reported as a runtime
     * task exception and does not change the successful bind result. The default is a no-op.
     */
    default CompletionStage<Void> onSessionBound(
            ZLinkSessionBindingIdentity incoming, ZLinkActorCallbackCancellation cancellation) {
        return CompletableFuture.completedFuture(null);
    }

    default CompletionStage<Optional<String>> onDuplicateBind(
            ZLinkSessionBindingIdentity current,
            ZLinkSessionBindingIdentity incoming,
            ZLinkActorCallbackCancellation cancellation) {
        return CompletableFuture.completedFuture(Optional.empty());
    }
}
