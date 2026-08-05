package systems.zlink.framework.runtime.channels;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.Executor;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotRouteBridge;

final class ZLinkSpotRouteBridgeDispatcher {
    private static final Duration RETRY_DELAY = Duration.ofMillis(10);

    private ZLinkSpotRouteBridgeDispatcher() {
    }

    static void submitSendWithRetry(
        ZLinkBackendSpotRouteBridge bridge,
        String routerChannelId,
        RoutingId targetNodeRid,
        String targetSpotId,
        List<byte[]> payloads,
        Duration timeout,
        ScheduledExecutorService scheduler,
        CompletableFuture<Void> result) {
        long timeoutNanos = timeout.toNanos();
        long deadline = System.nanoTime() + timeoutNanos;
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
                    boolean submitted = bridge.send(
                        routerChannelId,
                        targetNodeRid,
                        targetSpotId,
                        attemptParts,
                        SendFlags.DONT_WAIT);
                    if (submitted) {
                        result.complete(null);
                        return;
                    }
                    if (System.nanoTime() >= deadline) {
                        result.completeExceptionally(new TimeoutException(
                            "routed SPOT route mesh send was not ready before timeout"));
                        return;
                    }
                    scheduler.schedule(this, RETRY_DELAY.toMillis(), TimeUnit.MILLISECONDS);
                } catch (RuntimeException ex) {
                    result.completeExceptionally(ex);
                } finally {
                    attemptParts.forEach(Message::close);
                }
            }
        }
        new Attempt().run();
    }

    static void submitRequest(
        ZLinkBackendSpotRouteBridge bridge,
        String routerChannelId,
        RoutingId targetNodeRid,
        String targetSpotId,
        List<Message> requestParts,
        Duration timeout,
        Executor executor,
        ZLinkSpotRouteBridgeRawReplies rawReplies,
        CompletableFuture<List<Message>> result) {
        ZLinkSpotRouteBridgeRawReplies.Pending rawReply = rawReplies.enqueue(
            routerChannelId,
            targetNodeRid,
            requestParts.stream().map(Message::toByteArray).toList(),
            result,
            timeout);
        executor.execute(() -> {
            try {
                bridge.requestAsync(
                        routerChannelId,
                        targetNodeRid,
                        targetSpotId,
                        requestParts,
                        SendFlags.NONE,
                        timeout)
                    .whenComplete((reply, error) -> {
                    if (error != null) {
                        ZLinkChannelRuntime.trace("spot-route bridge-reply-error router=" + routerChannelId
                            + " targetNode=" + targetNodeRid
                            + " targetSpot=" + targetSpotId
                            + " error=" + ZLinkChannelRuntime.requestErrorSummary(error));
                        result.completeExceptionally(error);
                        return;
                    }
                    try {
                        ZLinkChannelRuntime.trace("spot-route bridge-reply router=" + routerChannelId
                            + " targetNode=" + targetNodeRid
                            + " targetSpot=" + targetSpotId
                            + " parts=" + ZLinkChannelRuntime.describeTraceParts(reply));
                        if (ZLinkSpotRouteBridgeRawReplies.isEcho(rawReply, reply)) {
                            return;
                        }
                        if (ZLinkChannelRuntime.isFrameworkErrorReply(reply)) {
                            result.completeExceptionally(new ZLinkFrameworkException(
                                ZLinkChannelRuntime.frameworkErrorReplyKind(reply),
                                ZLinkChannelRuntime.frameworkErrorReplyMessage(reply)));
                            return;
                        }
                        List<Message> normalizedReply =
                            ZLinkSpotRouteBridgeRawReplies.copyReplyMessages(reply);
                        if (ZLinkSpotRouteBridgeRawReplies.isEcho(rawReply, normalizedReply)) {
                            normalizedReply.forEach(Message::close);
                            return;
                        }
                        if (!result.complete(normalizedReply)) {
                            normalizedReply.forEach(Message::close);
                        }
                    } finally {
                        reply.forEach(Message::close);
                    }
                });
            } catch (RuntimeException ex) {
                ZLinkChannelRuntime.trace("spot-route bridge-submit-error router=" + routerChannelId
                    + " targetNode=" + targetNodeRid
                    + " targetSpot=" + targetSpotId
                    + " error=" + ex);
                result.completeExceptionally(ex);
            } finally {
                requestParts.forEach(Message::close);
            }
        });
    }
}
