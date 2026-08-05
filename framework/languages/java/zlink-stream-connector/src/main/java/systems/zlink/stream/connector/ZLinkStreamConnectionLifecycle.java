package systems.zlink.stream.connector;

import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.StandardSocketOptions;
import java.net.http.HttpClient;
import java.nio.channels.AsynchronousSocketChannel;
import java.nio.channels.CompletionHandler;
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
import javax.net.ssl.SSLContext;
import javax.net.ssl.TrustManager;
import javax.net.ssl.X509TrustManager;

final class ZLinkStreamConnectionLifecycle {
    private final ZLinkStreamConnectorConfiguration configuration;
    private final ScheduledExecutorService timeouts;
    private final ZLinkStreamDispatchQueue dispatchQueue;
    private final ZLinkStreamPendingRequests pendingRequests;
    private final ZLinkStreamInboundObserverDispatcher inboundObservers;
    private final ZLinkStreamReceiveDispatcher receiveDispatcher;
    private final Consumer<ZLinkStreamError> errorPublisher;
    private final Runnable disconnectedNotifier;
    private final Consumer<ZLinkStreamCloseReason> closeReasonRecorder;
    private final Function<String, CompletionStage<Void>> controlSender;
    private final List<ZLinkStreamConnectionStateHandler> stateHandlers = new CopyOnWriteArrayList<>();
    private final Object connectionAttemptLock = new Object();

    private volatile ZLinkStreamConnectionState state = ZLinkStreamConnectionState.CREATED;
    private volatile ZLinkStreamTransportConnection connection;
    private volatile ScheduledFuture<?> heartbeatTask;
    private volatile long lastInboundNanos = System.nanoTime();
    private volatile boolean connectStarted;
    private volatile CompletableFuture<Void> connectionAttempt;

    ZLinkStreamConnectionLifecycle(
        ZLinkStreamConnectorConfiguration configuration,
        ScheduledExecutorService timeouts,
        ZLinkStreamDispatchQueue dispatchQueue,
        ZLinkStreamPendingRequests pendingRequests,
        ZLinkStreamInboundObserverDispatcher inboundObservers,
        ZLinkStreamReceiveDispatcher receiveDispatcher,
        Consumer<ZLinkStreamError> errorPublisher,
        Runnable disconnectedNotifier,
        Consumer<ZLinkStreamCloseReason> closeReasonRecorder,
        Function<String, CompletionStage<Void>> controlSender) {
        this.configuration = configuration;
        this.timeouts = timeouts;
        this.dispatchQueue = dispatchQueue;
        this.pendingRequests = pendingRequests;
        this.inboundObservers = inboundObservers;
        this.receiveDispatcher = receiveDispatcher;
        this.errorPublisher = errorPublisher;
        this.disconnectedNotifier = disconnectedNotifier;
        this.closeReasonRecorder = closeReasonRecorder;
        this.controlSender = controlSender;
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
                throw new IllegalStateException("connector is closed");
            }
            if (isConnected()) {
                return CompletableFuture.completedFuture(null);
            }
            if ((state == ZLinkStreamConnectionState.CONNECTING
                || state == ZLinkStreamConnectionState.RECONNECTING)
                && connectionAttempt != null) {
                return connectionAttempt;
            }
            connectStarted = true;
            transitionTo(ZLinkStreamConnectionState.CONNECTING);
            return startConnectionAttempt(this::connectOnceStage);
        }
    }

    CompletionStage<Void> close() {
        boolean wasConnected = isConnected();
        ZLinkStreamTransportConnection current = connection;
        connection = null;
        stopHeartbeat();
        closeQuietly(current);
        pendingRequests.failAll(new IOException("connector closed"));
        dispatchQueue.clear();
        inboundObservers.close();
        transitionTo(ZLinkStreamConnectionState.CLOSED);
        if (wasConnected) {
            disconnectedNotifier.run();
        }
        return CompletableFuture.completedFuture(null);
    }

    void serverClosing() {
        ZLinkStreamTransportConnection current = connection;
        if (current == null || state != ZLinkStreamConnectionState.CONNECTED) {
            return;
        }
        connection = null;
        stopHeartbeat();
        closeQuietly(current);
        pendingRequests.failAll(new IOException("server closed the session"));
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
            return CompletableFuture.failedFuture(new IllegalStateException("connector is not connected"));
        }
        return current.writeAsync(frame);
    }

    AutoCloseable onConnectionStateChanged(ZLinkStreamConnectionStateHandler handler) {
        Objects.requireNonNull(handler, "handler");
        stateHandlers.add(handler);
        return () -> stateHandlers.remove(handler);
    }

    void requireCanRegisterInboundObserver() {
        if (connectStarted) {
            throw new IllegalStateException("inbound observers must be registered before connecting");
        }
    }

    private CompletionStage<Void> connectOnceStage() {
        return switch (configuration.transport().kind()) {
            case WEB_SOCKET, WEB_SOCKET_SECURE -> connectWebSocketStage();
            case TLS -> ZLinkTlsTransportConnection.connectStage(
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

        var timeout = timeouts.schedule(
            () -> result.completeExceptionally(
                new TimeoutException("connect timed out after " + configuration.timeouts().connect())),
            configuration.timeouts().connect().toMillis(),
            TimeUnit.MILLISECONDS);

        InetSocketAddress address = new InetSocketAddress(
            configuration.endpoint().getHost(),
            DefaultZLinkStreamConnector.resolvePort(configuration.endpoint()));
        DefaultZLinkStreamConnector.trace(
            "connector connect-start endpoint=" + configuration.endpoint() + " address=" + address);
        channel.connect(address, null, new CompletionHandler<Void, Void>() {
            @Override
            public void completed(Void ignored, Void attachment) {
                timeout.cancel(false);
                if (state == ZLinkStreamConnectionState.CLOSED) {
                    closeRawQuietly(channel);
                    result.completeExceptionally(new IllegalStateException("connector is closed"));
                    return;
                }
                ZLinkTcpTransportConnection tcp = new ZLinkTcpTransportConnection(
                    channel,
                    configuration.limits().receivePayload());
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
                    "connector connect-failed endpoint=" + configuration.endpoint() + " error=" + exc);
                result.completeExceptionally(exc);
            }
        });
        result.whenComplete((ignored, ex) -> {
            if (ex != null) {
                closeRawQuietly(channel);
            }
        });
        return result;
    }

    private CompletionStage<Void> connectWebSocketStage() {
        HttpClient.Builder clientBuilder = HttpClient.newBuilder()
            .connectTimeout(configuration.timeouts().connect());
        if (configuration.transport().skipServerCertificateValidation()) {
            clientBuilder.sslContext(insecureSslContext());
        }
        return ZLinkWebSocketTransportConnection.connectStage(
            clientBuilder.build(),
            configuration.endpoint(),
            configuration.limits().receivePayload()).thenAccept(ws -> {
                if (state == ZLinkStreamConnectionState.CLOSED) {
                    closeQuietly(ws);
                    throw new IllegalStateException("connector is closed");
                }
                activateConnection(ws);
                DefaultZLinkStreamConnector.trace(
                    "connector connect-complete endpoint=" + configuration.endpoint());
            });
    }

    private void activateConnection(ZLinkStreamTransportConnection transport) {
        connection = transport;
        lastInboundNanos = System.nanoTime();
        transitionTo(ZLinkStreamConnectionState.CONNECTED);
        readNextFrame(transport);
        startHeartbeat();
    }

    private void readNextFrame(ZLinkStreamTransportConnection transport) {
        transport.readFrameAsync().whenComplete((frame, ex) -> {
            if (ex != null) {
                handleReceiveFailure(transport, ex);
                return;
            }
            try {
                lastInboundNanos = System.nanoTime();
                receiveDispatcher.dispatch(frame.header(), frame.payload());
                if (connection == transport && state == ZLinkStreamConnectionState.CONNECTED) {
                    readNextFrame(transport);
                }
            } catch (RuntimeException dispatchEx) {
                errorPublisher.accept(new ZLinkStreamError(
                    ZLinkStreamErrorCode.FRAME_DECODE_FAILED,
                    "Receive frame dispatch failed.",
                    dispatchEx));
                handleReceiveFailure(transport, dispatchEx);
            }
        });
    }

    private void handleReceiveFailure(ZLinkStreamTransportConnection failed, Throwable ex) {
        if (connection != failed || state != ZLinkStreamConnectionState.CONNECTED) {
            return;
        }
        connection = null;
        stopHeartbeat();
        closeQuietly(failed);
        pendingRequests.failAll(ex);
        boolean reconnectEnabled = configuration.reconnect().enabled();
        if (reconnectEnabled) {
            startAutomaticReconnect();
        } else {
            transitionTo(ZLinkStreamConnectionState.DISCONNECTED);
        }
        disconnectedNotifier.run();
    }

    private CompletionStage<Void> startConnectionAttempt(
        java.util.function.Supplier<CompletionStage<Void>> starter) {
        CompletableFuture<Void> result = new CompletableFuture<>();
        connectionAttempt = result;
        try {
            starter.get().whenComplete((ignored, error) -> {
                synchronized (connectionAttemptLock) {
                    if (connectionAttempt == result) {
                        connectionAttempt = null;
                    }
                    if (error != null && state != ZLinkStreamConnectionState.CLOSED) {
                        transitionTo(ZLinkStreamConnectionState.DISCONNECTED);
                    }
                }
                if (error == null) {
                    result.complete(null);
                } else {
                    result.completeExceptionally(error);
                }
            });
        } catch (RuntimeException error) {
            connectionAttempt = null;
            transitionTo(ZLinkStreamConnectionState.DISCONNECTED);
            result.completeExceptionally(error);
        }
        return result;
    }

    private void startAutomaticReconnect() {
        synchronized (connectionAttemptLock) {
            if (state == ZLinkStreamConnectionState.CLOSED) {
                return;
            }
            boolean reconnectAlreadyStarted = state == ZLinkStreamConnectionState.RECONNECTING;
            transitionTo(ZLinkStreamConnectionState.RECONNECTING);
            CompletableFuture<Void> currentAttempt = connectionAttempt;
            if (currentAttempt != null) {
                if (!reconnectAlreadyStarted) {
                    currentAttempt.whenComplete((ignored, error) -> startAutomaticReconnect());
                }
                return;
            }
            startConnectionAttempt(() -> reconnectAttemptStage(
                1,
                configuration.reconnect().initialDelay()));
        }
    }

    private CompletableFuture<Void> reconnectAttemptStage(int attempt, Duration delay) {
        CompletableFuture<Void> result = new CompletableFuture<>();
        timeouts.schedule(() -> {
            if (state == ZLinkStreamConnectionState.CLOSED) {
                result.completeExceptionally(new IllegalStateException("connector is closed"));
                return;
            }
            ZLinkConnectorMetrics.reconnectAttempt();
            connectOnceStage().whenComplete((ignored, ex) -> {
                if (ex == null) {
                    result.complete(null);
                    return;
                }
                if (reconnectAttemptsExhausted(attempt)) {
                    transitionTo(ZLinkStreamConnectionState.DISCONNECTED);
                    result.completeExceptionally(ex);
                    return;
                }
                reconnectAttemptStage(attempt + 1, nextReconnectDelay(delay))
                    .whenComplete((retryIgnored, retryEx) -> {
                        if (retryEx == null) {
                            result.complete(null);
                        } else {
                            result.completeExceptionally(retryEx);
                        }
                    });
            });
        }, delay.toMillis(), TimeUnit.MILLISECONDS);
        return result;
    }

    private Duration nextReconnectDelay(Duration current) {
        long nextMillis = Math.round(
            current.toMillis() * configuration.reconnect().backoffFactor());
        if (nextMillis <= 0) {
            nextMillis = configuration.reconnect().initialDelay().toMillis();
        }
        return Duration.ofMillis(Math.min(
            nextMillis, configuration.reconnect().maxDelay().toMillis()));
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
        heartbeatTask = timeouts.scheduleAtFixedRate(
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
            handleReceiveFailure(current, new TimeoutException("Heartbeat timed out"));
            return;
        }
        controlSender.apply("$zlink.heartbeat.ping");
    }

    private void transitionTo(ZLinkStreamConnectionState next) {
        if (state == next) {
            return;
        }
        state = next;
        for (ZLinkStreamConnectionStateHandler handler : List.copyOf(stateHandlers)) {
            if (configuration.dispatchMode() == ZLinkStreamDispatchMode.IMMEDIATE) {
                invokeStateCallback(handler, next);
            } else {
                dispatchQueue.addAsync(() -> invokeStateCallback(handler, next));
            }
        }
    }

    private CompletionStage<Void> invokeStateCallback(
        ZLinkStreamConnectionStateHandler handler,
        ZLinkStreamConnectionState next) {
        try {
            return handler.handleAsync(next).exceptionally(ex -> {
                errorPublisher.accept(DefaultZLinkStreamConnector.userCallbackFailed(ex));
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
            TrustManager[] trustAllManagers = new TrustManager[] {
                new X509TrustManager() {
                    @Override
                    public void checkClientTrusted(
                        java.security.cert.X509Certificate[] chain,
                        String authType) {
                    }

                    @Override
                    public void checkServerTrusted(
                        java.security.cert.X509Certificate[] chain,
                        String authType) {
                    }

                    @Override
                    public java.security.cert.X509Certificate[] getAcceptedIssuers() {
                        return new java.security.cert.X509Certificate[0];
                    }
                }
            };
            SSLContext context = SSLContext.getInstance("TLS");
            context.init(null, trustAllManagers, new java.security.SecureRandom());
            return context;
        } catch (java.security.GeneralSecurityException ex) {
            throw new IllegalStateException("failed to create insecure TLS context", ex);
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
