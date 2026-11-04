package systems.zlink.framework.runtime.binding;

import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorJoinRequest;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotDispatchHandler;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotDispatchInfo;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendTopicMessage;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalAsyncSpotDispatchHandler;

import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

public final class ZLinkJavaRawSpotTestAccess {
    private ZLinkJavaRawSpotTestAccess() {}

    public static boolean hasNoPendingSubscriptions(ZLinkBackendSpot spot) {
        return ((ZLinkJavaRawSpot) spot).subscriptions.isEmpty();
    }

    public static AutoCloseable observeNextTopic(
            ZLinkBackendSpot spot, CompletableFuture<ZLinkBackendTopicMessage> observed) {
        var raw = (ZLinkJavaRawSpot) spot;
        ZLinkBackendSpotDispatchHandler original = raw.dispatchHandler;
        if (!(original instanceof ZLinkInternalAsyncSpotDispatchHandler async)) {
            throw new IllegalStateException("Spot dispatch handler is not asynchronous");
        }
        raw.onDispatchEvent(
                new ZLinkInternalAsyncSpotDispatchHandler() {
                    @Override
                    public CompletionStage<Void> handleAsync(ZLinkBackendSpotDispatchInfo info) {
                        return async.handleAsync(info);
                    }

                    @Override
                    public CompletionStage<Void> handleRoute(
                            ZLinkBackendReceived received, CompletableFuture<Void> admission) {
                        return async.handleRoute(received, admission);
                    }

                    @Override
                    public Boolean handleTopic(ZLinkBackendTopicMessage message) {
                        Boolean accepted = async.handleTopic(message);
                        if (Boolean.TRUE.equals(accepted)) observed.complete(message);
                        return accepted;
                    }

                    @Override
                    public CompletionStage<Void> handleJoin(ZLinkBackendActorJoinRequest request) {
                        return async.handleJoin(request);
                    }

                    @Override
                    public CompletionStage<Void> handleActor(
                            List<ZLinkBackendActorReceived> messages,
                            CompletableFuture<Void> requestAdmission) {
                        return async.handleActor(messages, requestAdmission);
                    }
                });
        return () -> raw.onDispatchEvent(original);
    }
}
