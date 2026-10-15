package systems.zlink.framework.runtime.channels;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotRouteBridge;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;

final class ZLinkSpotRouteBridgeDispatcher {
    private ZLinkSpotRouteBridgeDispatcher() {}

    static void submitSend(
            ZLinkBackendSpotRouteBridge bridge,
            String routerChannelId,
            RoutingId targetNodeRid,
            String targetSpotId,
            List<Message> requestParts,
            CompletableFuture<Void> result) {
        try {
            bridge.send(routerChannelId, targetNodeRid, targetSpotId, requestParts)
                    .whenComplete(
                            (ignored, failure) -> {
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
        submitRequest(
                bridge,
                routerChannelId,
                targetNodeRid,
                targetSpotId,
                requestParts,
                timeout,
                result,
                null);
    }

    static void submitRequest(
            ZLinkBackendSpotRouteBridge bridge,
            String routerChannelId,
            RoutingId targetNodeRid,
            String targetSpotId,
            List<Message> requestParts,
            Duration timeout,
            CompletableFuture<List<Message>> result,
            CompletableFuture<Void> requestAdmission) {
        try {
            bridge.request(
                            routerChannelId,
                            targetNodeRid,
                            targetSpotId,
                            requestParts,
                            timeout,
                            requestAdmission)
                    .whenComplete(
                            (reply, failure) -> {
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
                                    //  Keep the reply metadata: the framework-origin
                                    //  marker distinguishes a framework NotFound
                                    //  (stale route) from an application NotFound.
                                    ZLinkFrameworkException errorReply =
                                            ZLinkChannelRuntime.frameworkErrorReplyFailure(reply);
                                    if (errorReply != null) {
                                        result.completeExceptionally(errorReply);
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
            return ZLinkChannelRuntime.copyMessages(parts.subList(payloadOffset, parts.size()));
        }
        return ZLinkChannelRuntime.copyMessages(parts);
    }
}
