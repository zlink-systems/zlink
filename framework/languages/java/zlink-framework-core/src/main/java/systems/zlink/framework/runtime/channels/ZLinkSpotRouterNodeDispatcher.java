package systems.zlink.framework.runtime.channels;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.BiConsumer;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkRequestException;
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
        long authorityOwnerGeneration,
        long ownerLeaseGeneration,
        List<Message> spotParts,
        Duration timeout,
        BiConsumer<CompletableFuture<Void>, Duration> trackPendingRequest) {
        CompletableFuture<Void> result = new CompletableFuture<>();
        trackPendingRequest.accept(result, timeout);
        var entrySpot = node.entrySpot();
        rememberAuthority(
            entrySpot,
            targetNodeRid,
            targetSpotId,
            targetSpotGeneration,
            authorityOwnerGeneration,
            ownerLeaseGeneration);
        ZLinkChannelRuntime.trace(
            "spot-route node-send-submit router=" + routerChannelId
                + " targetNode=" + targetNodeRid
                + " targetSpot=" + targetSpotId);
        entrySpot.sendToSpot(
                targetNodeRid,
                targetSpotId,
                targetSpotGeneration,
                spotParts)
            .whenComplete((ignored, failure) -> {
                ZLinkChannelRuntime.trace(
                    "spot-route node-send-result router=" + routerChannelId
                        + " targetNode=" + targetNodeRid
                        + " targetSpot=" + targetSpotId
                        + " result=" + traceResult(failure));
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
        long authorityOwnerGeneration,
        long ownerLeaseGeneration,
        List<Message> spotParts,
        Duration timeout,
        BiConsumer<CompletableFuture<List<Message>>, Duration> trackPendingRequest) {
        CompletableFuture<List<Message>> result = new CompletableFuture<>();
        trackPendingRequest.accept(result, timeout);
        long requestStartedNanos = System.nanoTime();
        var entrySpot = node.entrySpot();
        rememberAuthority(
            entrySpot,
            targetNodeRid,
            targetSpotId,
            targetSpotGeneration,
            authorityOwnerGeneration,
            ownerLeaseGeneration);
        ZLinkChannelRuntime.trace(
            "spot-route node-request-submit router=" + routerChannelId
                + " targetNode=" + targetNodeRid
                + " targetSpot=" + targetSpotId);
        entrySpot.requestToSpot(
                targetNodeRid,
                targetSpotId,
                targetSpotGeneration,
                spotParts,
                timeout)
            .whenComplete((reply, failure) -> {
                ZLinkChannelRuntime.trace(
                    "spot-route node-request-result router=" + routerChannelId
                        + " targetNode=" + targetNodeRid
                        + " targetSpot=" + targetSpotId
                        + " elapsedMs="
                        + ZLinkChannelRuntime.elapsedMillis(requestStartedNanos)
                        + " result=" + requestTraceResult(reply, failure));
                if (failure == null) {
                    completeReply(reply, result, requestStartedNanos);
                } else {
                    result.completeExceptionally(classifyTransportFailure(failure));
                }
            });
        return result;
    }

    private static void rememberAuthority(
        systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot entrySpot,
        RoutingId targetNodeRid,
        String targetSpotId,
        long targetSpotGeneration,
        long authorityOwnerGeneration,
        long ownerLeaseGeneration) {
        if (targetSpotGeneration > 0
            && authorityOwnerGeneration > 0
            && ownerLeaseGeneration > 0) {
            entrySpot.rememberSpotAuthority(
                targetNodeRid,
                targetSpotId,
                targetSpotGeneration,
                authorityOwnerGeneration,
                ownerLeaseGeneration);
        }
    }

    private static String traceResult(Throwable failure) {
        if (failure == null) {
            return "accepted";
        }
        Throwable current = failure;
        while ((current instanceof java.util.concurrent.CompletionException
                || current instanceof java.util.concurrent.ExecutionException)
            && current.getCause() != null) {
            current = current.getCause();
        }
        if (current instanceof ZlinkRequestException request) {
            return "rejected requestResult=" + request.getResult()
                + " nativeErrno=" + request.getNativeErrno();
        }
        return "failed error=" + current.getClass().getSimpleName()
            + ":" + String.valueOf(current.getMessage());
    }

    private static String requestTraceResult(
        ZLinkBackendReceived reply,
        Throwable failure) {
        if (failure != null) {
            return traceResult(failure);
        }
        return "completed backendResult=" + reply.result()
            + " failureCode=" + reply.failureCode();
    }

    /**
     * Spec 32:58,87 — a Framework failure discovered while waiting on route
     * resolution or the remote reply is delivered as a typed Framework
     * exception; a route or connection that is not available is
     * {@code Unavailable}. The binding's local preflight throws the raw
     * {@link ZlinkRequestException}; keep it as the cause.
     */
    private static Throwable classifyTransportFailure(Throwable failure) {
        Throwable current = failure;
        while (current != null) {
            if (current instanceof ZLinkFrameworkException) {
                return failure;
            }
            if (current instanceof ZlinkRequestException request) {
                return switch (request.getResult()) {
                    case NOT_CONNECTED, CONFLICT -> new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.UNAVAILABLE,
                        "SPOT route request failed because the target route is not available.",
                        request);
                    case NOT_FOUND -> new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.NOT_FOUND,
                        "SPOT route request failed because the target route was not found.",
                        request);
                    default -> failure;
                };
            }
            current = current.getCause();
        }
        return failure;
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
                    reply.result().toFrameworkErrorKind(reply.failureCode()),
                    "SPOT route request failed: " + reply.result()));
                return;
            }
            List<Message> replyParts = ZLinkChannelRuntime.copyMessages(reply.parts());
            if (ZLinkChannelRuntime.isFrameworkErrorReply(replyParts)) {
                //  Preserve the public kind the framework-error reply carries
                //  rather than collapsing every error reply to InternalFailure.
                ZLinkFrameworkErrorKind errorKind =
                    ZLinkChannelRuntime.frameworkErrorReplyKind(reply.parts());
                replyParts.forEach(Message::close);
                result.completeExceptionally(new ZLinkFrameworkException(
                    errorKind,
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
