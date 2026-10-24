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
    final List<Object> history = Collections.synchronizedList(new ArrayList<>());
    final AtomicInteger disconnects = new AtomicInteger();
    private final List<AutoCloseable> registrations = new ArrayList<>();

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
                            history.add(message.payload());
                            queue.add(message.payload());
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

    <T> T next(Class<T> type) throws Exception {
        return next(type, value -> true);
    }

    <T> T next(Class<T> type, Predicate<T> predicate) throws Exception {
        long expires = System.nanoTime() + TimeUnit.SECONDS.toNanos(MESSAGE_SECONDS);
        while (true) {
            Object event =
                    queue.poll(Math.max(0, expires - System.nanoTime()), TimeUnit.NANOSECONDS);
            if (event == null)
                throw new IllegalStateException(
                        "Timed out waiting for " + type.getSimpleName() + ", received=" + history);
            if (event instanceof Exception error) throw error;
            if (!type.isInstance(event))
                throw new IllegalStateException("Unexpected event: " + event);
            T value = type.cast(event);
            if (predicate.test(value)) return value;
        }
    }

    public void close() throws Exception {
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
