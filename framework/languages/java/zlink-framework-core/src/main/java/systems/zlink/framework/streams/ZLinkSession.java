package systems.zlink.framework.streams;

import systems.zlink.framework.messaging.ZLinkMessage;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

public interface ZLinkSession {
    ZLinkSessionContext context();

    CompletionStage<Void> onConnected();

    CompletionStage<Void> onDisconnected();

    CompletionStage<Void> onError(ZLinkStreamError error);

    default CompletionStage<Void> onDispatch(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload) {
        return CompletableFuture.completedFuture(null);
    }

    /**
     * Invoked after a bound Actor is replaced while this physical Session
     * remains connected. Existing Session implementations are not required to
     * opt in; the default is an already-completed callback.
     */
    default CompletionStage<Void> onActorBindingReplaced(String actorId) {
        return CompletableFuture.completedFuture(null);
    }
}
