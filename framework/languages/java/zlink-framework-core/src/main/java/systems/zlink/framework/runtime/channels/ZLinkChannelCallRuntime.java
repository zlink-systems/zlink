package systems.zlink.framework.runtime.channels;

import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeoutException;
import java.util.function.Consumer;
import java.util.function.LongSupplier;
import java.util.function.Supplier;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendDealerSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRouterSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationRegistry;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationIds;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;
import systems.zlink.framework.runtime.messaging.ZLinkFrameworkErrorOrigin;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;

final class ZLinkChannelCallRuntime {
    @FunctionalInterface
    interface SpotSend {
        CompletionStage<Void> send(
            String channelName,
            RoutingId targetNode,
            String targetSpot,
            long targetSpotGeneration,
            long authorityOwnerGeneration,
            long ownerLeaseGeneration,
            List<Message> parts);
    }

    @FunctionalInterface
    interface SpotRequest {
        CompletionStage<List<Message>> request(
            String channelName,
            RoutingId targetNode,
            String targetSpot,
            long targetSpotGeneration,
            long authorityOwnerGeneration,
            long ownerLeaseGeneration,
            List<Message> parts,
            Duration timeout,
            ZLinkServiceOperationRegistry operations,
            UUID operationId);
    }

    private final ZLinkMessageFlowTracer flow;
    private final ZLinkServiceOperationRegistry operations;
    private final LongSupplier nanoTime;
    private final ZLinkChannelReplyDecoder replyDecoder;
    private final SpotSend spotSend;
    private final SpotRequest spotRequest;

    ZLinkChannelCallRuntime(
        ZLinkMessageFlowTracer flow,
        ScheduledExecutorService timeoutExecutor,
        ZLinkChannelReplyDecoder replyDecoder,
        SpotSend spotSend,
        SpotRequest spotRequest) {
        this(flow, timeoutExecutor, replyDecoder, spotSend, spotRequest, System::nanoTime);
    }

    ZLinkChannelCallRuntime(
        ZLinkMessageFlowTracer flow,
        ScheduledExecutorService timeoutExecutor,
        ZLinkChannelReplyDecoder replyDecoder,
        SpotSend spotSend,
        SpotRequest spotRequest,
        LongSupplier nanoTime) {
        this.flow = flow;
        this.operations = new ZLinkServiceOperationRegistry(
            timeoutExecutor, ZLinkServiceOperationRegistry.DEFAULT_MAX_PENDING_OPERATIONS,
            closedFailure(), () -> requestFailure(
                new TimeoutException("service operation timed out")), nanoTime);
        this.nanoTime = nanoTime;
        this.replyDecoder = replyDecoder;
        this.spotSend = spotSend;
        this.spotRequest = spotRequest;
    }

    long nanoTime() {
        return nanoTime.getAsLong();
    }

    ZLinkMessageFlowTracer flow() {
        return flow;
    }

    ZLinkFlowContext.Scope enterApplicationFlow() {
        return ZLinkFlowContext.enterCurrentOrCreate(
            ZLinkFlowOrigin.APPLICATION,
            flow.captureEnabled());
    }

    <T> CompletableFuture<T> submit(
        Duration timeout,
        Supplier<? extends CompletionStage<T>> submission,
        Consumer<? super T> discardValue) {
        return submit(ZLinkServiceOperationIds.next(), timeout, submission, discardValue);
    }

    private <T> CompletableFuture<T> submit(
        UUID operationId,
        Duration timeout,
        Supplier<? extends CompletionStage<T>> submission,
        Consumer<? super T> discardValue) {
        try {
            return operations.submit(operationId, timeout, submission, discardValue);
        } catch (RuntimeException failure) {
            return CompletableFuture.failedFuture(failure);
        }
    }

    CompletionStage<ZLinkBackendReceived> requestClient(
        ZLinkBackendDealerSocket client,
        List<Message> requestParts,
        Duration timeout) {
        return preserveCurrentFlow(submit(
            ZLinkServiceOperationIds.next(), timeout,
            () -> client.request(requestParts, timeout), ZLinkBackendReceived::close));
    }

    CompletionStage<ZLinkBackendReceived> requestRoute(
        UUID operationId,
        ZLinkBackendRouterSocket router,
        RoutingId target,
        List<Message> requestParts,
        Duration timeout) {
        return preserveCurrentFlow(submit(
            operationId, timeout,
            () -> router.request(target, requestParts, timeout),
            ZLinkBackendReceived::close));
    }

    CompletionStage<ZLinkBackendReceived> requestChannel(
        UUID operationId,
        ZLinkInternalSpotNode node,
        String channelName,
        byte[] metadata,
        List<Message> requestParts,
        Duration timeout) {
        if (operations.isClosed()) {
            return CompletableFuture.failedFuture(closedFailure());
        }
        try {
            return preserveCurrentFlow(node.requestToChannel(
                channelName, metadata, requestParts, timeout, operations, operationId));
        } catch (RuntimeException failure) {
            return CompletableFuture.failedFuture(failure);
        }
    }

    CompletionStage<ZLinkBackendReceived> requestNode(
        UUID operationId,
        ZLinkInternalSpotNode node,
        RoutingId target,
        byte[] metadata,
        List<Message> requestParts,
        Duration timeout) {
        if (operations.isClosed()) {
            return CompletableFuture.failedFuture(closedFailure());
        }
        try {
            return preserveCurrentFlow(node.requestToNode(
                target, metadata, requestParts, timeout, operations, operationId));
        } catch (RuntimeException failure) {
            return CompletableFuture.failedFuture(failure);
        }
    }

    static Throwable requestFailure(Throwable failure) {
        Throwable cause = unwrap(failure);
        if (cause instanceof TimeoutException) {
            return new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                cause.getMessage(), cause);
        }
        return cause;
    }

    static Throwable unwrap(Throwable failure) {
        Throwable current = failure;
        while (current instanceof java.util.concurrent.CompletionException
            && current.getCause() != null) {
            current = current.getCause();
        }
        return current;
    }

    static <T> CompletionStage<T> preserveCurrentFlow(
        CompletionStage<T> request) {
        ZLinkFlowContext.State captured = ZLinkFlowContext.current();
        if (captured == null) {
            return request;
        }
        CompletableFuture<T> contextual = new CompletableFuture<>();
        request.whenComplete((reply, failure) -> {
            try (ZLinkFlowContext.Scope ignored = ZLinkFlowContext.enter(captured)) {
                if (failure == null) {
                    contextual.complete(reply);
                } else {
                    contextual.completeExceptionally(unwrap(failure));
                }
            }
        });
        return contextual;
    }

    <TReply> void completeReply(
        ZLinkBackendReceived reply,
        Class<TReply> replyType,
        CompletableFuture<TReply> result) {
        if (reply.result() != ZLinkBackendRequestResult.OK) {
            //  A backend request terminal is framework-generated; carry the
            //  origin marker (zlink.origin=framework).
            result.completeExceptionally(ZLinkFrameworkErrorOrigin.framework(
                reply.result().toFrameworkErrorKind(reply.failureCode()),
                "channel request failed: " + reply.result()));
            return;
        }
        if (ZLinkChannelRuntime.isFrameworkErrorReply(reply.parts())) {
            result.completeExceptionally(new ZLinkFrameworkException(
                ZLinkChannelRuntime.frameworkErrorReplyKind(reply.parts()),
                ZLinkChannelRuntime.frameworkErrorReplyMessage(reply.parts()),
                null,
                ZLinkChannelRuntime.frameworkErrorReplyMetadata(reply.parts())));
            return;
        }
        result.complete(replyDecoder.decode(
            reply.parts(),
            replyType,
            "route mesh reply decode failed"));
    }

    <TReply> TReply decodeSpotReply(List<Message> replies, Class<TReply> replyType) {
        return replyDecoder.decode(
            replies,
            replyType,
            "route mesh SPOT reply decode failed");
    }

    CompletionStage<Void> sendToSpot(
        String channelName,
        RoutingId targetNode,
        String targetSpot,
        long targetSpotGeneration,
        long authorityOwnerGeneration,
        long ownerLeaseGeneration,
        List<Message> parts) {
        return spotSend.send(
            channelName,
            targetNode,
            targetSpot,
            targetSpotGeneration,
            authorityOwnerGeneration,
            ownerLeaseGeneration,
            parts);
    }

    CompletionStage<List<Message>> requestToSpot(
        String channelName,
        RoutingId targetNode,
        String targetSpot,
        long targetSpotGeneration,
        long authorityOwnerGeneration,
        long ownerLeaseGeneration,
        List<Message> parts,
        Duration timeout) {
        return requestToSpot(
            channelName, targetNode, targetSpot, targetSpotGeneration,
            authorityOwnerGeneration, ownerLeaseGeneration, parts, timeout,
            ZLinkServiceOperationIds.next());
    }

    CompletionStage<List<Message>> requestToSpot(
        String channelName,
        RoutingId targetNode,
        String targetSpot,
        long targetSpotGeneration,
        long authorityOwnerGeneration,
        long ownerLeaseGeneration,
        List<Message> parts,
        Duration timeout,
        UUID operationId) {
        if (operations.isClosed()) {
            return CompletableFuture.failedFuture(closedFailure());
        }
        try {
            return spotRequest.request(
                channelName,
                targetNode,
                targetSpot,
                targetSpotGeneration,
                authorityOwnerGeneration,
                ownerLeaseGeneration,
                parts,
                timeout,
                operations,
                operationId);
        } catch (RuntimeException failure) {
            return CompletableFuture.failedFuture(failure);
        }
    }

    void beginClose() {
        operations.close();
    }

    private static ZLinkFrameworkException closedFailure() {
        //  Spec 32-framework-error-model — a request settled because the channel
        //  runtime is closing/closed is ShuttingDown, not a NotConfigured
        //  configuration error.
        return new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.SHUTTING_DOWN,
            "channel runtime is closed");
    }

    static List<Message> parts(Optional<String> packetName, Message payload) {
        return parts(
            packetName,
            payload,
            ZLinkChannelContentTypeFrame.DEFAULT_CONTENT_TYPE);
    }

    static List<Message> parts(
        Optional<String> packetName,
        Message payload,
        String contentType) {
        if (packetName.isEmpty()) {
            return List.of(payload);
        }
        Message flow = ZLinkChannelFlowFrame.current();
        Message packet = Message.from(packetName.get().getBytes(StandardCharsets.UTF_8));
        if (ZLinkChannelContentTypeFrame.DEFAULT_CONTENT_TYPE.equalsIgnoreCase(contentType)) {
            return flow == null
                ? List.of(packet, payload)
                : List.of(packet, payload, flow);
        }
        Message contentTypeFrame = ZLinkChannelContentTypeFrame.encode(contentType);
        return flow == null
            ? List.of(packet, payload, contentTypeFrame)
            : List.of(packet, payload, contentTypeFrame, flow);
    }

    static List<Message> copyParts(
        Optional<String> packetName,
        Message payload,
        String contentType) {
        List<Message> source = parts(packetName, payload, contentType);
        try {
            return ZLinkChannelRuntime.copyMessages(source);
        } finally {
            for (Message part : source) {
                if (part != payload) {
                    part.close();
                }
            }
        }
    }

    /**
     * Shared cross-language envelope frame for SPOT route and route mesh
     * calls: {@code [JSON header, payload]} with content type, application
     * metadata and the ambient flow pair carried as header fields. A call
     * without a packet name stays a bare single-part payload.
     */
    static List<Message> envelopeParts(
        int kind,
        String channelName,
        Optional<String> packetName,
        Message payload,
        String contentType,
        java.util.Map<String, String> metadata) {
        return envelopeParts(kind, channelName, packetName, payload, contentType,
            metadata, kind == systems.zlink.framework.runtime.messaging
                .ZLinkChannelEnvelope.KIND_REQUEST ? ZLinkServiceOperationIds.next() : null);
    }

    static List<Message> envelopeParts(
        int kind,
        String channelName,
        Optional<String> packetName,
        Message payload,
        String contentType,
        Map<String, String> metadata,
        UUID operationId) {
        if (packetName.isEmpty()) {
            return List.of(payload);
        }
        return systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.encode(
            systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.create(
                kind, channelName, packetName.orElseThrow(), contentType, null,
                metadata, ZLinkFlowContext.current(), operationId), payload);
    }

    static List<Message> copyEnvelopeParts(
        int kind,
        String channelName,
        Optional<String> packetName,
        Message payload,
        String contentType,
        java.util.Map<String, String> metadata) {
        return copyEnvelopeParts(
            kind, channelName, packetName, payload, contentType, metadata,
            kind == systems.zlink.framework.runtime.messaging
                .ZLinkChannelEnvelope.KIND_REQUEST ? ZLinkServiceOperationIds.next() : null);
    }

    static List<Message> copyEnvelopeParts(
        int kind,
        String channelName,
        Optional<String> packetName,
        Message payload,
        String contentType,
        Map<String, String> metadata,
        UUID operationId) {
        List<Message> source = envelopeParts(
            kind, channelName, packetName, payload, contentType, metadata, operationId);
        try {
            return ZLinkChannelRuntime.copyMessages(source);
        } finally {
            for (Message part : source) {
                if (part != payload) {
                    part.close();
                }
            }
        }
    }

}
