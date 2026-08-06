package systems.zlink.framework.runtime.channels;

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
        this.backend = java.util.Objects.requireNonNull(backend, "backend");
        this.monitoring = monitoring;
        this.context = java.util.Objects.requireNonNull(context, "context");
        this.dispatch = java.util.Objects.requireNonNull(dispatch, "dispatch");
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

    synchronized void start() {
        if (running) return;
        running = true;
        desired.forEach((channel, endpoints) ->
            List.copyOf(endpoints).forEach(endpoint -> open(channel, endpoint)));
        executor.scheduleAtFixedRate(this::tickSafely, 0, 10, TimeUnit.MILLISECONDS);
    }

    private synchronized void connectEndpoint(String channelName, String endpoint) {
        desired.computeIfAbsent(channelName, ignored -> new java.util.LinkedHashSet<>())
            .add(endpoint);
        if (running) open(channelName, endpoint);
    }

    private synchronized void disconnectEndpoint(String channelName, String endpoint) {
        Set<String> endpoints = desired.get(channelName);
        if (endpoints != null) {
            endpoints.remove(endpoint);
            if (endpoints.isEmpty()) desired.remove(channelName);
        }
        remove(connectionId(channelName, endpoint));
    }

    private synchronized void open(String channelName, String endpoint) {
        String id = connectionId(channelName, endpoint);
        if (!running || connections.containsKey(id)) return;
        RoutingId publisherId = manualPublisherId(channelName, endpoint);
        ZLinkBackendSubscriberSocket subscriber = backend.createSubscriberSocket(context);
        subscriber.setChannelName(channelName);
        subscriber.setSubscription("");
        ZLinkBackendSocketMonitor monitor = monitoring == null
            ? null
            : monitoring.openSocketMonitor(subscriber);
        Connection connection = new Connection(
            channelName, endpoint, publisherId, id, subscriber, monitor);
        connections.put(id, connection);
        connection.liveness.connect(publisherId, id, System.nanoTime());
        if (monitor != null) {
            monitor.onEvent(event -> onMonitorEvent(connection, event.event()));
        }
        try {
            subscriber.connect(endpoint);
        } catch (RuntimeException failure) {
            LOGGER.log(Level.WARNING,
                "manual fanout subscriber connect failed for " + endpoint, failure);
            replace(connection);
        }
    }

    private synchronized void onMonitorEvent(Connection connection, String event) {
        if (connections.get(connection.connectionId) != connection) return;
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

    private synchronized void reconcileDesired() {
        desired.forEach((channel, endpoints) ->
            endpoints.forEach(endpoint -> open(channel, endpoint)));
    }

    private synchronized void receiveAvailable() {
        List<Connection> ordered = new ArrayList<>(connections.values());
        if (ordered.isEmpty()) return;
        int cursor = (int) Math.floorMod(receiveCursor, ordered.size());
        int idle = 0;
        ZLinkReceiveBatchBudget batch = new ZLinkReceiveBatchBudget();
        while (batch.canReceiveNext() && idle < ordered.size()) {
            Connection connection = ordered.get(cursor);
            cursor = (cursor + 1) % ordered.size();
            receiveCursor = cursor;
            if (!connection.subscriber.waitForReadable(Duration.ZERO)) {
                idle++;
                continue;
            }
            ZLinkBackendTopicMessage received = connection.subscriber.subscribe(
                ZLinkBackendRecvMode.DONT_WAIT);
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
                var kind = connection.liveness.receive(
                    connection.publisherId, connection.connectionId,
                    frames, System.nanoTime());
                connection.ready = connection.liveness.isReady(connection.publisherId);
                if (kind == ZLinkClassicFanoutLiveness.ReceiveKind.APPLICATION) {
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

    private synchronized void expireConnections() {
        long now = System.nanoTime();
        List.copyOf(connections.values()).stream()
            .filter(connection -> !connection.liveness.expire(now).isEmpty())
            .forEach(this::replace);
    }

    private synchronized void replace(Connection connection) {
        if (connections.get(connection.connectionId) != connection) return;
        remove(connection.connectionId);
    }

    private synchronized void remove(String id) {
        Connection connection = connections.remove(id);
        if (connection == null) return;
        connection.liveness.disconnect(connection.publisherId, connection.connectionId);
        try { connection.subscriber.disconnect(connection.endpoint); }
        catch (RuntimeException ignored) { }
        if (connection.monitor != null) connection.monitor.close();
        connection.subscriber.close();
    }

    synchronized List<PublisherSnapshot> publisherSnapshots(String channelName) {
        return connections.values().stream()
            .filter(connection -> connection.channelName.equals(channelName))
            .map(connection -> new PublisherSnapshot(
                connection.publisherId, connection.ready))
            .toList();
    }

    @Override
    public synchronized void close() {
        running = false;
        List.copyOf(connections.keySet()).forEach(this::remove);
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
