package systems.zlink.framework.runtime.channels;
import java.util.LinkedHashSet;
import java.util.Objects;
import java.util.stream.Collectors;

import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.function.BiConsumer;
import java.util.logging.Level;
import java.util.logging.Logger;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendConnectableSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendContext;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRecvMode;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitor;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSubscriberSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendTopicMessage;
import systems.zlink.framework.runtime.internal.backend.ZLinkChannelBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkMonitoringBackendAdapter;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkReceiveBatchBudget;
import systems.zlink.framework.runtime.internal.service.ZLinkClassicFanoutLiveness;

/** Owns one classic fanout receive path for each configured manual endpoint. */
final class ZLinkManualFanoutRuntime implements AutoCloseable {
    private static final Logger LOGGER =
        Logger.getLogger(ZLinkManualFanoutRuntime.class.getName());

    private final ZLinkChannelBackendAdapter backend;
    private final ZLinkMonitoringBackendAdapter monitoring;
    private final ZLinkBackendContext context;
    private final BiConsumer<String, ZLinkBackendTopicMessage> dispatch;
    private final Map<String, Set<String>> desired = new LinkedHashMap<>();
    private final Map<String, Connection> connections = new LinkedHashMap<>();
    private final ScheduledExecutorService executor =
        Executors.newSingleThreadScheduledExecutor(task -> {
            Thread thread = new Thread(task, "zlink-java-manual-fanout");
            thread.setDaemon(true);
            return thread;
        });
    private volatile boolean running;
    private long receiveCursor;

    ZLinkManualFanoutRuntime(
        ZLinkChannelBackendAdapter backend,
        ZLinkMonitoringBackendAdapter monitoring,
        ZLinkBackendContext context,
        BiConsumer<String, ZLinkBackendTopicMessage> dispatch) {
        this.backend = Objects.requireNonNull(backend, "backend");
        this.monitoring = monitoring;
        this.context = Objects.requireNonNull(context, "context");
        this.dispatch = Objects.requireNonNull(dispatch, "dispatch");
    }

    ZLinkBackendConnectableSocket connections(String channelName) {
        return new ZLinkBackendConnectableSocket() {
            @Override public String name() { return "manualFanoutConnections"; }
            @Override public void bind(String endpoint) {
                throw new UnsupportedOperationException(
                    "manual fanout subscriber connections cannot bind");
            }
            @Override public void connect(String endpoint) {
                connectEndpoint(channelName, endpoint);
            }
            @Override public void disconnect(String endpoint) {
                disconnectEndpoint(channelName, endpoint);
            }
            @Override public void close() { }
        };
    }

    void start() {
        Map<String, List<String>> initial;
        synchronized (this) {
            if (running) return;
            running = true;
            initial = desired.entrySet().stream().collect(
                Collectors.toMap(
                    Map.Entry::getKey,
                    entry -> List.copyOf(entry.getValue()),
                    (left, right) -> left,
                    LinkedHashMap::new));
        }
        initial.forEach((channel, endpoints) ->
            endpoints.forEach(endpoint -> open(channel, endpoint)));
        executor.scheduleAtFixedRate(this::tickSafely, 0, 10, TimeUnit.MILLISECONDS);
    }

    private void connectEndpoint(String channelName, String endpoint) {
        boolean openNow;
        synchronized (this) {
            desired.computeIfAbsent(
                channelName, ignored -> new LinkedHashSet<>())
                .add(endpoint);
            openNow = running;
        }
        if (openNow) {
            open(channelName, endpoint);
        }
    }

    private void disconnectEndpoint(String channelName, String endpoint) {
        synchronized (this) {
            Set<String> endpoints = desired.get(channelName);
            if (endpoints != null) {
                endpoints.remove(endpoint);
                if (endpoints.isEmpty()) desired.remove(channelName);
            }
        }
        remove(connectionId(channelName, endpoint));
    }

    private void open(String channelName, String endpoint) {
        String id = connectionId(channelName, endpoint);
        synchronized (this) {
            if (!running || connections.containsKey(id)) return;
        }
        RoutingId publisherId = manualPublisherId(channelName, endpoint);
        ZLinkBackendSubscriberSocket subscriber = backend.createSubscriberSocket(context);
        ZLinkBackendSocketMonitor monitor = null;
        Connection connection = null;
        try {
            subscriber.setChannelName(channelName);
            subscriber.setSubscription("");
            monitor = monitoring == null
                ? null
                : monitoring.openSocketMonitor(subscriber);
            connection = new Connection(
                channelName, endpoint, publisherId, id, subscriber, monitor);
            synchronized (connection) {
                boolean accepted;
                synchronized (this) {
                    Set<String> endpoints = desired.get(channelName);
                    accepted = running
                        && endpoints != null
                        && endpoints.contains(endpoint)
                        && !connections.containsKey(id);
                    if (accepted) {
                        connections.put(id, connection);
                    }
                }
                if (!accepted) {
                    closeConnection(connection);
                    return;
                }
                connection.liveness.connect(publisherId, id, System.nanoTime());
                if (monitor != null) {
                    Connection acceptedConnection = connection;
                    monitor.onEvent(event -> onMonitorEvent(
                        acceptedConnection, event.event()));
                }
                // remove/closeConnection shares this fence, preventing a
                // disconnect racing with open from reconnecting a closed socket.
                subscriber.connect(endpoint);
            }
        } catch (RuntimeException failure) {
            LOGGER.log(Level.WARNING,
                "manual fanout subscriber connect failed for " + endpoint, failure);
            if (connection != null) {
                replace(connection);
            } else {
                if (monitor != null) {
                    try {
                        monitor.close();
                    } catch (RuntimeException ignored) {
                    }
                }
                try {
                    subscriber.close();
                } catch (RuntimeException ignored) {
                }
            }
        }
    }

    private static void closeConnection(Connection connection) {
        synchronized (connection) {
            connection.liveness.disconnect(
                connection.publisherId, connection.connectionId);
            try {
                connection.subscriber.disconnect(connection.endpoint);
            } catch (RuntimeException ignored) {
            }
            if (connection.monitor != null) connection.monitor.close();
            connection.subscriber.close();
        }
    }

    private void onMonitorEvent(Connection connection, String event) {
        if (!isCurrent(connection)) return;
        if (isTerminatedEvent(event)) {
            replace(connection);
        }
    }

    private void tickSafely() {
        if (!running) return;
        try {
            reconcileDesired();
            receiveAvailable();
            expireConnections();
        } catch (RuntimeException failure) {
            LOGGER.log(Level.WARNING,
                "manual fanout tick failed; the next bounded tick retries", failure);
        }
    }

    private void reconcileDesired() {
        List<String> endpoints = new ArrayList<>();
        synchronized (this) {
            desired.forEach((channel, values) ->
                values.forEach(endpoint -> endpoints.add(
                    channel + "\u0000" + endpoint)));
        }
        endpoints.forEach(value -> {
            int separator = value.indexOf('\u0000');
            open(value.substring(0, separator), value.substring(separator + 1));
        });
    }

    private void receiveAvailable() {
        List<Connection> ordered;
        synchronized (this) {
            ordered = new ArrayList<>(connections.values());
        }
        if (ordered.isEmpty()) return;
        int cursor = (int) Math.floorMod(receiveCursor, ordered.size());
        int idle = 0;
        ZLinkReceiveBatchBudget batch = new ZLinkReceiveBatchBudget();
        while (batch.canReceiveNext() && idle < ordered.size()) {
            Connection connection = ordered.get(cursor);
            cursor = (cursor + 1) % ordered.size();
            synchronized (this) {
                receiveCursor = cursor;
            }
            ZLinkBackendTopicMessage received;
            synchronized (connection) {
                if (!isCurrent(connection)
                    || !connection.subscriber.waitForReadable(Duration.ZERO)) {
                    received = null;
                } else {
                    received = connection.subscriber.subscribe(
                        ZLinkBackendRecvMode.DONT_WAIT);
                }
            }
            if (received == null) {
                idle++;
                continue;
            }
            idle = 0;
            batch.record(ZLinkReceiveBatchBudget.bytesOf(
                received.parts(), received.applicationMetadataSize(),
                received.topic().getBytes(StandardCharsets.UTF_8).length));
            List<byte[]> frames = new ArrayList<>();
            frames.add(received.topic().getBytes(StandardCharsets.UTF_8));
            frames.addAll(received.parts().stream().map(Message::toByteArray).toList());
            try {
                ZLinkClassicFanoutLiveness.ReceiveKind kind;
                synchronized (connection) {
                    if (!isCurrent(connection)) {
                        received.parts().forEach(Message::close);
                        continue;
                    }
                    kind = connection.liveness.receive(
                        connection.publisherId, connection.connectionId,
                        frames, System.nanoTime());
                    connection.ready = connection.liveness.isReady(
                        connection.publisherId);
                }
                if (kind == ZLinkClassicFanoutLiveness.ReceiveKind.APPLICATION) {
                    // Dispatch owns the received message and closes its parts
                    // after queue admission. Do not invoke it under the
                    // connection registry monitor.
                    dispatch.accept(connection.channelName, received);
                } else {
                    received.parts().forEach(Message::close);
                }
            } catch (IllegalArgumentException malformed) {
                received.parts().forEach(Message::close);
                replace(connection);
            }
        }
    }

    private synchronized boolean isCurrent(Connection connection) {
        return connections.get(connection.connectionId) == connection;
    }

    private void expireConnections() {
        long now = System.nanoTime();
        List<Connection> expired = new ArrayList<>();
        List<Connection> current;
        synchronized (this) {
            current = List.copyOf(connections.values());
        }
        for (Connection connection : current) {
            synchronized (connection) {
                if (!connection.liveness.expire(now).isEmpty()) {
                    expired.add(connection);
                }
            }
        }
        expired.forEach(this::replace);
    }

    private void replace(Connection connection) {
        if (!isCurrent(connection)) return;
        remove(connection.connectionId, connection);
    }

    private void remove(String id) {
        Connection connection;
        synchronized (this) {
            connection = connections.remove(id);
        }
        if (connection == null) return;
        synchronized (connection) {
            closeConnection(connection);
        }
    }

    private void remove(String id, Connection expected) {
        synchronized (this) {
            if (connections.get(id) != expected
                || connections.remove(id) != expected) {
                return;
            }
        }
        synchronized (expected) {
            closeConnection(expected);
        }
    }

    synchronized List<PublisherSnapshot> publisherSnapshots(String channelName) {
        return connections.values().stream()
            .filter(connection -> connection.channelName.equals(channelName))
            .map(connection -> new PublisherSnapshot(
                connection.publisherId, connection.ready))
            .toList();
    }

    @Override
    public void close() {
        List<String> ids;
        synchronized (this) {
            running = false;
            ids = List.copyOf(connections.keySet());
        }
        ids.forEach(this::remove);
        executor.shutdownNow();
    }

    private static String connectionId(String channelName, String endpoint) {
        return channelName + "/manual/" + endpoint;
    }

    private static RoutingId manualPublisherId(String channelName, String endpoint) {
        UUID value = UUID.nameUUIDFromBytes(
            (channelName + "\u0000" + endpoint).getBytes(StandardCharsets.UTF_8));
        return RoutingId.from("manual-" + value);
    }

    private static boolean isTerminatedEvent(String event) {
        return "DISCONNECTED".equals(event)
            || "CLOSED".equals(event)
            || "HANDSHAKE_FAILED_NO_DETAIL".equals(event)
            || "HANDSHAKE_FAILED_PROTOCOL".equals(event)
            || "HANDSHAKE_FAILED_AUTH".equals(event)
            || "Disconnected".equals(event)
            || "Closed".equals(event);
    }

    record PublisherSnapshot(RoutingId nodeRid, boolean ready) { }

    private static final class Connection {
        private final String channelName;
        private final String endpoint;
        private final RoutingId publisherId;
        private final String connectionId;
        private final ZLinkBackendSubscriberSocket subscriber;
        private final ZLinkBackendSocketMonitor monitor;
        private final ZLinkClassicFanoutLiveness liveness =
            new ZLinkClassicFanoutLiveness();
        private volatile boolean ready;

        private Connection(
            String channelName,
            String endpoint,
            RoutingId publisherId,
            String connectionId,
            ZLinkBackendSubscriberSocket subscriber,
            ZLinkBackendSocketMonitor monitor) {
            this.channelName = channelName;
            this.endpoint = endpoint;
            this.publisherId = publisherId;
            this.connectionId = connectionId;
            this.subscriber = subscriber;
            this.monitor = monitor;
        }
    }
}
