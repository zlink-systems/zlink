package systems.zlink.stream.connector;

import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.StandardSocketOptions;
import java.net.http.HttpClient;
import java.nio.channels.AsynchronousSocketChannel;
import java.nio.channels.CompletionHandler;
import java.security.GeneralSecurityException;
import java.security.SecureRandom;
import java.security.cert.X509Certificate;
import java.time.Duration;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.function.Consumer;
import java.util.function.Function;
import java.util.function.Supplier;

import javax.net.ssl.SSLContext;
import javax.net.ssl.TrustManager;
import javax.net.ssl.X509TrustManager;

final class ZLinkStreamConnectionLifecycle {
    private final ZLinkStreamConnectorConfiguration configuration;
    private final ScheduledExecutorService timeouts;
    private final ZLinkStreamDispatchQueue dispatchQueue;
    private final ZLinkStreamPendingRequests pendingRequests;
    private final ZLinkStreamReceiveDispatcher receiveDispatcher;
    private final Consumer<ZLinkStreamError> errorPublisher;
    private final Runnable disconnectedNotifier;
    private final Consumer<ZLinkStreamCloseReason> closeReasonRecorder;
    private final Runnable connectFailureRecorder;
    private final Function<String, CompletionStage<Void>> controlSender;
    private final Runnable connectionEstablishedNotifier;
    private final List<ZLinkStreamConnectionStateHandler> stateHandlers =
            new CopyOnWriteArrayList<>();
    //  The connection field and the state field move together. Every
    //  transition of the pair is decided under this lock and every
    //  notification runs after it is released, so two threads cannot both
    //  claim the same ending and no user callback runs while it is held.
    private final Object connectionAttemptLock = new Object();

    private volatile ZLinkStreamConnectionState state = ZLinkStreamConnectionState.CREATED;
    private volatile ZLinkStreamTransportConnection connection;
    private volatile ScheduledFuture<?> heartbeatTask;
    private volatile long lastInboundNanos = System.nanoTime();
    private volatile CompletableFuture<Void> connectionAttempt;

    ZLinkStreamConnectionLifecycle(
            ZLinkStreamConnectorConfiguration configuration,
            ScheduledExecutorService timeouts,
            ZLinkStreamDispatchQueue dispatchQueue,
            ZLinkStreamPendingRequests pendingRequests,
            ZLinkStreamReceiveDispatcher receiveDispatcher,
            Consumer<ZLinkStreamError> errorPublisher,
            Runnable disconnectedNotifier,
            Consumer<ZLinkStreamCloseReason> closeReasonRecorder,
            Runnable connectFailureRecorder,
            Function<String, CompletionStage<Void>> controlSender,
            Runnable connectionEstablishedNotifier) {
        this.configuration = configuration;
        this.timeouts = timeouts;
        this.dispatchQueue = dispatchQueue;
        this.pendingRequests = pendingRequests;
        this.receiveDispatcher = receiveDispatcher;
        this.errorPublisher = errorPublisher;
        this.disconnectedNotifier = disconnectedNotifier;
        this.closeReasonRecorder = closeReasonRecorder;
        this.connectFailureRecorder = connectFailureRecorder;
        this.controlSender = controlSender;
        this.connectionEstablishedNotifier = connectionEstablishedNotifier;
    }

    boolean isConnected() {
        return state == ZLinkStreamConnectionState.CONNECTED
                && connection != null
                && connection.isOpen();
    }

    ZLinkStreamConnectionState state() {
        return state;
    }

    CompletionStage<Void> connect() {
        synchronized (connectionAttemptLock) {
            if (state == ZLinkStreamConnectionState.CLOSED) {
                //  Spec 32 6: connecting a closed connector fails. There is
                //  no connection and there will not be one, so the code the
                //  caller reads is Disconnected.
                throw ZLinkStreamException.disconnected("connector is closed");
            }
            if (isConnected()) {
                return CompletableFuture.completedFuture(null);
            }
            if ((state == ZLinkStreamConnectionState.CONNECTING
                            || state == ZLinkStreamConnectionState.RECONNECTING)
                    && connectionAttempt != null) {
                return connectionAttempt;
            }
        }
        return startConnectionAttempt(
                ZLinkStreamConnectionState.CONNECTING, this::connectOnceStage);
    }

    CompletionStage<Void> close() {
        boolean wasConnected;
        boolean stateChanged;
        ZLinkStreamTransportConnection current;
        synchronized (connectionAttemptLock) {
            wasConnected = isConnected();
            current = connection;
            connection = null;
            stateChanged = setStateLocked(ZLinkStreamConnectionState.CLOSED);
        }
        stopHeartbeat();
        closeQuietly(current);
        pendingRequests.failAll(ZLinkStreamException.disconnected("connector closed"));
        dispatchQueue.clear();
        if (stateChanged) {
            notifyStateHandlers(ZLinkStreamConnectionState.CLOSED);
        }
        if (wasConnected) {
            disconnectedNotifier.run();
        }
        return CompletableFuture.completedFuture(null);
    }

    void serverClosing() {
        ZLinkStreamTransportConnection current;
        synchronized (connectionAttemptLock) {
            current = connection;
            if (current == null || state != ZLinkStreamConnectionState.CONNECTED) {
                return;
            }
            //  Claiming the connection under the lock is what makes this
            //  ending run once, whichever thread reaches it first.
            connection = null;
        }
        stopHeartbeat();
        closeQuietly(current);
        pendingRequests.failAll(ZLinkStreamException.disconnected("server closed the session"));
        //  Spec 32 10.1.1: a wait is released when the connection it observed
        //  ends, here, and not when the reconnect that may follow establishes
        //  the next one.
        dispatchQueue.connectionEnded();
        boolean reconnectEnabled = configuration.reconnect().enabled();
        if (reconnectEnabled) {
            startAutomaticReconnect();
        } else {
            transitionTo(ZLinkStreamConnectionState.DISCONNECTED);
        }
        disconnectedNotifier.run();
    }

    CompletionStage<Void> writeAsync(byte[] frame) {
        ZLinkStreamTransportConnection current = connection;
        if (current == null) {
            return CompletableFuture.failedFuture(
                    ZLinkStreamException.disconnected("connector is not connected"));
        }
        return current.writeAsync(frame)
                .handle(
                        (ignored, failure) -> {
                            if (failure == null) return (Void) null;
                            if (handleReceiveFailure(
                                    current,
                                    ZLinkStreamException.disconnected("transport write failed"))) {
                                throw ZLinkStreamException.of(
                                        ZLinkStreamErrorCode.SEND_FAILED,
                                        "Transport write failed.",
                                        failure);
                            }
                            throw ZLinkStreamException.disconnected(
                                    "connection ended before the write completed");
                        });
    }

    AutoCloseable onConnectionStateChanged(ZLinkStreamConnectionStateHandler handler) {
        Objects.requireNonNull(handler, "handler");
        stateHandlers.add(handler);
        return () -> stateHandlers.remove(handler);
    }

    private CompletionStage<Void> connectOnceStage() {
        return switch (configuration.transport().kind()) {
            case WEB_SOCKET, WEB_SOCKET_SECURE -> connectWebSocketStage();
            case TLS ->
                    ZLinkTlsTransportConnection.connectStage(
                                    configuration.endpoint(),
                                    configuration.timeouts().connect(),
                                    configuration.limits().receivePayload(),
                                    configuration.transport().skipServerCertificateValidation())
                            .thenAccept(this::activateConnection);
            case TCP -> connectTcpStage();
        };
    }

    private CompletionStage<Void> connectTcpStage() {
        CompletableFuture<Void> result = new CompletableFuture<>();
        AsynchronousSocketChannel channel;
        try {
            channel = AsynchronousSocketChannel.open();
            channel.setOption(StandardSocketOptions.SO_KEEPALIVE, true);
        } catch (IOException ex) {
            return CompletableFuture.failedFuture(ex);
        }

        var timeout =
                timeouts.schedule(
                        () ->
                                result.completeExceptionally(
                                        ZLinkStreamException.of(
                                                ZLinkStreamErrorCode.CONNECT_TIMEOUT,
                                                "connect timed out after "
                                                        + configuration.timeouts().connect(),
                                                new TimeoutException(
                                                        "connect timed out after "
                                                                + configuration
                                                                        .timeouts()
                                                                        .connect()))),
                        configuration.timeouts().connect().toMillis(),
                        TimeUnit.MILLISECONDS);

        InetSocketAddress address =
                new InetSocketAddress(
                        configuration.endpoint().getHost(),
                        DefaultZLinkStreamConnector.resolvePort(configuration.endpoint()));
        DefaultZLinkStreamConnector.trace(
                "connector connect-start endpoint="
                        + configuration.endpoint()
                        + " address="
                        + address);
        channel.connect(
                address,
                null,
                new CompletionHandler<Void, Void>() {
                    @Override
                    public void completed(Void ignored, Void attachment) {
                        timeout.cancel(false);
                        if (state == ZLinkStreamConnectionState.CLOSED) {
                            closeRawQuietly(channel);
                            result.completeExceptionally(
                                    ZLinkStreamException.disconnected("connector is closed"));
                            return;
                        }
                        ZLinkTcpTransportConnection tcp =
                                new ZLinkTcpTransportConnection(
                                        channel, configuration.limits().receivePayload());
                        activateConnection(tcp);
                        DefaultZLinkStreamConnector.trace(
                                "connector connect-complete endpoint=" + configuration.endpoint());
                        result.complete(null);
                    }

                    @Override
                    public void failed(Throwable exc, Void attachment) {
                        timeout.cancel(false);
                        closeRawQuietly(channel);
                        DefaultZLinkStreamConnector.trace(
                                "connector connect-failed endpoint="
                                        + configuration.endpoint()
                                        + " error="
                                        + exc);
                        result.completeExceptionally(exc);
                    }
                });
        result.whenComplete(
                (ignored, ex) -> {
                    if (ex != null) {
                        closeRawQuietly(channel);
                    }
                });
        return result;
    }

    private CompletionStage<Void> connectWebSocketStage() {
        HttpClient.Builder clientBuilder =
                HttpClient.newBuilder().connectTimeout(configuration.timeouts().connect());
        if (configuration.transport().skipServerCertificateValidation()) {
            clientBuilder.sslContext(insecureSslContext());
        }
        return ZLinkWebSocketTransportConnection.connectStage(
                        clientBuilder.build(),
                        configuration.endpoint(),
                        configuration.limits().receivePayload())
                .thenAccept(
                        ws -> {
                            if (state == ZLinkStreamConnectionState.CLOSED) {
                                closeQuietly(ws);
                                throw ZLinkStreamException.disconnected("connector is closed");
                            }
                            activateConnection(ws);
                            DefaultZLinkStreamConnector.trace(
                                    "connector connect-complete endpoint="
                                            + configuration.endpoint());
                        });
    }

    private void activateConnection(ZLinkStreamTransportConnection transport) {
        boolean closed;
        boolean stateChanged = false;
        synchronized (connectionAttemptLock) {
            closed = state == ZLinkStreamConnectionState.CLOSED;
            if (!closed) {
                //  Reading the state and publishing the connection in one
                //  step is what keeps a close that arrives in between from
                //  leaving a read loop on a closed connector.
                connection = transport;
                lastInboundNanos = System.nanoTime();
                stateChanged = setStateLocked(ZLinkStreamConnectionState.CONNECTED);
            }
        }
        if (closed) {
            //  Closing the transport stays outside the lock: it takes the
            //  transport monitor, and no lock of this class is held while
            //  another object's monitor is taken.
            closeQuietly(transport);
            throw ZLinkStreamException.disconnected("connector is closed");
        }
        //  Spec 32 10: receivedCount is measured from the moment a
        //  connection is established, so every reconnect starts again at 0,
        //  and whatever the previous connection left unconsumed goes with
        //  it. Keeping the queue would let waitFor hand back a packet from
        //  before the drop as if it belonged to the new connection. The
        //  waits of the previous connection are not this call's concern:
        //  they ended with that connection (spec 32 10.1.1).
        dispatchQueue.resetForNewConnection();
        //  A new connection carries no outstanding write, so the send chain
        //  of the connection that ended does not hold up this one.
        connectionEstablishedNotifier.run();
        if (stateChanged) {
            notifyStateHandlers(ZLinkStreamConnectionState.CONNECTED);
        }
        readNextFrame(transport);
        startHeartbeat();
    }

    private void readNextFrame(ZLinkStreamTransportConnection transport) {
        transport
                .readFrameAsync()
                .whenComplete(
                        (frame, ex) -> {
                            if (ex != null) {
                                handleReceiveFailure(transport, ex);
                                return;
                            }
                            try {
                                lastInboundNanos = System.nanoTime();
                                receiveDispatcher.dispatch(frame.header(), frame.payload());
                                if (connection == transport
                                        && state == ZLinkStreamConnectionState.CONNECTED) {
                                    readNextFrame(transport);
                                }
                            } catch (RuntimeException dispatchEx) {
                                errorPublisher.accept(
                                        new ZLinkStreamError(
                                                ZLinkStreamErrorCode.FRAME_DECODE_FAILED,
                                                "Receive frame dispatch failed.",
                                                dispatchEx));
                                handleReceiveFailure(transport, dispatchEx);
                            }
                        });
    }

    private boolean handleReceiveFailure(ZLinkStreamTransportConnection failed, Throwable ex) {
        synchronized (connectionAttemptLock) {
            if (connection != failed || state != ZLinkStreamConnectionState.CONNECTED) {
                return false;
            }
            //  The receive loop thread and the heartbeat thread both reach
            //  this for the same ending. Claiming the connection under the
            //  lock holds the notification below to one run: the loser reads
            //  null and returns. Spec 32 6 still expects the second run that
            //  reports exhausted reconnect attempts, which is a different
            //  event and happens elsewhere.
            connection = null;
        }
        stopHeartbeat();
        closeQuietly(failed);
        pendingRequests.failAll(ex);
        //  Spec 32 10.1.1: the wait ends with its connection, not with the
        //  next one.
        dispatchQueue.connectionEnded();
        boolean reconnectEnabled = configuration.reconnect().enabled();
        if (reconnectEnabled) {
            startAutomaticReconnect();
        } else {
            transitionTo(ZLinkStreamConnectionState.DISCONNECTED);
        }
        disconnectedNotifier.run();
        return true;
    }

    private CompletionStage<Void> startConnectionAttempt(
            ZLinkStreamConnectionState targetState, Supplier<CompletionStage<Void>> starter) {
        CompletableFuture<Void> result = new CompletableFuture<>();
        boolean notifyState;
        synchronized (connectionAttemptLock) {
            if (state == ZLinkStreamConnectionState.CLOSED) {
                return CompletableFuture.failedFuture(
                        ZLinkStreamException.disconnected("connector is closed"));
            }
            if (connectionAttempt != null
                    && (state == ZLinkStreamConnectionState.CONNECTING
                            || state == ZLinkStreamConnectionState.RECONNECTING)) {
                return connectionAttempt;
            }
            connectionAttempt = result;
            notifyState = setStateLocked(targetState);
        }
        if (notifyState) {
            notifyStateHandlers(targetState);
        }
        try {
            starter.get()
                    .whenComplete(
                            (ignored, error) -> {
                                boolean transition = false;
                                synchronized (connectionAttemptLock) {
                                    if (connectionAttempt == result) {
                                        connectionAttempt = null;
                                    }
                                    if (error != null
                                            && state != ZLinkStreamConnectionState.CLOSED) {
                                        transition =
                                                setStateLocked(
                                                        ZLinkStreamConnectionState.DISCONNECTED);
                                    }
                                }
                                if (transition) {
                                    notifyStateHandlers(ZLinkStreamConnectionState.DISCONNECTED);
                                }
                                if (error == null) {
                                    result.complete(null);
                                } else {
                                    //  Spec 32 6.2: a connect attempt that never produced a
                                    //  connection still leaves a close reason behind.
                                    connectFailureRecorder.run();
                                    result.completeExceptionally(error);
                                }
                            });
        } catch (RuntimeException error) {
            boolean transition;
            synchronized (connectionAttemptLock) {
                if (connectionAttempt == result) {
                    connectionAttempt = null;
                }
                transition =
                        state != ZLinkStreamConnectionState.CLOSED
                                && setStateLocked(ZLinkStreamConnectionState.DISCONNECTED);
            }
            if (transition) {
                notifyStateHandlers(ZLinkStreamConnectionState.DISCONNECTED);
            }
            connectFailureRecorder.run();
            result.completeExceptionally(error);
        }
        return result;
    }

    private void startAutomaticReconnect() {
        CompletableFuture<Void> currentAttempt;
        boolean notifyReconnect = false;
        synchronized (connectionAttemptLock) {
            if (state == ZLinkStreamConnectionState.CLOSED) {
                return;
            }
            currentAttempt = connectionAttempt;
            if (currentAttempt != null) {
                notifyReconnect = setStateLocked(ZLinkStreamConnectionState.RECONNECTING);
            }
        }
        if (currentAttempt != null) {
            if (notifyReconnect) {
                notifyStateHandlers(ZLinkStreamConnectionState.RECONNECTING);
                currentAttempt.whenComplete((ignored, error) -> startAutomaticReconnect());
            }
            return;
        }
        startConnectionAttempt(
                ZLinkStreamConnectionState.RECONNECTING,
                () -> reconnectAttemptStage(1, configuration.reconnect().initialDelay()));
    }

    /**
     * One reconnect attempt. {@code base} is the deterministic backoff delay for this attempt; the
     * time actually waited is a value between 50% and 100% of it, so clients that lost the same
     * server do not all return at once (spec 32 6).
     */
    private CompletableFuture<Void> reconnectAttemptStage(int attempt, Duration base) {
        CompletableFuture<Void> result = new CompletableFuture<>();
        Duration delay = ZLinkStreamReconnectDelay.jittered(base);
        timeouts.schedule(
                () -> {
                    if (state == ZLinkStreamConnectionState.CLOSED) {
                        result.completeExceptionally(
                                ZLinkStreamException.disconnected("connector is closed"));
                        return;
                    }
                    connectOnceStage()
                            .whenComplete(
                                    (ignored, ex) -> {
                                        if (ex == null) {
                                            result.complete(null);
                                            return;
                                        }
                                        if (reconnectAttemptsExhausted(attempt)) {
                                            //  Spec 32 6: when the attempts run out the state
                                            //  becomes Disconnected and the disconnect handler
                                            // runs.
                                            //  The handler already ran once when the transport was
                                            //  lost; this second call is what tells a client that
                                            //  watched the connector go to Reconnecting that the
                                            //  connection is not coming back.
                                            transitionTo(ZLinkStreamConnectionState.DISCONNECTED);
                                            disconnectedNotifier.run();
                                            result.completeExceptionally(ex);
                                            return;
                                        }
                                        reconnectAttemptStage(attempt + 1, nextReconnectDelay(base))
                                                .whenComplete(
                                                        (retryIgnored, retryEx) -> {
                                                            if (retryEx == null) {
                                                                result.complete(null);
                                                            } else {
                                                                result.completeExceptionally(
                                                                        retryEx);
                                                            }
                                                        });
                                    });
                },
                delay.toMillis(),
                TimeUnit.MILLISECONDS);
        return result;
    }

    private Duration nextReconnectDelay(Duration currentBase) {
        return ZLinkStreamReconnectDelay.nextBase(currentBase, configuration.reconnect());
    }

    private boolean reconnectAttemptsExhausted(int attempt) {
        int maxAttempts = configuration.reconnect().maxAttempts();
        return maxAttempts != ZLinkStreamConnectorOptions.UNLIMITED_RECONNECT_ATTEMPTS
                && attempt >= maxAttempts;
    }

    private void startHeartbeat() {
        stopHeartbeat();
        if (!configuration.heartbeat().enabled()) {
            return;
        }
        heartbeatTask =
                timeouts.scheduleAtFixedRate(
                        this::heartbeatTick,
                        configuration.heartbeat().interval().toMillis(),
                        configuration.heartbeat().interval().toMillis(),
                        TimeUnit.MILLISECONDS);
    }

    private void stopHeartbeat() {
        ScheduledFuture<?> current = heartbeatTask;
        heartbeatTask = null;
        if (current != null) {
            current.cancel(false);
        }
    }

    private void heartbeatTick() {
        ZLinkStreamTransportConnection current = connection;
        if (current == null || state != ZLinkStreamConnectionState.CONNECTED) {
            return;
        }
        long idleNanos = System.nanoTime() - lastInboundNanos;
        if (idleNanos > configuration.heartbeat().timeout().toNanos()) {
            closeReasonRecorder.accept(ZLinkStreamCloseReason.HEARTBEAT_TIMEOUT);
            //  The transport is gone, so spec 32 9 fails the operations in
            //  flight as Disconnected; the heartbeat timeout is the cause.
            handleReceiveFailure(
                    current,
                    ZLinkStreamException.of(
                            ZLinkStreamErrorCode.DISCONNECTED,
                            "Heartbeat timed out",
                            new TimeoutException("Heartbeat timed out")));
            return;
        }
        controlSender.apply("$zlink.heartbeat.ping");
    }

    private void transitionTo(ZLinkStreamConnectionState next) {
        boolean changed;
        synchronized (connectionAttemptLock) {
            changed = setStateLocked(next);
        }
        if (!changed) {
            return;
        }
        notifyStateHandlers(next);
    }

    private boolean setStateLocked(ZLinkStreamConnectionState next) {
        if (state == next) {
            return false;
        }
        state = next;
        return true;
    }

    private void notifyStateHandlers(ZLinkStreamConnectionState next) {
        //  stateHandlers is a CopyOnWriteArrayList, so its iterator is
        //  already the snapshot this loop needs.
        for (ZLinkStreamConnectionStateHandler handler : stateHandlers) {
            if (configuration.dispatchMode() == ZLinkStreamDispatchMode.IMMEDIATE) {
                invokeStateCallback(handler, next);
            } else {
                dispatchQueue.addAsync(() -> invokeStateCallback(handler, next));
            }
        }
    }

    private CompletionStage<Void> invokeStateCallback(
            ZLinkStreamConnectionStateHandler handler, ZLinkStreamConnectionState next) {
        try {
            return handler.handleAsync(next)
                    .exceptionally(
                            ex -> {
                                errorPublisher.accept(
                                        DefaultZLinkStreamConnector.userCallbackFailed(ex));
                                return null;
                            });
        } catch (Throwable ex) {
            errorPublisher.accept(DefaultZLinkStreamConnector.userCallbackFailed(ex));
            return CompletableFuture.completedFuture(null);
        }
    }

    private boolean isWebSocketEndpoint() {
        return configuration.transport().kind() == ZLinkStreamTransport.WEB_SOCKET
                || configuration.transport().kind() == ZLinkStreamTransport.WEB_SOCKET_SECURE;
    }

    private static SSLContext insecureSslContext() {
        try {
            TrustManager[] trustAllManagers =
                    new TrustManager[] {
                        new X509TrustManager() {
                            @Override
                            public void checkClientTrusted(
                                    X509Certificate[] chain, String authType) {}

                            @Override
                            public void checkServerTrusted(
                                    X509Certificate[] chain, String authType) {}

                            @Override
                            public X509Certificate[] getAcceptedIssuers() {
                                return new X509Certificate[0];
                            }
                        }
                    };
            SSLContext context = SSLContext.getInstance("TLS");
            context.init(null, trustAllManagers, new SecureRandom());
            return context;
        } catch (GeneralSecurityException ex) {
            throw ZLinkStreamException.configurationError(
                    "failed to create insecure TLS context", ex);
        }
    }

    private static void closeQuietly(ZLinkStreamTransportConnection connection) {
        if (connection != null) {
            connection.close();
        }
    }

    private static void closeRawQuietly(AsynchronousSocketChannel channel) {
        if (channel == null) {
            return;
        }
        try {
            channel.close();
        } catch (IOException ignored) {
        }
    }
}
