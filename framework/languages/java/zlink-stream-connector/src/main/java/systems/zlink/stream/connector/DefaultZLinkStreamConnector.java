package systems.zlink.stream.connector;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.util.function.Predicate;

import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ThreadFactory;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
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

    private final ZLinkStreamConnectorConfiguration configuration;
    private final Map<String, List<ZLinkStreamMessageHandler<ZLinkStreamEncodedPayload>>> handlers =
        new ConcurrentHashMap<>();
    private final List<ZLinkStreamErrorHandler> errorHandlers = new CopyOnWriteArrayList<>();
    private final List<ZLinkStreamDisconnectedHandler> disconnectedHandlers =
        new CopyOnWriteArrayList<>();
    private final ZLinkStreamDispatchQueue dispatchQueue;
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
    //  `stagedCloseReason` holds the reason a specific ending already knows
    //  (client close, heartbeat timeout, a server `session-closing`) until
    //  the disconnect is published; anything else ends as TRANSPORT_ERROR.
    private final AtomicReference<ZLinkStreamCloseReason> lastCloseReason =
        new AtomicReference<>();
    //  Three threads stage a reason (heartbeat, receive, application) and
    //  two consume it, so the read-and-clear has to be one step.
    private final AtomicReference<ZLinkStreamCloseReason> stagedCloseReason =
        new AtomicReference<>();

    DefaultZLinkStreamConnector(ZLinkStreamConnectorOptions options) {
        this.configuration = ZLinkStreamConnectorConfiguration.from(options);
        this.dispatchQueue = new ZLinkStreamDispatchQueue(this::publishError);
        this.payloadCodec = new ZLinkStreamConnectorPayloadCodec(this.configuration);
        this.receiveDispatcher = new ZLinkStreamReceiveDispatcher(
            this.configuration,
            handlers,
            dispatchQueue,
            pendingRequests,
            payloadCodec,
            this::publishError,
            this::sendControl,
            this::onSessionClosing);
        this.lifecycle = new ZLinkStreamConnectionLifecycle(
            this.configuration,
            timeouts,
            dispatchQueue,
            pendingRequests,
            receiveDispatcher,
            this::publishError,
            this::notifyDisconnected,
            this::stageCloseReason,
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
    public ZLinkStreamDiagnosticsLevel diagnosticsLevel() {
        return configuration.diagnosticsLevel();
    }

    @Override
    public void setDiagnosticsLevel(ZLinkStreamDiagnosticsLevel level) {
        //  Common connector spec 32 13: the synchronous surface changes the
        //  value without waiting, so calling it from inside a dispatch
        //  callback cannot wait on its own completion.
        configuration.diagnosticsLevel(level);
    }

    @Override
    public CompletionStage<Void> setDiagnosticsLevelAsync(
        ZLinkStreamDiagnosticsLevel level) {
        setDiagnosticsLevel(level);
        return CompletableFuture.completedFuture(null);
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
        //  A connector that never left CREATED was never connected, so
        //  closing it is not an ending that 6.2 records.
        if (state() != ZLinkStreamConnectionState.CREATED) {
            stagedCloseReason.set(ZLinkStreamCloseReason.CLIENT_CLOSE);
        }
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
        Predicate<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> predicate) {
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
        ZLinkConnectorFlowContext.State flow = outboundFlow();
        byte[] body = payloadCodec.encode(payload, compress);
        //  Spec 27 §2: a one-way Send has no reply, so no correlation_id is
        //  created and header flag 0x08 stays clear.
        ZLinkStreamWireProtocol.Header header = new ZLinkStreamWireProtocol.Header(
            ZLinkStreamWireProtocol.KIND_SEND,
            ZLinkStreamConnectorPayloadCodec.toWireCodec(payload.codec()),
            (payload.metadata().isEmpty() ? 0 : ZLinkStreamWireProtocol.FLAG_HAS_METADATA)
                | (compress ? ZLinkStreamWireProtocol.FLAG_PAYLOAD_COMPRESSED : 0),
            null,
            payload.packetName(),
            payload.metadata(),
            null,
            flow == null ? null : flow.flowId(),
            flow == null ? 0 : flow.flowOrigin());
        return sendFrame(header, body);
    }

    /**
     * Spec 27 §4: at Off no flow pair is created or copied onto outbound
     * envelopes (flag 0x10 stays clear); otherwise the ambient flow is
     * preserved or a new application flow starts on the first outbound call.
     */
    private ZLinkConnectorFlowContext.State outboundFlow() {
        //  Single atomic read of the level for this one outbound processing
        //  point; the boolean derived from it is used for every decision
        //  this submit/submitRequest call makes, never re-read mid-call.
        return ZLinkStreamConnectorConfiguration.flowCaptureEnabled(configuration.diagnosticsLevel())
            ? ZLinkConnectorFlowContext.currentOrApplication()
            : null;
    }

    CompletionStage<ZLinkStreamEncodedPayload> submitRequest(
        ZLinkStreamEncodedPayload payload,
        Duration timeout,
        boolean compress) {
        ensureConnected();
        ZLinkConnectorFlowContext.State flow = outboundFlow();
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
            flow == null ? null : flow.flowId(),
            flow == null ? 0 : flow.flowOrigin());

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
        //  The wire codec is internal and reports structural problems with
        //  plain exceptions. This is the connector boundary, so a rejection
        //  the caller can act on (metadata limits, correlation id, send
        //  payload limit) leaves here as ValidationFailed (spec 32 9, 9.2).
        byte[] encodedHeader;
        byte[] frame;
        try {
            encodedHeader = ZLinkStreamWireProtocol.encodeHeader(header);
            frame = ZLinkStreamWireProtocol.encodeFrame(
                encodedHeader,
                payload,
                configuration.limits().sendPayload());
        } catch (ZLinkStreamException alreadyCoded) {
            throw alreadyCoded;
        } catch (IllegalArgumentException | IllegalStateException invalid) {
            throw ZLinkStreamException.validationFailed(
                "outbound stream frame is invalid: " + invalid.getMessage(), invalid);
        }
        trace("connector write-start endpoint=" + configuration.endpoint()
            + " kind=" + header.kind()
            + " name=" + header.name()
            + " requestSeq=" + header.requestSeq()
            + " bytes=" + payload.length
            + " correlation=" + header.correlationId()
            + " flow=" + header.flowId()
            + " origin=" + flowOriginName(header.flowOrigin()));
        CompletableFuture<Void> publication = sendChain.enqueue(() -> writeFrame(frame));
        return publication.whenComplete((ignored, ex) -> {
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
            throw ZLinkStreamException.disconnected("connector is not connected");
        }
    }

    private void notifyDisconnected() {
        ZLinkStreamCloseReason reason = takeCloseReason();
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

    private void stageCloseReason(ZLinkStreamCloseReason reason) {
        stagedCloseReason.set(reason);
    }

    /**
     * Records why a connect attempt failed even though no connection had
     * been established, so spec 32 6.2's "a failed first connect also leaves
     * a reason" holds. Spec 32 9's impact table maps ConnectTimeout and
     * TlsValidationFailed to TransportError.
     */
    private void recordConnectAttemptFailure() {
        takeCloseReason();
    }

    private ZLinkStreamCloseReason takeCloseReason() {
        ZLinkStreamCloseReason staged = stagedCloseReason.getAndSet(null);
        ZLinkStreamCloseReason reason =
            staged == null ? ZLinkStreamCloseReason.TRANSPORT_ERROR : staged;
        lastCloseReason.set(reason);
        return reason;
    }

    private void onSessionClosing(ZLinkStreamCloseReason reason) {
        stageCloseReason(reason);
        lifecycle.serverClosing();
    }

    private void publishError(ZLinkStreamError error) {
        for (ZLinkStreamErrorHandler handler : errorHandlers) {
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
        for (ZLinkStreamErrorHandler handler : errorHandlers) {
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

    static int resolvePort(URI endpoint) {
        if (endpoint.getPort() > 0) {
            return endpoint.getPort();
        }
        throw ZLinkStreamException.configurationError("endpoint port is required");
    }

    private static String requirePacketName(String packetName) {
        //  Pre-send validation, so spec 32 9 makes every rejection here
        //  ValidationFailed and 9.2 requires it to carry that code.
        if (packetName == null || packetName.isBlank()) {
            throw ZLinkStreamException.validationFailed("packetName is required");
        }
        if (packetName.startsWith(RESERVED_PACKET_NAME_PREFIX)) {
            throw ZLinkStreamException.validationFailed(
                "packetName uses a reserved zlink prefix");
        }
        if (packetName.getBytes(StandardCharsets.UTF_8).length > MAX_PACKET_NAME_BYTES) {
            throw ZLinkStreamException.validationFailed(
                "packetName must not exceed 255 UTF-8 bytes");
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
