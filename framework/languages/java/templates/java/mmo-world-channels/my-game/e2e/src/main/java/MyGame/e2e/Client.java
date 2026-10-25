package MyGame.e2e;

import MyGame.*;

import systems.zlink.stream.connector.*;

import java.net.URI;
import java.time.Duration;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.Predicate;

public final class Client implements AutoCloseable {
    static final long MESSAGE_SECONDS = 5;
    final String playerId;
    final ZLinkStreamConnector connector;
    private final LinkedBlockingQueue<Object> queue = new LinkedBlockingQueue<>();
    private final List<Object> pending = new ArrayList<>();
    final List<Object> history = Collections.synchronizedList(new ArrayList<>());
    private final List<ChannelView> channelViews = new CopyOnWriteArrayList<>();
    final AtomicInteger disconnects = new AtomicInteger();
    private final List<AutoCloseable> registrations = new ArrayList<>();
    private boolean closed;

    public Client(String endpoint, String playerId) {
        this.playerId = playerId;
        var uri = URI.create(endpoint);
        var d = ZLinkStreamConnectorOptions.createDefault(uri);
        connector =
                ZLinkStreamConnectorFactory.create(
                        new ZLinkStreamConnectorOptions(
                                uri,
                                ZLinkStreamDispatchMode.IMMEDIATE,
                                Duration.ofSeconds(MESSAGE_SECONDS),
                                d.maxReconnectAttempts(),
                                d.connectTimeout(),
                                d.maxSendPayloadSize(),
                                d.heartbeatEnabled(),
                                d.heartbeatInterval(),
                                d.heartbeatTimeout(),
                                false,
                                d.reconnectInitialDelay(),
                                d.reconnectMaxDelay(),
                                d.reconnectBackoffFactor()));
        register(PlayerView.class);
        register(ChannelView.class);
        register(ActionRejected.class);
        registrations.add(
                connector.onDisconnected(
                        event -> {
                            disconnects.incrementAndGet();
                            queue.add(new IllegalStateException("Disconnected: " + event));
                            return CompletableFuture.completedFuture(null);
                        }));
        registrations.add(
                connector.onErrorReceived(
                        event -> {
                            queue.add(new IllegalStateException("Connector error: " + event));
                            return CompletableFuture.completedFuture(null);
                        }));
    }

    private <T> void register(Class<T> type) {
        registrations.add(
                connector.on(
                        type,
                        message -> {
                            var payload = message.payload();
                            history.add(payload);
                            if (payload instanceof ChannelView channelView) channelViews.add(channelView);
                            queue.add(payload);
                            return CompletableFuture.completedFuture(null);
                        }));
    }

    PlayerView connect() throws Exception {
        connector.connect().submit().toCompletableFuture().join();
        send(new EnterWorldRequest(playerId));
        return next(PlayerView.class);
    }

    SessionInfo sessionInfo() {
        return connector
                .request(new SessionInfoRequest())
                .submit(SessionInfo.class)
                .toCompletableFuture()
                .join();
    }

    void send(Object message) {
        connector.send(message).submit().toCompletableFuture().join();
    }

    ChannelView channelView() throws Exception {
        int from = markChannelViews();
        send(new ChannelViewRequest());
        return channelViewAfter(from, value -> true);
    }

    int markChannelViews() {
        return channelViews.size();
    }

    ChannelView channelViewAfter(int from, Predicate<ChannelView> predicate) throws Exception {
        return next(
                ChannelView.class,
                value -> channelViewPosition(value) >= from && predicate.test(value));
    }

    private int channelViewPosition(ChannelView expected) {
        for (int i = 0; i < channelViews.size(); i++) {
            if (channelViews.get(i) == expected) return i;
        }
        return -1;
    }

    ActionRejected rejection() throws Exception {
        return next(ActionRejected.class);
    }

    void waitRetryAfter(int retryAfterMs) throws InterruptedException {
        if (retryAfterMs <= 0) throw new IllegalArgumentException("RetryAfterMs must be positive");
        Thread.sleep(retryAfterMs);
    }

    <T> T next(Class<T> type) throws Exception {
        return next(type, value -> true);
    }

    <T> T next(Class<T> type, Predicate<T> predicate) throws Exception {
        long expires = System.nanoTime() + TimeUnit.SECONDS.toNanos(MESSAGE_SECONDS);
        while (true) {
            for (int i = 0; i < pending.size(); i++) {
                Object queued = pending.get(i);
                if (queued instanceof Exception error) throw error;
                if (type.isInstance(queued)) {
                    T value = type.cast(queued);
                    if (predicate.test(value)) {
                        pending.remove(i);
                        return value;
                    }
                }
            }
            Object event =
                    queue.poll(Math.max(0, expires - System.nanoTime()), TimeUnit.NANOSECONDS);
            if (event == null)
                throw new IllegalStateException(
                        "Timed out waiting for " + type.getSimpleName() + ", received=" + history);
            if (event instanceof Exception error) throw error;
            if (type.isInstance(event)) {
                T value = type.cast(event);
                if (predicate.test(value)) return value;
            }
            pending.add(event);
        }
    }

    public void close() throws Exception {
        if (closed) return;
        closed = true;
        Exception failure = null;
        try {
            connector.close().submit().toCompletableFuture().join();
        } catch (Exception error) {
            failure = error;
        }
        for (var registration : registrations)
            try {
                registration.close();
            } catch (Exception error) {
                if (failure == null) failure = error;
                else failure.addSuppressed(error);
            }
        if (failure != null) throw failure;
    }
}
