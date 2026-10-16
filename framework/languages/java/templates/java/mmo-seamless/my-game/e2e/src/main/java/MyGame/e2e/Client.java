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
        register(WorldState.class);
        register(BorderVisible.class);
        register(DungeonOpened.class);
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

    void connect() {
        connector.connect().submit().toCompletableFuture().join();
    }

    WorldState join(int x) throws Exception {
        send(new JoinWorldRequest(playerId, x));
        return next(WorldState.class);
    }

    WorldState move(int x, int sequence) throws Exception {
        send(new MoveRequest(x, sequence));
        return next(WorldState.class, v -> v.sequence() == sequence);
    }

    DungeonOpened dungeon(String id) throws Exception {
        send(new OpenDungeonRequest(id));
        return next(DungeonOpened.class, v -> v.dungeonId().equals(id));
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
        return next(type, v -> true);
    }

    <T> T next(Class<T> type, Predicate<T> predicate) throws Exception {
        long expires = System.nanoTime() + TimeUnit.SECONDS.toNanos(MESSAGE_SECONDS);
        while (true) {
            for (int i = 0; i < pending.size(); i++)
                if (type.isInstance(pending.get(i)) && predicate.test(type.cast(pending.get(i))))
                    return type.cast(pending.remove(i));
            Object event =
                    queue.poll(Math.max(0, expires - System.nanoTime()), TimeUnit.NANOSECONDS);
            if (event == null)
                throw new IllegalStateException(
                        "Timed out waiting for " + type.getSimpleName() + ", received=" + history);
            if (event instanceof Exception error) throw error;
            pending.add(event);
        }
    }

    public void close() throws Exception {
        connector.close().submit().toCompletableFuture().join();
        Exception failure = null;
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
