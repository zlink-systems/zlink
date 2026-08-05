package systems.zlink.framework.runtime.streams;

import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;

import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.Supplier;
import java.util.concurrent.ConcurrentHashMap;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.actors.ZLinkSessionActorsRuntime;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendStreamSocket;
import systems.zlink.framework.runtime.internal.streams.ZLinkStreamErrorPayload;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionActors;
import systems.zlink.framework.streams.ZLinkSessionClient;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamCompressionCodec;

final class ZLinkStreamSessionContextState implements ZLinkSessionContext {
    private static final long ASYNC_REPLY_TIMEOUT_NANOS = TimeUnit.SECONDS.toNanos(30);

    private final String streamNodeName;
    private final ZLinkBackendStreamSocket stream;
    private final RoutingId routingId;
    private final ZLinkSessionActors actors;
    private final ZLinkMessageSerializer serializer;
    private final ZLinkStreamCodec defaultCodec;
    private final ZLinkStreamCompressionCodec compressionCodec;
    private final ZLinkMessageFlowTracer flow;
    private final Supplier<CompletionStage<Void>> closeAction;
    private final ZLinkOneWayCalls oneWayCalls;
    private final ScheduledExecutorService replyRetryExecutor;
    private final ConcurrentHashMap<String, ZLinkStreamHeader> requestHeadersByFlow =
        new ConcurrentHashMap<>();
    private final ConcurrentHashMap<ZLinkStreamHeader, Boolean> claimedReplyHeaders =
        new ConcurrentHashMap<>();
    private final java.util.Set<ReplyAttempt> replyAttempts =
        ConcurrentHashMap.newKeySet();
    private final AtomicBoolean replyRetriesClosed = new AtomicBoolean();

    ZLinkStreamSessionContextState(
        String streamNodeName,
        ZLinkBackendStreamSocket stream,
        RoutingId routingId,
        ZLinkSessionActors actors,
        ZLinkMessageSerializer serializer,
        ZLinkStreamCodec defaultCodec,
        ZLinkStreamCompressionCodec compressionCodec,
        ZLinkMessageFlowTracer flow,
        Supplier<CompletionStage<Void>> closeAction) {
        this(
            streamNodeName,
            stream,
            routingId,
            actors,
            serializer,
            defaultCodec,
            compressionCodec,
            flow,
            closeAction,
            new ZLinkOneWayCalls((backend, key) -> (submission, cleanup) -> {
                try {
                    return submission.get()
                        ? CompletableFuture.completedFuture(null)
                        : CompletableFuture.failedFuture(new IllegalStateException(
                            "one-way submission was not admitted"));
                } finally {
                    cleanup.run();
                }
            }),
            null);
    }

    ZLinkStreamSessionContextState(
        String streamNodeName,
        ZLinkBackendStreamSocket stream,
        RoutingId routingId,
        ZLinkSessionActors actors,
        ZLinkMessageSerializer serializer,
        ZLinkStreamCodec defaultCodec,
        ZLinkStreamCompressionCodec compressionCodec,
        ZLinkMessageFlowTracer flow,
        Supplier<CompletionStage<Void>> closeAction,
        ZLinkOneWayCalls oneWayCalls) {
        this(
            streamNodeName,
            stream,
            routingId,
            actors,
            serializer,
            defaultCodec,
            compressionCodec,
            flow,
            closeAction,
            oneWayCalls,
            null);
    }

    ZLinkStreamSessionContextState(
        String streamNodeName,
        ZLinkBackendStreamSocket stream,
        RoutingId routingId,
        ZLinkSessionActors actors,
        ZLinkMessageSerializer serializer,
        ZLinkStreamCodec defaultCodec,
        ZLinkStreamCompressionCodec compressionCodec,
        ZLinkMessageFlowTracer flow,
        Supplier<CompletionStage<Void>> closeAction,
        ZLinkOneWayCalls oneWayCalls,
        ScheduledExecutorService replyRetryExecutor) {
        this.streamNodeName = streamNodeName;
        this.stream = stream;
        this.routingId = routingId;
        this.actors = actors;
        this.serializer = serializer;
        this.defaultCodec = defaultCodec;
        this.compressionCodec = compressionCodec;
        this.flow = flow;
        this.closeAction = java.util.Objects.requireNonNull(closeAction, "closeAction");
        this.oneWayCalls = java.util.Objects.requireNonNull(oneWayCalls, "oneWayCalls");
        this.replyRetryExecutor = replyRetryExecutor;
    }

    @Override
    public String sessionId() {
        return streamNodeName + ":" + routingId;
    }

    @Override
    public Optional<RoutingId> routingId() {
        return Optional.of(routingId);
    }

    @Override
    public Optional<String> localAddr() {
        return Optional.empty();
    }

    @Override
    public Optional<String> remoteAddr() {
        return Optional.empty();
    }

    @Override
    public ZLinkSessionClient client() {
        return new ZLinkStreamSessionClient(
            stream,
            routingId,
            this,
            serializer,
            defaultCodec,
            compressionCodec,
            oneWayCalls);
    }

    ZLinkOneWayCalls oneWayCalls() {
        return oneWayCalls;
    }

    @Override
    public ZLinkSessionActors actors() {
        if (actors == null) {
            throw new ZLinkConfigurationException("stream node is not attached to a session relay");
        }
        return actors;
    }

    CompletionStage<Void> notifyBoundActorsDisconnected() {
        if (actors instanceof ZLinkSessionActorsRuntime runtime) {
            return runtime.notifyDisconnectedAll();
        }
        return CompletableFuture.completedFuture(null);
    }

    void closeReplyRetries() {
        if (!replyRetriesClosed.compareAndSet(false, true)) {
            return;
        }
        for (ReplyAttempt attempt : replyAttempts) {
            attempt.cancel();
        }
    }

    CompletionStage<systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec
        .SessionRelocationRouted> applyRelocationRouteCommand(
            systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec
                .SessionRelocationRoute command) {
        if (!(actors instanceof ZLinkSessionActorsRuntime runtime)) {
            return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                "Session is not attached to Actor routing"));
        }
        return runtime.applyRelocationRouteCommand(command);
    }

    @Override
    public CompletionStage<Void> close() {
        return closeAction.get();
    }

    CompletionStage<Void> dispatchStage(
        ZLinkStreamHeader header,
        ZLinkMessage payload,
        ZLinkSession session) {
        ZLinkFlowContext.State dispatchFlow = ZLinkFlowContext.current();
        if (header.requestSequence().isPresent()) {
            String dispatchKey = dispatchFlow == null
                ? "request:" + header.requestSequence().orElseThrow()
                : dispatchFlow.flowId();
            requestHeadersByFlow.put(dispatchKey, header);
        }
        ZLinkStreamRuntime.trace("stream-node dispatch-start node=" + streamNodeName
            + " routingId=" + routingId
            + " name=" + header.packetName()
            + " requestSeq=" + header.requestSequence().orElse(null)
            + " correlation=" + header.correlationId().orElse(null));
        ZLinkSessionDispatchContext dispatch = new ZLinkSessionDispatchContext(
            header.name(),
            header.metadata(),
            header.requestSequence().isPresent());
        CompletionStage<Void> stage;
        try {
            ZLinkSessionActorsRuntime.enterRelayDispatch(dispatch, header);
            try {
                stage = java.util.Objects.requireNonNull(
                    session.onDispatch(dispatch, payload),
                    "session onDispatch result");
            } finally {
                ZLinkSessionActorsRuntime.exitRelayDispatch();
            }
        } catch (RuntimeException ex) {
            stage = CompletableFuture.failedFuture(ex);
        }
        CompletableFuture<Void> result = new CompletableFuture<>();
        stage.whenComplete((ignored, error) -> {
            ZLinkSessionActorsRuntime.exitRelayDispatch(dispatch);
            completeDispatch(header, error, result);
        });
        return result;
    }

    void traceStreamReplied(ZLinkStreamHeader requestHeader) {
        if (flow.enabled(systems.zlink.framework.configuration.ZLinkMessageFlowOutcome.REPLIED)) {
            flow.trace(new systems.zlink.framework.configuration.ZLinkMessageFlowEvent(
                systems.zlink.framework.configuration.ZLinkMessageFlowOutcome.REPLIED,
                systems.zlink.framework.configuration.ZLinkDispatchErrorSurface.STREAM_SESSION,
                systems.zlink.framework.configuration.ZLinkDispatchMessageKind.REQUEST,
                requestHeader.packetName(),
                null,
                null,
                ZLinkStreamCorrelations.forTrace(requestHeader),
                null,
                null,
                null,
                null,
                null, null, null, null,
                requestHeader.flowId().orElse(null),
                requestHeader.flowOrigin().orElse(null)));
        }
    }

    Optional<ZLinkStreamHeader> currentDispatchHeader() {
        ZLinkFlowContext.State flow = ZLinkFlowContext.current();
        if (flow != null) {
            ZLinkStreamHeader header = requestHeadersByFlow.get(flow.flowId());
            if (header != null) {
                return Optional.of(header);
            }
        }
        if (requestHeadersByFlow.size() == 1) {
            return requestHeadersByFlow.values().stream().findFirst();
        }
        return Optional.empty();
    }

    boolean claimReplyHeader(ZLinkStreamHeader header) {
        return claimedReplyHeaders.putIfAbsent(header, Boolean.TRUE) == null;
    }

    private void completeDispatch(
        ZLinkStreamHeader header,
        Throwable error,
        CompletableFuture<Void> result) {
        requestHeadersByFlow.entrySet().removeIf(entry -> entry.getValue() == header);
        claimedReplyHeaders.remove(header);
        if (error != null) {
            completeDispatchError(header, error, result);
            return;
        }
        if (header.requestSequence().isEmpty()
            && flow.enabled(systems.zlink.framework.configuration.ZLinkMessageFlowOutcome.DISPATCHED)) {
            flow.trace(new systems.zlink.framework.configuration.ZLinkMessageFlowEvent(
                systems.zlink.framework.configuration.ZLinkMessageFlowOutcome.DISPATCHED,
                systems.zlink.framework.configuration.ZLinkDispatchErrorSurface.STREAM_SESSION,
                systems.zlink.framework.configuration.ZLinkDispatchMessageKind.SEND,
                header.packetName(),
                null,
                null,
                header.correlationId().orElse(null),
                null,
                null,
                null,
                null,
                null, null, null, null,
                header.flowId().orElse(null),
                header.flowOrigin().orElse(null)));
        }
        result.complete(null);
    }

    private void completeDispatchError(
        ZLinkStreamHeader header,
        Throwable error,
        CompletableFuture<Void> result) {
        traceDispatchError(header, error);
        if (header.requestSequence().isEmpty()) {
            result.completeExceptionally(error);
            return;
        }
        sendErrorReply(header, error).whenComplete((ignored, sendError) -> {
            if (sendError != null) {
                result.completeExceptionally(sendError);
            } else {
                result.complete(null);
            }
        });
    }

    private void traceDispatchError(ZLinkStreamHeader header, Throwable error) {
        if (!flow.enabled(systems.zlink.framework.configuration.ZLinkMessageFlowOutcome.ERROR)) {
            return;
        }
        Throwable actual = unwrap(error);
        flow.trace(new systems.zlink.framework.configuration.ZLinkMessageFlowEvent(
            systems.zlink.framework.configuration.ZLinkMessageFlowOutcome.ERROR,
            systems.zlink.framework.configuration.ZLinkDispatchErrorSurface.STREAM_SESSION,
            header.requestSequence().isPresent()
                ? systems.zlink.framework.configuration.ZLinkDispatchMessageKind.REQUEST
                : systems.zlink.framework.configuration.ZLinkDispatchMessageKind.SEND,
            header.packetName(),
            null,
            null,
            ZLinkStreamCorrelations.forTrace(header),
            null,
            null,
            null,
            null,
            systems.zlink.framework.configuration.ZLinkDispatchErrorReason.HANDLER_EXCEPTION,
            header.requestSequence().isPresent()
                ? systems.zlink.framework.configuration.ZLinkDispatchErrorAction.REPLY_ERROR
                : systems.zlink.framework.configuration.ZLinkDispatchErrorAction.DROP,
            actual.getClass().getName(),
            actual.getMessage(),
            header.flowId().orElse(null),
            header.flowOrigin().orElse(null)));
    }

    private CompletionStage<Void> sendErrorReply(
        ZLinkStreamHeader requestHeader,
        Throwable error) {
        try (Message payload = Message.from(ZLinkStreamErrorPayload.encode(error))) {
            ZLinkStreamHeader replyHeader =
                ZLinkStreamHeader.createErrorResponse(requestHeader, requestHeader.packetName());
            return submitReplyAsync(replyHeader, payload.toByteArray());
        }
    }

    private CompletionStage<Void> submitReplyAsync(
        ZLinkStreamHeader replyHeader,
        byte[] payloadBytes) {
        ReplyAttempt attempt = new ReplyAttempt(
            replyHeader,
            payloadBytes,
            System.nanoTime() + ASYNC_REPLY_TIMEOUT_NANOS);
        replyAttempts.add(attempt);
        attempt.run();
        return attempt.completion;
    }

    private final class ReplyAttempt implements Runnable {
        private final ZLinkStreamHeader replyHeader;
        private final byte[] payloadBytes;
        private final long deadlineNanos;
        private final CompletableFuture<Void> completion = new CompletableFuture<>();
        private final AtomicBoolean terminal = new AtomicBoolean();
        private volatile ScheduledFuture<?> retry;

        private ReplyAttempt(
            ZLinkStreamHeader replyHeader,
            byte[] payloadBytes,
            long deadlineNanos) {
            this.replyHeader = replyHeader;
            this.payloadBytes = payloadBytes;
            this.deadlineNanos = deadlineNanos;
        }

        @Override
        public void run() {
            if (terminal.get()) {
                return;
            }
            if (replyRetriesClosed.get()) {
                cancel();
                return;
            }
            try (Message payload = Message.from(payloadBytes)) {
                if (replyRetriesClosed.get()) {
                    cancel();
                    return;
                }
                if (stream.reply(
                    routingId,
                    replyHeader,
                    List.of(payload),
                    SendFlags.DONT_WAIT)) {
                    complete();
                    return;
                }
            } catch (RuntimeException failure) {
                completeExceptionally(failure);
                return;
            }
            if (replyRetriesClosed.get()) {
                cancel();
                return;
            }
            if (System.nanoTime() >= deadlineNanos) {
                completeExceptionally(new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                    "STREAM error reply was not admitted before the send deadline"));
                return;
            }
            if (replyRetryExecutor == null) {
                completeExceptionally(new IllegalStateException(
                    "STREAM reply retry executor is unavailable"));
                return;
            }
            try {
                retry = replyRetryExecutor.schedule(this, 10, TimeUnit.MILLISECONDS);
            } catch (RejectedExecutionException rejected) {
                completeExceptionally(rejected);
            }
        }

        private void cancel() {
            completeExceptionally(new CancellationException(
                "STREAM runtime closed before the error reply was admitted"));
        }

        private void complete() {
            if (!terminal.compareAndSet(false, true)) {
                return;
            }
            cancelRetry();
            replyAttempts.remove(this);
            completion.complete(null);
        }

        private void completeExceptionally(Throwable failure) {
            if (!terminal.compareAndSet(false, true)) {
                return;
            }
            cancelRetry();
            replyAttempts.remove(this);
            completion.completeExceptionally(failure);
        }

        private void cancelRetry() {
            ScheduledFuture<?> current = retry;
            if (current != null) {
                current.cancel(false);
            }
        }
    }

    private static Throwable unwrap(Throwable error) {
        Throwable current = error;
        while ((current instanceof CompletionException || current instanceof ExecutionException)
            && current.getCause() != null) {
            current = current.getCause();
        }
        return current;
    }
}
