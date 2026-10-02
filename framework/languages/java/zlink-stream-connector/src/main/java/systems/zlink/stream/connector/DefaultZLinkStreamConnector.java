package systems.zlink.stream.connector;

import systems.zlink.contracts.messaging.Message;

import java.net.URI;
import java.time.Duration;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ThreadFactory;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.BiFunction;
import java.util.function.BooleanSupplier;
import java.util.function.Consumer;
import java.util.function.Predicate;
import java.util.function.Supplier;
import java.util.logging.Level;
import java.util.logging.Logger;

final class DefaultZLinkStreamConnector implements ZLinkStreamConnector {
    private static final Logger LOGGER =
            Logger.getLogger(DefaultZLinkStreamConnector.class.getName());
    private static final String RESERVED_PACKET_NAME_PREFIX = "$zlink.";
    private static final boolean STREAM_TRACE =
            "1".equals(System.getenv("ZLINK_JAVA_STREAM_TRACE"));
    private final ScheduledExecutorService timeouts =
            Executors.newSingleThreadScheduledExecutor(new DaemonThreadFactory());

    private final ZLinkStreamConnectorConfiguration configuration;
    private final Map<String, List<ZLinkStreamMessageHandler<ZLinkStreamEncodedPayload>>> handlers =
            new ConcurrentHashMap<>();
    private final List<ZLinkStreamErrorHandler> errorHandlers = new CopyOnWriteArrayList<>();
    private final List<ZLinkStreamDisconnectedHandler> disconnectedHandlers =
            new CopyOnWriteArrayList<>();
    private final List<ZLinkStreamRequestSendingHandler> requestSendingHandlers =
            new CopyOnWriteArrayList<>();
    private final List<ZLinkStreamReplyReceivedHandler> replyReceivedHandlers =
            new CopyOnWriteArrayList<>();
    private final ZLinkStreamDispatchQueue dispatchQueue;
    private final ZLinkStreamActorRegistry actorRegistry;
    private final ZLinkStreamConnectorPayloadCodec payloadCodec;
    private final AtomicLong nextRequestSeq = new AtomicLong();
    //  Process-global monotonic correlation id (hex), stamped on outbound
    //  request packets only. Spec 27 §2: a one-way message that has no reply
    //  never creates a correlation_id.
    private final AtomicLong correlationCounter = new AtomicLong();
    private final ZLinkStreamPendingRequests pendingRequests = new ZLinkStreamPendingRequests();
    private final ZLinkStreamReceiveDispatcher receiveDispatcher;
    private final ZLinkStreamConnectionLifecycle lifecycle;

    private final ZLinkStreamSendChain sendChain = new ZLinkStreamSendChain();
    //  Common connector spec 32 6.2. `lastCloseReason` is the read surface:
    //  it starts empty, is filled the first time a connection ends (a failed
    //  first connect included) and is never cleared by a later reconnect, so
    //  code that missed the disconnect event still reads the last reason.
    //  Each ending names its reason where it is detected and hands it to
    //  notifyDisconnected.
    private final AtomicReference<ZLinkStreamCloseReason> lastCloseReason = new AtomicReference<>();

    DefaultZLinkStreamConnector(ZLinkStreamConnectorOptions options) {
        this.configuration = ZLinkStreamConnectorConfiguration.from(options);
        this.dispatchQueue = new ZLinkStreamDispatchQueue(this::publishError);
        this.actorRegistry =
                new ZLinkStreamActorRegistry(
                        this, configuration, dispatchQueue, this::publishError);
        this.payloadCodec = new ZLinkStreamConnectorPayloadCodec(this.configuration);
        this.receiveDispatcher =
                new ZLinkStreamReceiveDispatcher(
                        this.configuration,
                        handlers,
                        dispatchQueue,
                        pendingRequests,
                        payloadCodec,
                        this::publishError,
                        this::sendControl,
                        this::onSessionClosing,
                        actorRegistry);
        this.lifecycle =
                new ZLinkStreamConnectionLifecycle(
                        this.configuration,
                        timeouts,
                        dispatchQueue,
                        pendingRequests,
                        receiveDispatcher,
                        this::publishError,
                        actorRegistry::connectionEnded,
                        this::notifyDisconnected,
                        this::recordConnectAttemptFailure,
                        this::sendControl,
                        sendChain::reset);
    }

    @Override
    public boolean isConnected() {
        return lifecycle.isConnected();
    }

    @Override
    public ZLinkStreamConnectionState state() {
        return lifecycle.state();
    }

    @Override
    public ZLinkStreamConnectorOptions options() {
        return configuration.publicOptions();
    }

    @Override
    public Optional<ZLinkStreamCloseReason> closeReason() {
        return Optional.ofNullable(lastCloseReason.get());
    }

    @Override
    public int pendingDispatchCount() {
        return dispatchQueue.pendingCallbacks();
    }

    @Override
    public int receivedCount(String name) {
        requirePacketName(name);
        return dispatchQueue.receivedCount(name);
    }

    @Override
    public ZLinkStreamLifecycleCall connect() {
        return new DefaultZLinkStreamLifecycleCall(lifecycle::connect);
    }

    @Override
    public ZLinkStreamLifecycleCall close() {
        return new DefaultZLinkStreamLifecycleCall(this::closeInternal);
    }

    private CompletionStage<Void> closeInternal() {
        try {
            return lifecycle.close().whenComplete((ignored, failure) -> timeouts.shutdown());
        } catch (RuntimeException error) {
            timeouts.shutdown();
            throw error;
        }
    }

    @Override
    public ZLinkStreamLifecycleCall dispatch() {
        return new DefaultZLinkStreamLifecycleCall(this::dispatchInternalStage);
    }

    private CompletionStage<Void> dispatchInternalStage() {
        return dispatchQueue.drainAsync();
    }

    @Override
    public ZLinkStreamSendCall send(ZLinkStreamEncodedPayload payload) {
        return new ZLinkStreamConnectorSendCall(this, payloadCodec.copy(payload), false);
    }

    @Override
    public ZLinkStreamRequestCall request(ZLinkStreamEncodedPayload payload) {
        return new ZLinkStreamConnectorRequestCall(
                this, payloadCodec.copy(payload), configuration.timeouts().request(), false);
    }

    @Override
    public AutoCloseable on(
            String name, ZLinkStreamMessageHandler<ZLinkStreamEncodedPayload> handler) {
        requirePacketName(name);
        Objects.requireNonNull(handler, "handler");
        handlers.computeIfAbsent(name, ignored -> new CopyOnWriteArrayList<>()).add(handler);
        handlerRegistered();
        return () -> handlers.getOrDefault(name, List.of()).remove(handler);
    }

    /**
     * Spec 32 7, 10: a receive handler was registered on the connector or an Actor handle, so
     * packets already queued may now have one. In Immediate the registration is their dispatch
     * point; in Manual they wait for the next pump.
     */
    void handlerRegistered() {
        if (configuration.dispatchMode() == ZLinkStreamDispatchMode.IMMEDIATE) {
            dispatchQueue.drainAsync();
        }
    }

    CompletionStage<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> awaitMessage(
            String name, Predicate<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> predicate) {
        CompletableFuture<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> result =
                new CompletableFuture<>();
        dispatchQueue.awaitMessage(name, predicate, result);
        return result;
    }

    @Override
    public AutoCloseable onErrorReceived(ZLinkStreamErrorHandler handler) {
        Objects.requireNonNull(handler, "handler");
        errorHandlers.add(handler);
        return () -> errorHandlers.remove(handler);
    }

    @Override
    public AutoCloseable onRequestSending(ZLinkStreamRequestSendingHandler handler) {
        Objects.requireNonNull(handler, "handler");
        requestSendingHandlers.add(handler);
        return () -> requestSendingHandlers.remove(handler);
    }

    @Override
    public AutoCloseable onReplyReceived(ZLinkStreamReplyReceivedHandler handler) {
        Objects.requireNonNull(handler, "handler");
        replyReceivedHandlers.add(handler);
        return () -> replyReceivedHandlers.remove(handler);
    }

    @Override
    public AutoCloseable onDisconnected(ZLinkStreamDisconnectedHandler handler) {
        Objects.requireNonNull(handler, "handler");
        disconnectedHandlers.add(handler);
        return () -> disconnectedHandlers.remove(handler);
    }

    @Override
    public AutoCloseable onConnectionStateChanged(ZLinkStreamConnectionStateHandler handler) {
        Objects.requireNonNull(handler, "handler");
        return lifecycle.onConnectionStateChanged(handler);
    }

    @Override
    public List<ZLinkStreamActor> actors() {
        return actorRegistry.snapshot();
    }

    @Override
    public Optional<ZLinkStreamActor> actor(String actorId) {
        Objects.requireNonNull(actorId, "actorId");
        return actorRegistry.find(actorId);
    }

    @Override
    public AutoCloseable onActorBound(ZLinkStreamActorHandler handler) {
        return actorRegistry.onBound(handler);
    }

    @Override
    public AutoCloseable onActorUnbound(ZLinkStreamActorHandler handler) {
        return actorRegistry.onUnbound(handler);
    }

    ZLinkStreamSendCall actorSend(
            ZLinkStreamActorRegistry.DefaultActor actor, ZLinkStreamEncodedPayload payload) {
        return new ZLinkStreamConnectorSendCall(this, payloadCodec.copy(payload), false, actor);
    }

    ZLinkStreamRequestCall actorRequest(
            ZLinkStreamActorRegistry.DefaultActor actor, ZLinkStreamEncodedPayload payload) {
        return new ZLinkStreamConnectorRequestCall(
                this, payloadCodec.copy(payload), configuration.timeouts().request(), false, actor);
    }

    CompletionStage<Void> submit(ZLinkStreamEncodedPayload payload, boolean compress) {
        return submit(payload, compress, null);
    }

    CompletionStage<Void> submit(
            ZLinkStreamEncodedPayload payload,
            boolean compress,
            ZLinkStreamActorRegistry.DefaultActor actor) {
        //  Spec 32 9.2: submit() is the asynchronous surface, so a Send that
        //  is not accepted - no connection, an unbound Actor, a payload the
        //  codec rejects - fails its stage, as a Request does, and does not
        //  throw.
        try {
            Integer actorSlot = actorSlot(actor);
            byte[] body = payloadCodec.encode(payload, compress);
            //  Spec 27 §2: a one-way Send has no reply, so no correlation_id is
            //  created and header flag 0x08 stays clear.
            ZLinkStreamWireProtocol.Header header =
                    new ZLinkStreamWireProtocol.Header(
                            ZLinkStreamWireProtocol.KIND_SEND,
                            ZLinkStreamConnectorPayloadCodec.toWireCodec(payload.codec()),
                            (payload.metadata().isEmpty()
                                            ? 0
                                            : ZLinkStreamWireProtocol.FLAG_HAS_METADATA)
                                    | (compress
                                            ? ZLinkStreamWireProtocol.FLAG_PAYLOAD_COMPRESSED
                                            : 0),
                            null,
                            payload.packetName(),
                            payload.metadata(),
                            null,
                            null,
                            0,
                            actorSlot);
            return sendFrame(header, body);
        } catch (RuntimeException failure) {
            return CompletableFuture.failedFuture(failure);
        }
    }

    CompletionStage<ZLinkStreamEncodedPayload> submitRequest(
            ZLinkStreamEncodedPayload payload, Duration timeout, boolean compress) {
        return submitRequest(payload, timeout, compress, null);
    }

    CompletionStage<ZLinkStreamEncodedPayload> submitRequest(
            ZLinkStreamEncodedPayload payload,
            Duration timeout,
            boolean compress,
            ZLinkStreamActorRegistry.DefaultActor actor) {
        long start = System.nanoTime();
        String requestName = payload.packetName();
        String actorId = actor == null ? null : actor.actorId();
        BiFunction<ZLinkStreamEncodedPayload, BooleanSupplier, Boolean> onReply =
                (reply, complete) -> {
                    List<ZLinkStreamReplyReceivedHandler> registered =
                            List.copyOf(replyReceivedHandlers);
                    byte[] replyBytes =
                            reply == null || registered.isEmpty()
                                    ? null
                                    : reply.payload().toByteArray();
                    Duration elapsed = Duration.ofNanos(System.nanoTime() - start);
                    boolean completed = complete.getAsBoolean();
                    if (completed)
                        publishReplyReceived(
                                requestName, actorId, elapsed, reply, replyBytes, null, registered);
                    return completed;
                };
        BiFunction<Throwable, BooleanSupplier, Boolean> onFailure =
                (failure, complete) -> {
                    var registered = List.copyOf(replyReceivedHandlers);
                    boolean completed = complete.getAsBoolean();
                    if (completed)
                        publishReplyReceived(
                                requestName,
                                actorId,
                                Duration.ofNanos(System.nanoTime() - start),
                                null,
                                null,
                                failure,
                                registered);
                    return completed;
                };
        try {
            Map<String, String> metadata = new HashMap<>(payload.metadata());
            ZLinkStreamRequestSendingContext context =
                    new ZLinkStreamRequestSendingContext(payload.packetName(), actorId, metadata);
            for (ZLinkStreamRequestSendingHandler handler : List.copyOf(requestSendingHandlers)) {
                if (requestSendingHandlers.contains(handler)) {
                    Map<String, String> before = new HashMap<>(metadata);
                    try {
                        handler.handle(context);
                    } catch (Throwable failure) {
                        metadata.clear();
                        metadata.putAll(before);
                        publishError(userCallbackFailed(failure));
                    }
                }
            }
            ZLinkStreamEncodedPayload outgoing =
                    new ZLinkStreamEncodedPayload(
                            payload.packetName(), payload.payload(), metadata, payload.codec());
            return sendRequestFrame(outgoing, timeout, compress, actor, onReply, onFailure);
        } catch (RuntimeException failure) {
            onFailure.apply(failure, () -> true);
            return CompletableFuture.failedFuture(failure);
        }
    }

    private CompletionStage<ZLinkStreamEncodedPayload> sendRequestFrame(
            ZLinkStreamEncodedPayload payload,
            Duration timeout,
            boolean compress,
            ZLinkStreamActorRegistry.DefaultActor actor,
            BiFunction<ZLinkStreamEncodedPayload, BooleanSupplier, Boolean> onReply,
            BiFunction<Throwable, BooleanSupplier, Boolean> onFailure) {
        Integer actorSlot = actorSlot(actor);
        long requestSeq = nextRequestSeq();
        byte[] body = payloadCodec.encode(payload, compress);
        ZLinkStreamWireProtocol.Header header =
                new ZLinkStreamWireProtocol.Header(
                        ZLinkStreamWireProtocol.KIND_REQUEST,
                        ZLinkStreamConnectorPayloadCodec.toWireCodec(payload.codec()),
                        ZLinkStreamWireProtocol.FLAG_HAS_REQUEST_SEQ
                                | (payload.metadata().isEmpty()
                                        ? 0
                                        : ZLinkStreamWireProtocol.FLAG_HAS_METADATA)
                                | (compress ? ZLinkStreamWireProtocol.FLAG_PAYLOAD_COMPRESSED : 0),
                        requestSeq,
                        payload.packetName(),
                        payload.metadata(),
                        nextCorrelationId(),
                        null,
                        0,
                        actorSlot);

        return sendFrame(
                header,
                body,
                (write, writeFailure) ->
                        sendChain.enqueueRequestDeferred(
                                () -> traceWrite(header, write.get()),
                                failure -> {
                                    pendingRequests.fail(requestSeq, failure);
                                    writeFailure.accept(failure);
                                },
                                pending -> {
                                    pendingRequests.add(
                                            requestSeq,
                                            payload.packetName(),
                                            pending,
                                            onReply,
                                            onFailure);
                                    pendingRequests.startTimeout(requestSeq, timeout, timeouts);
                                }));
    }

    private void dispatchRequestCallback(Runnable callback) {
        if (configuration.dispatchMode() == ZLinkStreamDispatchMode.MANUAL) {
            dispatchQueue.add(callback);
        } else {
            callback.run();
        }
    }

    private void publishReplyReceived(
            String requestName,
            String actorId,
            Duration elapsed,
            ZLinkStreamEncodedPayload reply,
            byte[] replyBytes,
            Throwable failure,
            List<ZLinkStreamReplyReceivedHandler> registered) {
        if (registered.isEmpty()) {
            return;
        }
        Throwable cause = failure;
        while (cause instanceof CompletionException && cause.getCause() != null) {
            cause = cause.getCause();
        }
        //  Every request failure the connector decides carries its code.
        ZLinkStreamError error =
                cause == null ? null : ZLinkStreamPendingRequests.coded(cause).error();
        dispatchRequestCallback(
                () -> {
                    for (ZLinkStreamReplyReceivedHandler handler : registered) {
                        if (!replyReceivedHandlers.contains(handler)) {
                            continue;
                        }
                        ZLinkStreamMessage<ZLinkStreamEncodedPayload> replyMessage = null;
                        if (replyBytes != null) {
                            ZLinkStreamEncodedPayload copy =
                                    new ZLinkStreamEncodedPayload(
                                            reply.packetName(),
                                            Message.from(replyBytes),
                                            reply.metadata(),
                                            reply.codec());
                            replyMessage =
                                    new ZLinkStreamMessage<>(
                                            reply.packetName(), copy, reply.metadata(), actorId);
                        }
                        invokeReplyReceived(
                                handler,
                                new ZLinkStreamReplyReceivedContext(
                                        requestName,
                                        actorId,
                                        failure == null,
                                        replyMessage,
                                        error,
                                        elapsed));
                    }
                });
    }

    private void invokeReplyReceived(
            ZLinkStreamReplyReceivedHandler handler, ZLinkStreamReplyReceivedContext context) {
        if (!replyReceivedHandlers.contains(handler)) {
            if (context.reply() != null) {
                context.reply().payload().payload().close();
            }
            return;
        }
        try {
            handler.handle(context);
        } catch (Throwable failure) {
            if (context.reply() != null) {
                context.reply().payload().payload().close();
            }
            publishError(userCallbackFailed(failure));
        }
    }

    private CompletionStage<Void> sendFrame(ZLinkStreamWireProtocol.Header header, byte[] payload) {
        return traceWrite(header, sendFrame(header, payload, sendChain::enqueueDeferred));
    }

    private <T> CompletionStage<T> sendFrame(
            ZLinkStreamWireProtocol.Header header,
            byte[] payload,
            BiFunction<Supplier<CompletionStage<Void>>, Consumer<Throwable>, CompletableFuture<T>>
                    enqueue) {
        //  The wire codec is internal and reports structural problems with
        //  plain exceptions. This is the connector boundary, so a rejection
        //  the caller can act on (metadata limits, correlation id, send
        //  payload limit) leaves here as ValidationFailed (spec 32 9, 9.2).
        byte[] encodedHeader;
        byte[] frame;
        try {
            encodedHeader = ZLinkStreamWireProtocol.encodeHeader(header);
            frame =
                    ZLinkStreamWireProtocol.encodeFrame(
                            encodedHeader, payload, configuration.limits().sendPayload());
        } catch (ZLinkStreamException alreadyCoded) {
            throw alreadyCoded;
        } catch (IllegalArgumentException | IllegalStateException invalid) {
            throw ZLinkStreamException.validationFailed(
                    "outbound stream frame is invalid: " + invalid.getMessage(), invalid);
        }
        trace(
                () ->
                        "connector write-start endpoint="
                                + configuration.endpoint()
                                + " kind="
                                + header.kind()
                                + " name="
                                + header.name()
                                + " requestSeq="
                                + header.requestSeq()
                                + " bytes="
                                + payload.length
                                + " correlation="
                                + header.correlationId());
        return lifecycle.enqueueFrame(sendChain, frame, enqueue);
    }

    private CompletionStage<Void> traceWrite(
            ZLinkStreamWireProtocol.Header header, CompletionStage<Void> publication) {
        publication.whenComplete(
                (ignored, ex) -> {
                    if (ex == null) {
                        trace(
                                () ->
                                        "connector write-complete endpoint="
                                                + configuration.endpoint()
                                                + " kind="
                                                + header.kind()
                                                + " name="
                                                + header.name()
                                                + " requestSeq="
                                                + header.requestSeq()
                                                + " correlation="
                                                + header.correlationId());
                    } else {
                        trace(
                                () ->
                                        "connector write-failed endpoint="
                                                + configuration.endpoint()
                                                + " kind="
                                                + header.kind()
                                                + " name="
                                                + header.name()
                                                + " requestSeq="
                                                + header.requestSeq()
                                                + " correlation="
                                                + header.correlationId()
                                                + " error="
                                                + ex);
                    }
                });
        return publication;
    }

    private CompletionStage<Void> sendControl(String name) {
        ZLinkStreamWireProtocol.Header header =
                new ZLinkStreamWireProtocol.Header(
                        ZLinkStreamWireProtocol.KIND_CONTROL,
                        ZLinkStreamWireProtocol.CODEC_RAW,
                        0,
                        null,
                        name,
                        Map.of(),
                        null,
                        null,
                        0);
        return sendFrame(header, new byte[0]);
    }

    private Integer actorSlot(ZLinkStreamActorRegistry.DefaultActor actor) {
        return actor == null ? null : actorRegistry.currentSlot(actor);
    }

    private void notifyDisconnected(ZLinkStreamCloseReason reason) {
        lastCloseReason.set(reason);
        ZLinkStreamDisconnected event = new ZLinkStreamDisconnected(reason);
        //  disconnectedHandlers is a CopyOnWriteArrayList: its iterator is
        //  already the snapshot a callback that registers or removes a
        //  handler needs, so copying it again only allocates.
        for (ZLinkStreamDisconnectedHandler handler : disconnectedHandlers) {
            if (configuration.dispatchMode() == ZLinkStreamDispatchMode.IMMEDIATE) {
                invokeUserCallback(() -> handler.handle(event));
            } else {
                dispatchQueue.addAsync(() -> invokeUserCallback(() -> handler.handle(event)));
            }
        }
    }

    /**
     * Records why a connect attempt failed even though no connection had been established, so spec
     * 32 6.2's "a failed first connect also leaves a reason" holds. A connect attempt fails at the
     * transport, where spec 32 9's impact table also puts ConnectTimeout and TlsValidationFailed.
     */
    private void recordConnectAttemptFailure() {
        lastCloseReason.set(ZLinkStreamCloseReason.TRANSPORT_ERROR);
    }

    private void onSessionClosing(ZLinkStreamCloseReason reason) {
        lifecycle.serverClosing(reason);
    }

    private void publishError(ZLinkStreamError error) {
        for (ZLinkStreamErrorHandler handler : errorHandlers) {
            dispatchErrorCallback(handler, error, true);
        }
    }

    private void dispatchErrorCallback(
            ZLinkStreamErrorHandler handler, ZLinkStreamError error, boolean reportFailure) {
        if (configuration.dispatchMode() == ZLinkStreamDispatchMode.IMMEDIATE) {
            invokeErrorCallback(handler, error, reportFailure);
        } else {
            dispatchQueue.addAsync(() -> invokeErrorCallback(handler, error, reportFailure));
        }
    }

    private CompletionStage<Void> invokeUserCallback(UserCallback callback) {
        try {
            return callback.invoke()
                    .exceptionally(
                            ex -> {
                                publishUserCallbackFailed(ex);
                                return null;
                            });
        } catch (Throwable ex) {
            publishUserCallbackFailed(ex);
            return CompletableFuture.completedFuture(null);
        }
    }

    private CompletionStage<Void> invokeErrorCallback(
            ZLinkStreamErrorHandler handler, ZLinkStreamError error, boolean reportFailure) {
        try {
            return handler.handleAsync(error)
                    .exceptionally(
                            ex -> {
                                if (reportFailure) {
                                    reportErrorCallbackFailure(handler, ex);
                                }
                                return null;
                            });
        } catch (Throwable ex) {
            if (reportFailure) {
                reportErrorCallbackFailure(handler, ex);
            }
            return CompletableFuture.completedFuture(null);
        }
    }

    private void reportErrorCallbackFailure(
            ZLinkStreamErrorHandler failedHandler, Throwable failure) {
        LOGGER.log(Level.WARNING, "STREAM connector error callback failed", failure);
        ZLinkStreamError callbackError = userCallbackFailed(failure);
        for (ZLinkStreamErrorHandler handler : errorHandlers) {
            if (handler != failedHandler) {
                dispatchErrorCallback(handler, callbackError, false);
            }
        }
    }

    static ZLinkStreamError userCallbackFailed(Throwable ex) {
        return new ZLinkStreamError(
                ZLinkStreamErrorCode.USER_CALLBACK_FAILED, "User callback failed.", ex);
    }

    private void publishUserCallbackFailed(Throwable ex) {
        publishError(userCallbackFailed(ex));
    }

    @FunctionalInterface
    private interface UserCallback {
        CompletionStage<Void> invoke();
    }

    static int resolvePort(URI endpoint) {
        if (endpoint.getPort() > 0) {
            return endpoint.getPort();
        }
        throw ZLinkStreamException.configurationError("endpoint port is required");
    }

    private static String requirePacketName(String packetName) {
        //  Pre-send validation, so spec 32 9 makes every rejection here
        //  ValidationFailed and 9.2 requires it to carry that code.
        try {
            ZLinkStreamWireProtocol.validatePacketName(packetName);
        } catch (IllegalArgumentException invalid) {
            throw ZLinkStreamException.validationFailed(invalid.getMessage(), invalid);
        }
        if (packetName.startsWith(RESERVED_PACKET_NAME_PREFIX)) {
            throw ZLinkStreamException.validationFailed("packetName uses a reserved zlink prefix");
        }
        return packetName;
    }

    static String validatePacketName(String packetName) {
        return requirePacketName(packetName);
    }

    private static long nextRequestSeq(AtomicLong source) {
        while (true) {
            long previous = source.get();
            if (previous == -1L) {
                throw ZLinkStreamException.of(
                        ZLinkStreamErrorCode.SEND_FAILED, "request sequence is exhausted");
            }
            long next = previous + 1;
            if (source.compareAndSet(previous, next)) {
                return next;
            }
        }
    }

    private String nextCorrelationId() {
        return Long.toHexString(correlationCounter.incrementAndGet());
    }

    private long nextRequestSeq() {
        return nextRequestSeq(nextRequestSeq);
    }

    private static final class DaemonThreadFactory implements ThreadFactory {
        @Override
        public Thread newThread(Runnable runnable) {
            Thread thread = new Thread(runnable, "zlink-stream-connector-timeouts");
            thread.setDaemon(true);
            return thread;
        }
    }

    static void trace(Supplier<String> message) {
        if (STREAM_TRACE) {
            LOGGER.fine(() -> "[zlink-java-stream-trace] " + message.get());
        }
    }
}
