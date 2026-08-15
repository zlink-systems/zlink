package systems.zlink.framework.runtime.channels;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.BiConsumer;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;

final class ZLinkSpotRouterNodeDispatcher {
    private ZLinkSpotRouterNodeDispatcher() {
    }

    static CompletionStage<Void> send(
        String routerChannelId,
        ZLinkInternalSpotNode node,
        RoutingId targetNodeRid,
        String targetSpotId,
        long targetSpotGeneration,
        List<Message> spotParts,
        Duration timeout,
        BiConsumer<CompletableFuture<Void>, Duration> trackPendingRequest) {
        CompletableFuture<Void> result = new CompletableFuture<>();
        trackPendingRequest.accept(result, timeout);
        node.entrySpot().sendToSpot(
                targetNodeRid,
                targetSpotId,
                targetSpotGeneration,
                spotParts)
            .whenComplete((ignored, failure) -> {
                ZLinkChannelRuntime.trace(
                    "spot-route node-send-submit router=" + routerChannelId
                        + " targetNode=" + targetNodeRid
                        + " targetSpot=" + targetSpotId
                        + " result=" + (failure == null ? "accepted" : "failed"));
                if (failure == null) {
                    result.complete(null);
                } else {
                    result.completeExceptionally(failure);
                }
            });
        return result;
    }

    static CompletionStage<List<Message>> request(
        String routerChannelId,
        ZLinkInternalSpotNode node,
        RoutingId targetNodeRid,
        String targetSpotId,
        long targetSpotGeneration,
        List<Message> spotParts,
        Duration timeout,
        BiConsumer<CompletableFuture<List<Message>>, Duration> trackPendingRequest) {
        CompletableFuture<List<Message>> result = new CompletableFuture<>();
        trackPendingRequest.accept(result, timeout);
        long requestStartedNanos = System.nanoTime();
        node.entrySpot().requestToSpot(
                targetNodeRid,
                targetSpotId,
                targetSpotGeneration,
                spotParts,
                timeout)
            .whenComplete((reply, failure) -> {
                if (failure == null) {
                    completeReply(reply, result, requestStartedNanos);
                } else {
                    result.completeExceptionally(failure);
                }
            });
        return result;
    }

    private static void completeReply(
        ZLinkBackendReceived reply,
        CompletableFuture<List<Message>> result,
        long requestStartedNanos) {
        try {
            ZLinkChannelRuntime.trace("spot-route node-reply"
                + " elapsedMs=" + ZLinkChannelRuntime.elapsedMillis(requestStartedNanos)
                + " result=" + reply.result()
                + " origin=spot-node-callback"
                + " sourceRid=" + reply.routingId().map(Object::toString).orElse(null)
                + " sourceSpot=" + reply.spotId().map(Object::toString).orElse(null)
                + " requestSeq=" + reply.requestSeq().map(Object::toString).orElse(null)
                + " parts=" + ZLinkChannelRuntime.describeTraceParts(reply.parts()));
            if (reply.result() != ZLinkBackendRequestResult.OK) {
                result.completeExceptionally(new ZLinkFrameworkException(
                    reply.result().toFrameworkErrorKind(),
                    "SPOT route request failed: " + reply.result()));
                return;
            }
            List<Message> replyParts = ZLinkChannelRuntime.copyMessages(reply.parts());
            if (ZLinkChannelRuntime.isFrameworkErrorReply(replyParts)) {
                replyParts.forEach(Message::close);
                result.completeExceptionally(new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.INTERNAL_FAILURE,
                    ZLinkChannelRuntime.frameworkErrorReplyMessage(reply.parts())));
                return;
            }
            if (!result.complete(replyParts)) {
                replyParts.forEach(Message::close);
            }
        } finally {
            reply.close();
        }
    }

}
