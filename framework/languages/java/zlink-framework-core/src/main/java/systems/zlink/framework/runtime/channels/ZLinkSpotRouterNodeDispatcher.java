package systems.zlink.framework.runtime.channels;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;

import java.time.Duration;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationRegistry;
import systems.zlink.framework.runtime.messaging.ZLinkFrameworkErrorOrigin;

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
        Duration timeout) {
        CompletableFuture<Void> result = new CompletableFuture<>();
        var entrySpot = node.entrySpot();
        rememberAuthority(
            entrySpot,
            targetNodeRid,
            targetSpotId,
            targetSpotGeneration,
            authorityOwnerGeneration,
            ownerLeaseGeneration);
        entrySpot.sendToSpot(
                targetNodeRid,
                targetSpotId,
                targetSpotGeneration,
                spotParts)
            .whenComplete((ignored, failure) -> {
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
        ZLinkServiceOperationRegistry operations,
        UUID operationId) {
        CompletableFuture<List<Message>> result = new CompletableFuture<>();
        var entrySpot = node.entrySpot();
        rememberAuthority(
            entrySpot,
            targetNodeRid,
            targetSpotId,
            targetSpotGeneration,
            authorityOwnerGeneration,
            ownerLeaseGeneration);
        entrySpot.requestToSpot(
                targetNodeRid,
                targetSpotId,
                targetSpotGeneration,
                new byte[0],
                spotParts,
                timeout,
                operations,
                operationId)
            .whenComplete((reply, failure) -> {
                if (failure == null) {
                    completeReply(reply, result);
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
                    case NOT_CONNECTED, CONFLICT -> ZLinkFrameworkErrorOrigin.framework(
                        ZLinkFrameworkErrorKind.UNAVAILABLE,
                        "SPOT route request failed because the target route is not available.",
                        request);
                    case NOT_FOUND -> ZLinkFrameworkErrorOrigin.framework(
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
        CompletableFuture<List<Message>> result) {
        try {
            if (reply.result() != ZLinkBackendRequestResult.OK) {
                //  A backend request terminal is framework-generated; carry
                //  the origin marker so a NotFound terminal stays usable as
                //  the stale-route control signal.
                result.completeExceptionally(ZLinkFrameworkErrorOrigin.framework(
                    reply.result().toFrameworkErrorKind(reply.failureCode()),
                    "SPOT route request failed: " + reply.result()));
                return;
            }
            List<Message> replyParts = ZLinkChannelRuntime.copyMessages(reply.parts());
            if (ZLinkChannelRuntime.isFrameworkErrorReply(replyParts)) {
                //  Preserve the public kind the framework-error reply carries
                //  rather than collapsing every error reply to InternalFailure,
                //  and keep the reply metadata (framework-origin marker).
                ZLinkFrameworkErrorKind errorKind =
                    ZLinkChannelRuntime.frameworkErrorReplyKind(reply.parts());
                replyParts.forEach(Message::close);
                result.completeExceptionally(new ZLinkFrameworkException(
                    errorKind,
                    ZLinkChannelRuntime.frameworkErrorReplyMessage(reply.parts()),
                    null,
                    ZLinkChannelRuntime.frameworkErrorReplyMetadata(reply.parts())));
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
