package systems.zlink.framework.runtime.internal.backend;

import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

/**
 * Keeps native claim completion inside the Java backend without changing the public backend
 * dispatch callback contract.
 */
public interface ZLinkInternalAsyncSpotDispatchHandler extends ZLinkBackendSpotDispatchHandler {
    CompletionStage<Void> handleAsync(ZLinkBackendSpotDispatchInfo info);

    default CompletionStage<Void> handleRoute(ZLinkBackendReceived received) {
        return null;
    }

    default CompletionStage<Void> handleRoute(
            ZLinkBackendReceived received, CompletableFuture<Void> admission) {
        return null;
    }

    default Boolean handleTopic(ZLinkBackendTopicMessage message) {
        return null;
    }

    default CompletionStage<Void> handleJoin(ZLinkBackendActorJoinRequest request) {
        return null;
    }

    default CompletionStage<Void> handleActor(List<ZLinkBackendActorReceived> messages) {
        return null;
    }

    default CompletionStage<Void> handleLifecycle(ZLinkBackendActorLifecycleEvent event) {
        return null;
    }

    @Override
    default void handle(ZLinkBackendSpotDispatchInfo info) {
        handleAsync(info);
    }
}
