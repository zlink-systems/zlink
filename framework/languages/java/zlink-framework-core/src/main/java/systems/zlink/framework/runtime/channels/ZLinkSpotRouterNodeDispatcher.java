package systems.zlink.framework.runtime.channels;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.BiConsumer;
import java.util.function.Consumer;
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
        BiConsumer<CompletableFuture<Void>, Duration> trackPendingRequest,
        Consumer<Runnable> retrySend) {
        CompletableFuture<Void> result = new CompletableFuture<>();
        trackPendingRequest.accept(result, timeout);
        List<byte[]> payloads = spotParts.stream().map(Message::toByteArray).toList();
        long deadline = System.nanoTime() + timeout.toNanos();
        class Attempt implements Runnable {
            @Override
            public void run() {
                if (result.isDone()) {
                    return;
                }
                List<Message> attemptParts = payloads.stream()
                    .map(Message::from)
                    .toList();
                try {
                    boolean submitted = node.entrySpot().sendToSpot(
                        targetNodeRid,
                        targetSpotId,
                        targetSpotGeneration,
                        attemptParts,
                        SendFlags.NONE);
                    ZLinkChannelRuntime.trace("spot-route node-send-submit router=" + routerChannelId
                        + " targetNode=" + targetNodeRid
                        + " targetSpot=" + targetSpotId
                        + " submitted=" + submitted);
                    if (submitted) {
                        result.complete(null);
                    } else if (System.nanoTime() < deadline) {
                        retrySend.accept(this);
                    } else {
                        result.completeExceptionally(new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                            "Spot node router '" + routerChannelId
                                + "' was not ready before the send timeout."));
                    }
                } catch (RuntimeException ex) {
                    if (ZLinkChannelRequestSubmitter.isRetriableSubmit(ex)
                        && System.nanoTime() < deadline) {
                        retrySend.accept(this);
                    } else {
                        result.completeExceptionally(ex);
                    }
                } finally {
                    attemptParts.forEach(Message::close);
                }
            }
        }
        new Attempt().run();
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
        BiConsumer<CompletableFuture<List<Message>>, Duration> trackPendingRequest,
        Consumer<Runnable> retryRequest) {
        CompletableFuture<List<Message>> result = new CompletableFuture<>();
        trackPendingRequest.accept(result, timeout);
        List<byte[]> payloads = spotParts.stream().map(Message::toByteArray).toList();
        long requestStartedNanos = System.nanoTime();
        long deadline = requestStartedNanos + timeout.toNanos();
        class Attempt implements Runnable {
            @Override
            public void run() {
                if (result.isDone()) {
                    return;
                }
                List<Message> requestParts = payloads.stream().map(Message::from).toList();
                try {
                    boolean submitted = node.entrySpot().requestToSpot(
                        targetNodeRid,
                        targetSpotId,
                        targetSpotGeneration,
                        requestParts,
                        reply -> completeReply(
                            reply,
                            result,
                            requestStartedNanos),
                        SendFlags.NONE,
                        timeout);
                    ZLinkChannelRuntime.trace("spot-route node-submit router=" + routerChannelId
                        + " targetNode=" + targetNodeRid
                        + " targetSpot=" + targetSpotId
                        + " submitted=" + submitted);
                    if (!submitted && System.nanoTime() < deadline) {
                        retryRequest.accept(this);
                    } else if (!submitted) {
                        result.completeExceptionally(new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.INTERNAL_FAILURE,
                            "Spot node router '" + routerChannelId
                                + "' was not ready before the request timeout."));
                    }
                } catch (RuntimeException ex) {
                    ZLinkChannelRuntime.trace("spot-route node-submit-error router="
                        + routerChannelId
                        + " targetNode=" + targetNodeRid
                        + " targetSpot=" + targetSpotId
                        + " error=" + ZLinkChannelRuntime.requestErrorSummary(ex));
                    if (ZLinkChannelRequestSubmitter.isRetriableSubmit(ex)
                        && System.nanoTime() < deadline) {
                        retryRequest.accept(this);
                    } else {
                        result.completeExceptionally(ex);
                    }
                } finally {
                    requestParts.forEach(Message::close);
                }
            }
        }
        new Attempt().run();
        return result;
    }

    private static void completeReply(
        systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived reply,
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
                ZLinkFrameworkErrorKind errorKind = switch (reply.result()) {
                    case NOT_FOUND -> ZLinkFrameworkErrorKind.NOT_FOUND;
                    case NOT_CONNECTED -> ZLinkFrameworkErrorKind.UNAVAILABLE;
                    default -> ZLinkFrameworkErrorKind.INTERNAL_FAILURE;
                };
                result.completeExceptionally(new ZLinkFrameworkException(
                    errorKind,
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
