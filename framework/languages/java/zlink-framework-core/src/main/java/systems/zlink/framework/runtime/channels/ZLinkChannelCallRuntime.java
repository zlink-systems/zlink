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
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.logging.Level;
import java.util.logging.Logger;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendDealerSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestCallback;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRouterSocket;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;

final class ZLinkChannelCallRuntime {
    private static final Logger LOGGER = Logger.getLogger(ZLinkChannelCallRuntime.class.getName());
    private static final long ROUTE_RETRY_DELAY_MILLIS = 10;
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
    private final ZLinkChannelRequestSubmitter requestSubmitter;
    private final ZLinkChannelReplyDecoder replyDecoder;
    private final SpotSend spotSend;
    private final SpotRequest spotRequest;
    private final ZLinkOneWayCalls oneWayCalls;
    private final Set<CompletableFuture<?>> pendingRequests = ConcurrentHashMap.newKeySet();
    private final ExecutorService oneWayExecutor = Executors.newSingleThreadExecutor(runnable -> {
        Thread thread = new Thread(runnable, "zlink-java-channel-one-way-submit");
        thread.setDaemon(true);
        return thread;
    });

    ZLinkChannelCallRuntime(
        ZLinkMessageFlowTracer flow,
        ScheduledExecutorService timeoutExecutor,
        Duration defaultTimeout,
        ZLinkChannelReplyDecoder replyDecoder,
        SpotSend spotSend,
        SpotRequest spotRequest,
        ZLinkOneWayCalls oneWayCalls) {
        this.flow = flow;
        this.timeoutExecutor = timeoutExecutor;
        this.requestSubmitter = new ZLinkChannelRequestSubmitter(
            timeoutExecutor,
            defaultTimeout);
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
        pendingRequests.add(result);
        var timeoutTask = timeoutExecutor.schedule(
            () -> result.completeExceptionally(
                new TimeoutException("request timed out after " + timeout)),
            timeout.toNanos(),
            TimeUnit.NANOSECONDS);
        result.whenComplete((ignored, error) -> {
            timeoutTask.cancel(false);
            pendingRequests.remove(result);
        });
    }

    void retryRouteRequest(Runnable attempt) {
        timeoutExecutor.schedule(
            attempt,
            ROUTE_RETRY_DELAY_MILLIS,
            TimeUnit.MILLISECONDS);
    }

    void submitClient(
        ZLinkBackendDealerSocket client,
        List<Message> requestParts,
        Duration timeout,
        ZLinkBackendRequestCallback callback,
        CompletableFuture<?> result) {
        requestSubmitter.submitClient(
            client,
            requestParts,
            timeout,
            preserveCurrentFlow(callback),
            result);
    }

    void submitRoute(
        ZLinkBackendRouterSocket router,
        RoutingId target,
        List<Message> requestParts,
        ZLinkBackendRequestCallback callback,
        Duration timeout,
        CompletableFuture<?> result) {
        requestSubmitter.submitRoute(
            router,
            target,
            requestParts,
            preserveCurrentFlow(callback),
            timeout,
            result);
    }

    private static ZLinkBackendRequestCallback preserveCurrentFlow(
        ZLinkBackendRequestCallback callback) {
        ZLinkFlowContext.State captured = ZLinkFlowContext.current();
        if (captured == null) {
            return callback;
        }
        return reply -> {
            try (ZLinkFlowContext.Scope ignored = ZLinkFlowContext.enter(captured)) {
                callback.handle(reply);
            }
        };
    }

    <TReply> void completeReply(
        ZLinkBackendReceived reply,
        Class<TReply> replyType,
        CompletableFuture<TReply> result) {
        if (reply.result() != ZLinkBackendRequestResult.OK) {
            result.completeExceptionally(new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.INTERNAL_FAILURE,
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
        oneWayExecutor.shutdown();
        for (CompletableFuture<?> pending : pendingRequests) {
            pending.completeExceptionally(new ZLinkConfigurationException(
                "channel runtime is closed"));
        }
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
