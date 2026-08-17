package systems.zlink.framework.runtime.channels;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotRouteBridge;

final class ZLinkSpotRouteBridgeDispatcher {
    private ZLinkSpotRouteBridgeDispatcher() {
    }

    static void submitSend(
        ZLinkBackendSpotRouteBridge bridge,
        String routerChannelId,
        RoutingId targetNodeRid,
        String targetSpotId,
        List<Message> requestParts,
        CompletableFuture<Void> result) {
        try {
            bridge.send(
                    routerChannelId,
                    targetNodeRid,
                    targetSpotId,
                    requestParts)
                .whenComplete((ignored, failure) -> {
                    requestParts.forEach(Message::close);
                    if (failure == null) {
                        result.complete(null);
                    } else {
                        result.completeExceptionally(
                            ZLinkChannelCallRuntime.unwrap(failure));
                    }
                });
        } catch (RuntimeException failure) {
            requestParts.forEach(Message::close);
            result.completeExceptionally(failure);
        }
    }

    static void submitRequest(
        ZLinkBackendSpotRouteBridge bridge,
        String routerChannelId,
        RoutingId targetNodeRid,
        String targetSpotId,
        List<Message> requestParts,
        Duration timeout,
        CompletableFuture<List<Message>> result) {
        try {
            bridge.request(
                    routerChannelId,
                    targetNodeRid,
                    targetSpotId,
                    requestParts,
                    timeout)
                .whenComplete((reply, failure) -> {
                    requestParts.forEach(Message::close);
                    if (failure != null) {
                        result.completeExceptionally(
                            ZLinkChannelCallRuntime.unwrap(failure));
                        return;
                    }
                    if (result.isDone()) {
                        reply.forEach(Message::close);
                        return;
                    }
                    try {
                        ZLinkChannelRuntime.trace(ZLinkChannelRuntime.traceEnabled() ?
                            "spot-route bridge-reply router=" + routerChannelId
                                + " targetNode=" + targetNodeRid
                                + " targetSpot=" + targetSpotId
                                + " parts="
                                + ZLinkChannelRuntime.describeTraceParts(reply) : null);
                        if (ZLinkChannelRuntime.isFrameworkErrorReply(reply)) {
                            result.completeExceptionally(
                                new ZLinkFrameworkException(
                                    ZLinkChannelRuntime.frameworkErrorReplyKind(reply),
                                    ZLinkChannelRuntime.frameworkErrorReplyMessage(reply)));
                            return;
                        }
                        List<Message> normalizedReply = copyReplyMessages(reply);
                        if (!result.complete(normalizedReply)) {
                            normalizedReply.forEach(Message::close);
                        }
                    } finally {
                        reply.forEach(Message::close);
                    }
                });
        } catch (RuntimeException failure) {
            requestParts.forEach(Message::close);
            result.completeExceptionally(failure);
        }
    }

    private static List<Message> copyReplyMessages(List<Message> parts) {
        int payloadOffset = 0;
        if (ZLinkChannelRuntime.looksLikeSpotRouteBridgePacket(parts)) {
            payloadOffset = 1;
        }
        if (parts.size() - payloadOffset > 1) {
            payloadOffset++;
        }
        if (payloadOffset > 0 && payloadOffset < parts.size()) {
            return ZLinkChannelRuntime.copyMessages(
                parts.subList(payloadOffset, parts.size()));
        }
        return ZLinkChannelRuntime.copyMessages(parts);
    }
}
