package systems.zlink.framework.runtime.channels;

import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;

import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.List;
import java.util.Optional;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.logging.Level;
import java.util.logging.Logger;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendDealerSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRouterSocket;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;

final class ZLinkChannelCallRuntime {
    private static final Logger LOGGER = Logger.getLogger(ZLinkChannelCallRuntime.class.getName());
    @FunctionalInterface
    interface SpotSend {
        CompletionStage<Void> send(
            String channelName,
            RoutingId targetNode,
            String targetSpot,
            long targetSpotGeneration,
            List<Message> parts);
    }

    @FunctionalInterface
    interface SpotRequest {
        CompletionStage<List<Message>> request(
            String channelName,
            RoutingId targetNode,
            String targetSpot,
            long targetSpotGeneration,
            List<Message> parts,
            Duration timeout);
    }

    private final ZLinkMessageFlowTracer flow;
    private final ScheduledExecutorService timeoutExecutor;
    private final ZLinkChannelReplyDecoder replyDecoder;
    private final SpotSend spotSend;
    private final SpotRequest spotRequest;
    private final ZLinkOneWayCalls oneWayCalls;
    private final Set<CompletableFuture<?>> pendingRequests = ConcurrentHashMap.newKeySet();
    private final AtomicBoolean closing = new AtomicBoolean();
    private final ExecutorService oneWayExecutor = Executors.newSingleThreadExecutor(runnable -> {
        Thread thread = new Thread(runnable, "zlink-java-channel-one-way-submit");
        thread.setDaemon(true);
        return thread;
    });

    ZLinkChannelCallRuntime(
        ZLinkMessageFlowTracer flow,
        ScheduledExecutorService timeoutExecutor,
        ZLinkChannelReplyDecoder replyDecoder,
        SpotSend spotSend,
        SpotRequest spotRequest,
        ZLinkOneWayCalls oneWayCalls) {
        this.flow = flow;
        this.timeoutExecutor = timeoutExecutor;
        this.replyDecoder = replyDecoder;
        this.spotSend = spotSend;
        this.spotRequest = spotRequest;
        this.oneWayCalls = oneWayCalls;
    }

    ZLinkOneWayCalls oneWayCalls() {
        return oneWayCalls;
    }

    ZLinkMessageFlowTracer flow() {
        return flow;
    }

    void settleOneWay(CompletableFuture<Void> submission) {
        pendingRequests.add(submission);
        submission.whenComplete((ignored, error) -> {
            pendingRequests.remove(submission);
            if (error != null) {
                LOGGER.log(Level.SEVERE, "one-way channel submission failed", error);
            }
        });
    }

    void submitOneWay(Runnable submission) {
        settleOneWay(CompletableFuture.runAsync(submission, oneWayExecutor));
    }

    void track(CompletableFuture<?> result, Duration timeout) {
        if (closing.get()) {
            result.completeExceptionally(closedFailure());
            return;
        }
        pendingRequests.add(result);
        if (closing.get()) {
            pendingRequests.remove(result);
            result.completeExceptionally(closedFailure());
            return;
        }
        final java.util.concurrent.ScheduledFuture<?> timeoutTask;
        try {
            timeoutTask = timeoutExecutor.schedule(
                () -> {
                    String message = "request timed out after " + timeout;
                    //  Spec 32-framework-error-model:90 — a reply not received
                    //  within the deadline is DeadlineExceeded, not a raw
                    //  language timeout. The TimeoutException cause is retained
                    //  so timeout metrics tagging still recognises it.
                    result.completeExceptionally(new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                        message,
                        new TimeoutException(message)));
                },
                timeout.toNanos(),
                TimeUnit.NANOSECONDS);
        } catch (RejectedExecutionException rejection) {
            pendingRequests.remove(result);
            result.completeExceptionally(shuttingDownFailure(rejection));
            return;
        }
        result.whenComplete((ignored, error) -> {
            timeoutTask.cancel(false);
            pendingRequests.remove(result);
        });
    }

    CompletionStage<ZLinkBackendReceived> requestClient(
        ZLinkBackendDealerSocket client,
        List<Message> requestParts,
        Duration timeout) {
        return preserveCurrentFlow(client.request(requestParts, timeout));
    }

    CompletionStage<ZLinkBackendReceived> requestRoute(
        ZLinkBackendRouterSocket router,
        RoutingId target,
        List<Message> requestParts,
        Duration timeout) {
        return preserveCurrentFlow(
            router.request(target, requestParts, timeout));
    }

    static Throwable unwrap(Throwable failure) {
        Throwable current = failure;
        while (current instanceof java.util.concurrent.CompletionException
            && current.getCause() != null) {
            current = current.getCause();
        }
        return current;
    }

    private static <T> CompletionStage<T> preserveCurrentFlow(
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
            result.completeExceptionally(new ZLinkFrameworkException(
                reply.result().toFrameworkErrorKind(),
                "channel request failed: " + reply.result()));
            return;
        }
        if (ZLinkChannelRuntime.isFrameworkErrorReply(reply.parts())) {
            result.completeExceptionally(new ZLinkFrameworkException(
                ZLinkChannelRuntime.frameworkErrorReplyKind(reply.parts()),
                ZLinkChannelRuntime.frameworkErrorReplyMessage(reply.parts())));
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
        List<Message> parts) {
        return spotSend.send(
            channelName, targetNode, targetSpot, targetSpotGeneration, parts);
    }

    CompletionStage<List<Message>> requestToSpot(
        String channelName,
        RoutingId targetNode,
        String targetSpot,
        long targetSpotGeneration,
        List<Message> parts,
        Duration timeout) {
        return spotRequest.request(
            channelName,
            targetNode,
            targetSpot,
            targetSpotGeneration,
            parts,
            timeout);
    }

    void beginClose() {
        closing.set(true);
        oneWayExecutor.shutdown();
        for (CompletableFuture<?> pending : pendingRequests) {
            pending.completeExceptionally(closedFailure());
        }
    }

    private static ZLinkFrameworkException closedFailure() {
        //  Spec 32-framework-error-model — a request settled because the channel
        //  runtime is closing/closed is ShuttingDown, not a NotConfigured
        //  configuration error.
        return new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.SHUTTING_DOWN,
            "channel runtime is closed");
    }

    static ZLinkFrameworkException shuttingDownFailure(Throwable cause) {
        return new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.SHUTTING_DOWN,
            "channel runtime is closed",
            cause);
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
            source.stream()
                .filter(part -> part != payload)
                .forEach(Message::close);
        }
    }

}
