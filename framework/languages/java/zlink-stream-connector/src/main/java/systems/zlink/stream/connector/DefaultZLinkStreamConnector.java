package systems.zlink.stream.connector;

import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ThreadFactory;
import java.util.concurrent.atomic.AtomicLong;
import java.util.logging.Level;
import java.util.logging.Logger;
import systems.zlink.contracts.messaging.Message;

final class DefaultZLinkStreamConnector implements ZLinkStreamConnector {
    private static final Logger LOGGER = Logger.getLogger(DefaultZLinkStreamConnector.class.getName());
    private static final String RESERVED_PACKET_NAME_PREFIX = "$zlink.";
    private static final String HEARTBEAT_PING_NAME = "$zlink.heartbeat.ping";
    private static final String HEARTBEAT_PONG_NAME = "$zlink.heartbeat.pong";
    private static final int MAX_PACKET_NAME_BYTES = 255;
    private static final boolean STREAM_TRACE =
        "1".equals(System.getenv("ZLINK_JAVA_STREAM_TRACE"));
    private final ScheduledExecutorService timeouts =
        Executors.newSingleThreadScheduledExecutor(new DaemonThreadFactory());

    private final ZLinkStreamConnectorOptions options;
    private final ZLinkStreamConnectorConfiguration configuration;
    private final Map<String, List<ZLinkStreamMessageHandler<ZLinkStreamEncodedPayload>>> handlers =
        new ConcurrentHashMap<>();
    private final List<ZLinkStreamErrorHandler> errorHandlers = new CopyOnWriteArrayList<>();
    private final List<ZLinkStreamDisconnectedHandler> disconnectedHandlers =
        new CopyOnWriteArrayList<>();
    private final ZLinkStreamDispatchQueue dispatchQueue;
    private final ZLinkStreamConnectorPayloadCodec payloadCodec;
    private final AtomicLong nextRequestSeq = new AtomicLong();
    // Process-global monotonic correlation id (hex), stamped on every non-control
    // outbound packet so the server can trace/echo it regardless of tracing mode.
    private final AtomicLong correlationCounter = new AtomicLong();
    private final ZLinkStreamPendingRequests pendingRequests = new ZLinkStreamPendingRequests();
    private final ZLinkStreamInboundObserverDispatcher inboundObservers;
    private final ZLinkStreamReceiveDispatcher receiveDispatcher;
    private final ZLinkStreamConnectionLifecycle lifecycle;

    private CompletableFuture<Void> sendChain = CompletableFuture.completedFuture(null);
    private volatile ZLinkStreamCloseReason closeReason = ZLinkStreamCloseReason.TRANSPORT_ERROR;

    DefaultZLinkStreamConnector(ZLinkStreamConnectorOptions options) {
        this.configuration = ZLinkStreamConnectorConfiguration.from(options);
        this.options = this.configuration.publicOptions();
        this.dispatchQueue = new ZLinkStreamDispatchQueue(
            this.configuration.limits().receivedMessages(),
            this::publishError);
        this.inboundObservers = new ZLinkStreamInboundObserverDispatcher(
            this.configuration.limits().inboundObserverNotifications(),
            this.configuration.limits().inboundObserverPayloadPreviewBytes(),
            this::publishError);
        this.payloadCodec = new ZLinkStreamConnectorPayloadCodec(this.configuration);
        this.receiveDispatcher = new ZLinkStreamReceiveDispatcher(
            this.configuration,
            handlers,
            dispatchQueue,
            pendingRequests,
            inboundObservers,
            payloadCodec,
            this::publishError,
            this::sendControl,
            this::onSessionClosing);
        this.lifecycle = new ZLinkStreamConnectionLifecycle(
            this.configuration,
            timeouts,
            dispatchQueue,
            pendingRequests,
            inboundObservers,
            receiveDispatcher,
            this::publishError,
            this::notifyDisconnected,
            reason -> closeReason = reason,
            this::sendControl);
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
        return options;
    }

    @Override
    public int pendingDispatchCount() {
        return dispatchQueue.size();
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
        closeReason = ZLinkStreamCloseReason.CLIENT_CLOSE;
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
            this,
            payloadCodec.copy(payload),
            configuration.timeouts().request(),
            false);
    }

    @Override
    public AutoCloseable on(
        String name,
        ZLinkStreamMessageHandler<ZLinkStreamEncodedPayload> handler) {
        requirePacketName(name);
        Objects.requireNonNull(handler, "handler");
        handlers.computeIfAbsent(name, ignored -> new CopyOnWriteArrayList<>()).add(handler);
        return () -> handlers.getOrDefault(name, List.of()).remove(handler);
    }

    CompletionStage<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> awaitMessage(
        String name,
        java.util.function.Predicate<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> predicate) {
        CompletableFuture<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> result =
            new CompletableFuture<>();
        dispatchQueue.awaitMessage(name, predicate, result);
        return result;
    }

    @Override
    public AutoCloseable observeInbound(ZLinkStreamInboundObserver observer) {
        Objects.requireNonNull(observer, "observer");
        lifecycle.requireCanRegisterInboundObserver();
        return inboundObservers.add(observer);
    }

    @Override
    public AutoCloseable onErrorReceived(ZLinkStreamErrorHandler handler) {
        Objects.requireNonNull(handler, "handler");
        errorHandlers.add(handler);
        return () -> errorHandlers.remove(handler);
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

    CompletionStage<Void> submit(ZLinkStreamEncodedPayload payload, boolean compress) {
        ensureConnected();
        ZLinkConnectorFlowContext.State flow = ZLinkConnectorFlowContext.currentOrApplication();
        byte[] body = payloadCodec.encode(payload, compress);
        ZLinkStreamWireProtocol.Header header = new ZLinkStreamWireProtocol.Header(
            ZLinkStreamWireProtocol.KIND_SEND,
            ZLinkStreamConnectorPayloadCodec.toWireCodec(payload.codec()),
            (payload.metadata().isEmpty() ? 0 : ZLinkStreamWireProtocol.FLAG_HAS_METADATA)
                | (compress ? ZLinkStreamWireProtocol.FLAG_PAYLOAD_COMPRESSED : 0),
            null,
            payload.packetName(),
            payload.metadata(),
            nextCorrelationId(),
            flow.flowId(),
            flow.flowOrigin());
        return sendFrame(header, body);
    }

    CompletionStage<ZLinkStreamEncodedPayload> submitRequest(
        ZLinkStreamEncodedPayload payload,
        Duration timeout,
        boolean compress) {
        ensureConnected();
        ZLinkConnectorFlowContext.State flow = ZLinkConnectorFlowContext.currentOrApplication();
        long requestSeq = nextRequestSeq();
        CompletableFuture<ZLinkStreamEncodedPayload> pending =
            pendingRequests.add(requestSeq, payload.packetName(), timeout, timeouts);

        byte[] body = payloadCodec.encode(payload, compress);
        ZLinkStreamWireProtocol.Header header = new ZLinkStreamWireProtocol.Header(
            ZLinkStreamWireProtocol.KIND_REQUEST,
            ZLinkStreamConnectorPayloadCodec.toWireCodec(payload.codec()),
            ZLinkStreamWireProtocol.FLAG_HAS_REQUEST_SEQ
                | (payload.metadata().isEmpty() ? 0 : ZLinkStreamWireProtocol.FLAG_HAS_METADATA)
                | (compress ? ZLinkStreamWireProtocol.FLAG_PAYLOAD_COMPRESSED : 0),
            requestSeq,
            payload.packetName(),
            payload.metadata(),
            nextCorrelationId(),
            flow.flowId(),
            flow.flowOrigin());

        sendFrame(header, body).whenComplete((ignored, ex) -> {
            if (ex != null) {
                pendingRequests.fail(requestSeq, ex);
            }
        });
        return pending;
    }

    private CompletionStage<Void> sendFrame(
        ZLinkStreamWireProtocol.Header header,
        byte[] payload) {
        byte[] encodedHeader = ZLinkStreamWireProtocol.encodeHeader(header);
        byte[] frame = ZLinkStreamWireProtocol.encodeFrame(
            encodedHeader,
            payload,
            configuration.limits().sendPayload());
        trace("connector write-start endpoint=" + configuration.endpoint()
            + " kind=" + header.kind()
            + " name=" + header.name()
            + " requestSeq=" + header.requestSeq()
            + " bytes=" + payload.length
            + " correlation=" + header.correlationId()
            + " flow=" + header.flowId()
            + " origin=" + flowOriginName(header.flowOrigin()));
        synchronized (this) {
            sendChain = sendChain.thenCompose(ignored -> writeFrame(frame));
            return sendChain.whenComplete((ignored, ex) -> {
                if (ex == null) {
                    trace("connector write-complete endpoint=" + configuration.endpoint()
                        + " kind=" + header.kind()
                        + " name=" + header.name()
                        + " requestSeq=" + header.requestSeq()
                        + " correlation=" + header.correlationId());
                } else {
                    trace("connector write-failed endpoint=" + configuration.endpoint()
                        + " kind=" + header.kind()
                        + " name=" + header.name()
                        + " requestSeq=" + header.requestSeq()
                        + " correlation=" + header.correlationId()
                        + " error=" + ex);
                }
            });
        }
    }

    private static String flowOriginName(int origin) {
        return switch (origin) {
            case 1 -> "inbound";
            case 2 -> "timer";
            case 3 -> "application";
            case 4 -> "lifecycle";
            default -> null;
        };
    }

    private CompletionStage<Void> writeFrame(byte[] frame) {
        return lifecycle.writeAsync(frame);
    }

    private CompletionStage<Void> sendControl(String name) {
        ZLinkStreamWireProtocol.Header header = new ZLinkStreamWireProtocol.Header(
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

    private void ensureConnected() {
        if (!isConnected()) {
            throw new IllegalStateException("connector is not connected");
        }
    }

    private void notifyDisconnected() {
        ZLinkStreamDisconnected event = new ZLinkStreamDisconnected(closeReason);
        for (ZLinkStreamDisconnectedHandler handler : List.copyOf(disconnectedHandlers)) {
            if (configuration.dispatchMode() == ZLinkStreamDispatchMode.IMMEDIATE) {
                invokeUserCallback(() -> handler.handle(event));
            } else {
                dispatchQueue.addAsync(() -> invokeUserCallback(() -> handler.handle(event)));
            }
        }
        closeReason = ZLinkStreamCloseReason.TRANSPORT_ERROR;
    }

    private void onSessionClosing(ZLinkStreamCloseReason reason) {
        closeReason = reason;
        lifecycle.serverClosing();
    }

    private void publishError(ZLinkStreamError error) {
        for (ZLinkStreamErrorHandler handler : List.copyOf(errorHandlers)) {
            dispatchErrorCallback(handler, error, true);
        }
    }

    private void dispatchErrorCallback(
        ZLinkStreamErrorHandler handler,
        ZLinkStreamError error,
        boolean reportFailure) {
        if (configuration.dispatchMode() == ZLinkStreamDispatchMode.IMMEDIATE) {
            invokeErrorCallback(handler, error, reportFailure);
        } else {
            dispatchQueue.addAsync(() -> invokeErrorCallback(handler, error, reportFailure));
        }
    }

    private CompletionStage<Void> invokeUserCallback(UserCallback callback) {
        try {
            return callback.invoke().exceptionally(ex -> {
                publishUserCallbackFailed(ex);
                return null;
            });
        } catch (Throwable ex) {
            publishUserCallbackFailed(ex);
            return CompletableFuture.completedFuture(null);
        }
    }

    private CompletionStage<Void> invokeErrorCallback(
        ZLinkStreamErrorHandler handler,
        ZLinkStreamError error,
        boolean reportFailure) {
        try {
            return handler.handleAsync(error).exceptionally(ex -> {
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
        ZLinkStreamErrorHandler failedHandler,
        Throwable failure) {
        LOGGER.log(Level.WARNING, "STREAM connector error callback failed", failure);
        ZLinkStreamError callbackError = userCallbackFailed(failure);
        for (ZLinkStreamErrorHandler handler : List.copyOf(errorHandlers)) {
            if (handler != failedHandler) {
                dispatchErrorCallback(handler, callbackError, false);
            }
        }
    }

    static ZLinkStreamError userCallbackFailed(Throwable ex) {
        return new ZLinkStreamError(
            ZLinkStreamErrorCode.USER_CALLBACK_FAILED,
            "User callback failed.",
            ex);
    }

    private void publishUserCallbackFailed(Throwable ex) {
        publishError(userCallbackFailed(ex));
    }

    @FunctionalInterface
    private interface UserCallback {
        CompletionStage<Void> invoke();
    }

    static int resolvePort(java.net.URI endpoint) {
        if (endpoint.getPort() > 0) {
            return endpoint.getPort();
        }
        throw new IllegalArgumentException("endpoint port is required");
    }

    private static String requirePacketName(String packetName) {
        if (packetName == null || packetName.isBlank()) {
            throw new IllegalArgumentException("packetName is required");
        }
        if (packetName.startsWith(RESERVED_PACKET_NAME_PREFIX)) {
            throw new IllegalArgumentException("packetName uses a reserved zlink prefix");
        }
        if (packetName.getBytes(java.nio.charset.StandardCharsets.UTF_8).length > MAX_PACKET_NAME_BYTES) {
            throw new IllegalArgumentException("packetName must not exceed 255 UTF-8 bytes");
        }
        return packetName;
    }

    static String validatePacketName(String packetName) {
        return requirePacketName(packetName);
    }

    private static long nextRequestSeq(AtomicLong source) {
        while (true) {
            long value = source.incrementAndGet();
            if (value != 0) {
                return value;
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

    static void trace(String message) {
        if (STREAM_TRACE) {
            LOGGER.fine("[zlink-java-stream-trace] " + message);
        }
    }
}
