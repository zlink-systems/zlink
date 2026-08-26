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
import java.util.concurrent.Executor;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.CompletionException;
import java.util.function.BiConsumer;
import java.util.function.Supplier;
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
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

/** Owns one classic fanout receive path for each configured manual endpoint. */
final class ZLinkManualFanoutRuntime implements AutoCloseable {
    private static final Logger LOGGER =
        Logger.getLogger(ZLinkManualFanoutRuntime.class.getName());

    private final ZLinkChannelBackendAdapter backend;
    private final ZLinkMonitoringBackendAdapter monitoring;
    private final ZLinkBackendContext context;
    private final BiConsumer<String, ZLinkBackendTopicMessage> dispatch;
    private final ZLinkStateLane stateLane = new ZLinkStateLane();
    private final Map<String, Set<String>> desired = new LinkedHashMap<>();
    private final Map<String, Connection> connections = new LinkedHashMap<>();
    private final ScheduledExecutorService scheduler;
    private final Executor infrastructureExecutor;
    private ScheduledFuture<?> tickTask;
    private volatile boolean running;
    private long lifecycleEpoch;
    private boolean tickAdmitted;
    private long receiveCursor;

    ZLinkManualFanoutRuntime(
        ZLinkChannelBackendAdapter backend,
        ZLinkMonitoringBackendAdapter monitoring,
        ZLinkBackendContext context,
        ScheduledExecutorService scheduler,
        Executor infrastructureExecutor,
        BiConsumer<String, ZLinkBackendTopicMessage> dispatch) {
        this.backend = Objects.requireNonNull(backend, "backend");
        this.monitoring = monitoring;
        this.context = Objects.requireNonNull(context, "context");
        this.scheduler = Objects.requireNonNull(scheduler, "scheduler");
        this.infrastructureExecutor = Objects.requireNonNull(
            infrastructureExecutor, "infrastructureExecutor");
        this.dispatch = Objects.requireNonNull(dispatch, "dispatch");
    }

    private <T> T inStateLane(Supplier<T> work) {
        try {
            return stateLane.runAsync(work).toCompletableFuture().join();
        } catch (CompletionException failure) {
            Throwable cause = failure.getCause();
            if (cause instanceof RuntimeException runtimeFailure) {
                throw runtimeFailure;
            }
            if (cause instanceof Error error) {
                throw error;
            }
            throw failure;
        }
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
        StartState start = inStateLane(() -> {
            if (running) return null;
            lifecycleEpoch = Math.addExact(lifecycleEpoch, 1);
            running = true;
            Map<String, List<String>> initial = desired.entrySet().stream().collect(
                Collectors.toMap(
                    Map.Entry::getKey,
                    entry -> List.copyOf(entry.getValue()),
                    (left, right) -> left,
                    LinkedHashMap::new));
            return new StartState(initial, lifecycleEpoch);
        });
        if (start == null) return;
        start.initial().forEach((channel, endpoints) ->
            endpoints.forEach(endpoint -> open(channel, endpoint)));
        ScheduledFuture<?> scheduled = scheduler.scheduleAtFixedRate(
            () -> signalTick(start.epoch()), 0, 10, TimeUnit.MILLISECONDS);
        boolean cancel = inStateLane(() -> {
            if (!running || lifecycleEpoch != start.epoch()) {
                return true;
            }
            tickTask = scheduled;
            return false;
        });
        if (cancel) {
            scheduled.cancel(false);
        }
    }

    private void connectEndpoint(String channelName, String endpoint) {
        boolean openNow = inStateLane(() -> {
            desired.computeIfAbsent(
                channelName, ignored -> new LinkedHashSet<>())
                .add(endpoint);
            return running;
        });
        if (openNow) {
            open(channelName, endpoint);
        }
    }

    private void disconnectEndpoint(String channelName, String endpoint) {
        inStateLane(() -> {
            Set<String> endpoints = desired.get(channelName);
            if (endpoints != null) {
                endpoints.remove(endpoint);
                if (endpoints.isEmpty()) desired.remove(channelName);
            }
            return null;
        });
        remove(connectionId(channelName, endpoint));
    }

    private void open(String channelName, String endpoint) {
        String id = connectionId(channelName, endpoint);
        boolean opening = inStateLane(() -> {
            if (!running || connections.containsKey(id)) return false;
            return true;
        });
        if (!opening) return;
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
            Connection candidate = connection;
            boolean accepted = inStateLane(() -> {
                Set<String> endpoints = desired.get(channelName);
                boolean current = running
                    && endpoints != null
                    && endpoints.contains(endpoint)
                    && !connections.containsKey(id);
                if (current) {
                    connections.put(id, candidate);
                    candidate.liveness.connect(
                        candidate.publisherId, candidate.connectionId,
                        System.nanoTime());
                }
                return current;
            });
            if (!accepted) {
                closeConnection(connection);
                return;
            }
            if (monitor != null) {
                monitor.onEvent(event -> onMonitorEvent(
                    candidate, event.event()));
            }
            // remove/closeConnection observes the lane claim before it
            // closes the socket, so callbacks never retain an unregistered
            // connection.
            subscriber.connect(endpoint);
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
        try {
            connection.subscriber.disconnect(connection.endpoint);
        } catch (RuntimeException ignored) {
        }
        if (connection.monitor != null) connection.monitor.close();
        connection.subscriber.close();
    }

    private void onMonitorEvent(Connection connection, String event) {
        if (!isCurrent(connection)) return;
        if (isTerminatedEvent(event)) {
            replace(connection);
        }
    }

    private void signalTick(long epoch) {
        boolean admitted = inStateLane(() -> {
            if (!running || lifecycleEpoch != epoch || tickAdmitted) {
                return false;
            }
            tickAdmitted = true;
            return true;
        });
        if (!admitted) return;
        try {
            infrastructureExecutor.execute(() -> runAdmittedTick(epoch));
        } catch (RejectedExecutionException closing) {
            inStateLane(() -> { tickAdmitted = false; return null; });
        }
    }

    private void runAdmittedTick(long epoch) {
        try {
            if (!inStateLane(() -> running && lifecycleEpoch == epoch)) {
                return;
            }
            reconcileDesired();
            receiveAvailable();
            expireConnections();
        } catch (RuntimeException failure) {
            LOGGER.log(Level.WARNING,
                "manual fanout tick failed; the next bounded tick retries", failure);
        } finally {
            inStateLane(() -> { tickAdmitted = false; return null; });
        }
    }

    private void reconcileDesired() {
        List<String> endpoints = inStateLane(() -> {
            List<String> current = new ArrayList<>();
            desired.forEach((channel, values) ->
                values.forEach(endpoint -> current.add(
                    channel + "\u0000" + endpoint)));
            return current;
        });
        endpoints.forEach(value -> {
            int separator = value.indexOf('\u0000');
            open(value.substring(0, separator), value.substring(separator + 1));
        });
    }

    private void receiveAvailable() {
        List<Connection> ordered = inStateLane(
            () -> new ArrayList<>(connections.values()));
        if (ordered.isEmpty()) return;
        int cursor = inStateLane(() ->
            (int) Math.floorMod(receiveCursor, ordered.size()));
        int idle = 0;
        ZLinkReceiveBatchBudget batch = new ZLinkReceiveBatchBudget();
        while (batch.canReceiveNext() && idle < ordered.size()) {
            Connection connection = ordered.get(cursor);
            cursor = (cursor + 1) % ordered.size();
            int nextCursor = cursor;
            inStateLane(() -> { receiveCursor = nextCursor; return null; });
            ZLinkBackendTopicMessage received;
            if (!isCurrent(connection)
                || !connection.subscriber.waitForReadable(Duration.ZERO)) {
                received = null;
            } else {
                received = connection.subscriber.subscribe(
                    ZLinkBackendRecvMode.DONT_WAIT);
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
                ZLinkClassicFanoutLiveness.ReceiveKind kind = inStateLane(() -> {
                    if (connections.get(connection.connectionId) != connection) {
                        received.parts().forEach(Message::close);
                        return null;
                    }
                    ZLinkClassicFanoutLiveness.ReceiveKind receivedKind = connection.liveness.receive(
                        connection.publisherId, connection.connectionId,
                        frames, System.nanoTime());
                    connection.ready = connection.liveness.isReady(
                        connection.publisherId);
                    return receivedKind;
                });
                if (kind == null) continue;
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

    private boolean isCurrent(Connection connection) {
        return inStateLane(() ->
            connections.get(connection.connectionId) == connection);
    }

    private void expireConnections() {
        long now = System.nanoTime();
        List<Connection> expired = new ArrayList<>();
        List<Connection> current = inStateLane(
            () -> List.copyOf(connections.values()));
        for (Connection connection : current) {
            if (inStateLane(() ->
                    !connection.liveness.expire(now).isEmpty())) {
                expired.add(connection);
            }
        }
        expired.forEach(this::replace);
    }

    private void replace(Connection connection) {
        if (!isCurrent(connection)) return;
        remove(connection.connectionId, connection);
    }

    private void remove(String id) {
        Connection connection = inStateLane(() -> {
            Connection current = connections.remove(id);
            if (current != null) {
                current.liveness.disconnect(
                    current.publisherId, current.connectionId);
            }
            return current;
        });
        if (connection == null) return;
        closeConnection(connection);
    }

    private void remove(String id, Connection expected) {
        boolean removed = inStateLane(() -> {
            if (connections.get(id) != expected) {
                return false;
            }
            connections.remove(id);
            expected.liveness.disconnect(
                expected.publisherId, expected.connectionId);
            return true;
        });
        if (removed) {
            closeConnection(expected);
        }
    }

    List<PublisherSnapshot> publisherSnapshots(String channelName) {
        return inStateLane(() -> connections.values().stream()
            .filter(connection -> connection.channelName.equals(channelName))
            .map(connection -> new PublisherSnapshot(
                connection.publisherId, connection.ready))
            .toList());
    }

    @Override
    public void close() {
        CloseState close = inStateLane(() -> {
            ScheduledFuture<?> task = tickTask;
            tickTask = null;
            running = false;
            lifecycleEpoch = Math.addExact(lifecycleEpoch, 1);
            return new CloseState(task, List.copyOf(connections.keySet()));
        });
        if (close.task() != null) close.task().cancel(false);
        close.ids().forEach(this::remove);
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

    private record StartState(Map<String, List<String>> initial, long epoch) { }

    private record CloseState(ScheduledFuture<?> task, List<String> ids) { }

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
