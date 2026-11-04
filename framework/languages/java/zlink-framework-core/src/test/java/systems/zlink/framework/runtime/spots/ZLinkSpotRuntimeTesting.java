package systems.zlink.framework.runtime.spots;

import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.binding.ZLinkJavaRawSpotTestAccess;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendTopicMessage;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.spots.ZLinkInstanceSpotContext;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotManager;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

public final class ZLinkSpotRuntimeTesting {
    private ZLinkSpotRuntimeTesting() {}

    public static CompletionStage<Boolean> closeInstanceSpot(
            ZLinkSpotManager manager, String spotId, long objectGeneration) {
        return ((ZLinkSpotRuntime) manager).closeInstanceSpot(spotId, objectGeneration);
    }

    public static CompletionStage<Boolean> hasPendingMessages(ZLinkInstanceSpotContext context) {
        return ((DefaultInstanceSpotContext) context)
                .ownerQueue()
                .pendingMessages()
                .thenApply(messages -> !messages.isEmpty());
    }

    public static CompletionStage<Void> admitInstanceSpotRoute(
            ZLinkSpotManager manager,
            String spotId,
            ZLinkBackendReceived received,
            CompletableFuture<Void> admission) {
        return ((ZLinkSpotRuntime) manager)
                .instanceSpotActivations
                .get(spotId)
                .admitRoute(received, admission);
    }

    public static ZLinkInternalMeshNode meshNode(ZLinkSpotManager manager, String meshName) {
        return ((ZLinkSpotRuntime) manager).routeMeshNodesByName.get(meshName);
    }

    public static CompletableFuture<Boolean> retainedCloseAttempt(ZLinkSpotManager manager) {
        return ((ZLinkSpotRuntime) manager)
                .closingCoordinators
                .values()
                .iterator()
                .next()
                .coordinator()
                .attempt;
    }

    public static ZLinkInternalSpotNode primaryNode(ZLinkSpotManager manager) {
        return ((ZLinkSpotRuntime) manager).primaryNode();
    }

    public static ZLinkSerialExecutionQueue spotQueue(ZLinkSpotContext context) {
        return ((DefaultSpotContext) context).serials.spotQueue;
    }

    public static boolean hasNoPendingSubscriptions(ZLinkSpotContext context) {
        return ZLinkJavaRawSpotTestAccess.hasNoPendingSubscriptions(
                ((DefaultSpotContext) context).backendSpot);
    }

    public static AutoCloseable observeNextTopic(
            ZLinkSpotContext context, CompletableFuture<ZLinkBackendTopicMessage> observed) {
        return ZLinkJavaRawSpotTestAccess.observeNextTopic(
                ((DefaultSpotContext) context).backendSpot, observed);
    }
}
