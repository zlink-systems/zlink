package systems.zlink.framework.runtime.channels;

import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executor;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.function.BiConsumer;
import java.util.function.Supplier;
import java.util.logging.Level;
import java.util.logging.Logger;
import java.util.stream.Collectors;
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
    private final Map<String, CompletableFuture<Void>> openingOperations =
        new LinkedHashMap<>();
    private final ScheduledExecutorService scheduler;
    private final Executor infrastructureExecutor;
    private ScheduledFuture<?> tickTask;
    private volatile boolean running;
    private long lifecycleEpoch;
    private CompletableFuture<Void> admittedTick;
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
            endpoints.forEach(endpoint -> open(channel, endpoint, null)));
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
        String id = connectionId(channelName, endpoint);
        // Registering the endpoint and claiming its first open happen in one
        // lane turn, so the reconcile tick cannot take the open away from the
        // caller between the two.
        CompletableFuture<Void> opening = inStateLane(() -> {
            desired.computeIfAbsent(
                channelName, ignored -> new LinkedHashSet<>())
                .add(endpoint);
            return claimOpeningInLane(id);
        });
        openClaimed(channelName, endpoint, id, opening, null);
    }

    private void disconnectEndpoint(String channelName, String endpoint) {
        String id = connectionId(channelName, endpoint);
        CompletableFuture<Void> opening = inStateLane(() -> {
            Set<String> endpoints = desired.get(channelName);
            if (endpoints != null) {
                endpoints.remove(endpoint);
                if (endpoints.isEmpty()) desired.remove(channelName);
            }
            return openingOperations.get(id);
        });
        CompletionStage<Void> connectionClose = remove(id, null, null);
        awaitClose(CompletableFuture.allOf(
            opening == null
                ? CompletableFuture.completedFuture(null)
                : opening,
            connectionClose.toCompletableFuture()));
    }

    private void open(
        String channelName,
        String endpoint,
        CompletableFuture<Void> activeTick) {
        String id = connectionId(channelName, endpoint);
        openClaimed(
            channelName, endpoint, id,
            inStateLane(() -> claimOpeningInLane(id)),
            activeTick);
    }

    private CompletableFuture<Void> claimOpeningInLane(String id) {
        if (!running
            || connections.containsKey(id)
            || openingOperations.containsKey(id)) {
            return null;
        }
        CompletableFuture<Void> admitted = new CompletableFuture<>();
        openingOperations.put(id, admitted);
        return admitted;
    }

    private void openClaimed(
        String channelName,
        String endpoint,
        String id,
        CompletableFuture<Void> opening,
        CompletableFuture<Void> activeTick) {
        if (opening == null) return;
        try {
            openAdmitted(channelName, endpoint, id, activeTick);
        } finally {
            inStateLane(() -> {
                openingOperations.remove(id, opening);
                return null;
            });
            opening.complete(null);
        }
    }

    private void openAdmitted(
        String channelName,
        String endpoint,
        String id,
        CompletableFuture<Void> activeTick) {
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
                }
                return current;
            });
            if (!accepted) {
                closeUnregistered(subscriber, monitor);
                return;
            }
            if (monitor != null) {
                ZLinkSocketMonitorDrainLoop.start(
                    "zlink-manual-fanout-monitor",
                    monitor,
                    event -> onMonitorEvent(candidate, event.event()));
            }
            // A close reservation observes the lane claim before it closes
            // the socket, so callbacks never retain an unregistered
            // connection.
            boolean connectAdmitted = inStateLane(() ->
                connections.get(id) == candidate
                    && candidate.phase == ConnectionPhase.OPENING);
            if (!connectAdmitted) {
                candidate.openingSettlement.complete(null);
                return;
            }
            try {
                subscriber.connect(endpoint);
            } finally {
                candidate.openingSettlement.complete(null);
            }
            boolean committed = inStateLane(() -> {
                Set<String> endpoints = desired.get(channelName);
                boolean current = running
                    && endpoints != null
                    && endpoints.contains(endpoint)
                    && connections.get(id) == candidate
                    && candidate.phase == ConnectionPhase.OPENING;
                if (current) {
                    candidate.liveness.connect(
                        candidate.publisherId, candidate.connectionId,
                        System.nanoTime());
                    candidate.phase = ConnectionPhase.RECEIVABLE;
                }
                return current;
            });
            if (!committed) {
                CompletionStage<Void> close = remove(id, candidate, activeTick);
                if (activeTick == null) {
                    awaitClose(close);
                }
            }
        } catch (RuntimeException failure) {
            LOGGER.log(Level.WARNING,
                "manual fanout subscriber connect failed for " + endpoint, failure);
            if (connection != null) {
                connection.openingSettlement.complete(null);
                CompletionStage<Void> close = remove(
                    connection.connectionId, connection, activeTick);
                if (activeTick == null) {
                    awaitClose(close);
                }
            } else {
                closeUnregistered(subscriber, monitor);
            }
        }
    }

    private static void closeUnregistered(
        ZLinkBackendSubscriberSocket subscriber,
        ZLinkBackendSocketMonitor monitor) {
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

    private void onMonitorEvent(Connection connection, String event) {
        if (!isCurrent(connection)) return;
        if (isTerminatedEvent(event)) {
            replace(connection, null);
        }
    }

    private void signalTick(long epoch) {
        CompletableFuture<Void> token = inStateLane(() -> {
            if (!running || lifecycleEpoch != epoch || admittedTick != null) {
                return null;
            }
            CompletableFuture<Void> admitted = new CompletableFuture<>();
            admittedTick = admitted;
            return admitted;
        });
        if (token == null) return;
        try {
            infrastructureExecutor.execute(() -> runAdmittedTick(epoch, token));
        } catch (RejectedExecutionException closing) {
            clearAdmittedTick(token);
        }
    }

    private void runAdmittedTick(long epoch, CompletableFuture<Void> token) {
        try {
            if (!inStateLane(() -> running
                    && lifecycleEpoch == epoch
                    && admittedTick == token)) {
                return;
            }
            reconcileDesired(token);
            receiveAvailable(token);
            expireConnections(token);
        } catch (RuntimeException failure) {
            LOGGER.log(Level.WARNING,
                "manual fanout tick failed; the next bounded tick retries", failure);
        } finally {
            clearAdmittedTick(token);
        }
    }

    private void clearAdmittedTick(CompletableFuture<Void> token) {
        inStateLane(() -> {
            if (admittedTick == token) {
                admittedTick = null;
            }
            return null;
        });
        token.complete(null);
    }

    private void reconcileDesired(CompletableFuture<Void> token) {
        List<String> endpoints = inStateLane(() -> {
            assertAdmittedTick(token);
            List<String> current = new ArrayList<>();
            desired.forEach((channel, values) ->
                values.forEach(endpoint -> current.add(
                    channel + "\u0000" + endpoint)));
            return current;
        });
        endpoints.forEach(value -> {
            int separator = value.indexOf('\u0000');
            open(
                value.substring(0, separator),
                value.substring(separator + 1),
                token);
        });
    }

    private void receiveAvailable(CompletableFuture<Void> token) {
        List<Connection> ordered = inStateLane(() -> {
            assertAdmittedTick(token);
            return connections.values().stream()
                .filter(connection ->
                    connection.phase == ConnectionPhase.RECEIVABLE)
                .toList();
        });
        if (ordered.isEmpty()) return;
        int cursor = inStateLane(() ->
            (int) Math.floorMod(receiveCursor, ordered.size()));
        int idle = 0;
        ZLinkReceiveBatchBudget batch = new ZLinkReceiveBatchBudget();
        while (batch.canReceiveNext() && idle < ordered.size()) {
            Connection connection = ordered.get(cursor);
            cursor = (cursor + 1) % ordered.size();
            int nextCursor = cursor;
            inStateLane(() -> {
                assertAdmittedTick(token);
                receiveCursor = nextCursor;
                return null;
            });
            ZLinkBackendTopicMessage received;
            if (!isReceivable(connection, token)
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
            frames.addAll(received.parts().stream()
                .map(Message::toByteArray)
                .toList());
            try {
                ZLinkClassicFanoutLiveness.ReceiveKind kind = inStateLane(() -> {
                    assertAdmittedTick(token);
                    if (connections.get(connection.connectionId) != connection
                        || connection.phase != ConnectionPhase.RECEIVABLE) {
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
                replace(connection, token);
            }
        }
    }

    private boolean isReceivable(
        Connection connection,
        CompletableFuture<Void> token) {
        return inStateLane(() -> {
            assertAdmittedTick(token);
            return connections.get(connection.connectionId) == connection
                && connection.phase == ConnectionPhase.RECEIVABLE;
        });
    }

    private void assertAdmittedTick(CompletableFuture<Void> token) {
        if (admittedTick != token) {
            throw new IllegalStateException(
                "manual fanout receive must run in its admitted tick");
        }
    }

    private boolean isCurrent(Connection connection) {
        return inStateLane(() ->
            connections.get(connection.connectionId) == connection);
    }

    private void expireConnections(CompletableFuture<Void> token) {
        long now = System.nanoTime();
        List<Connection> expired = new ArrayList<>();
        List<Connection> current = inStateLane(() -> {
            assertAdmittedTick(token);
            return connections.values().stream()
                .filter(connection ->
                    connection.phase == ConnectionPhase.RECEIVABLE)
                .toList();
        });
        for (Connection connection : current) {
            if (inStateLane(() -> {
                assertAdmittedTick(token);
                return connections.get(connection.connectionId) == connection
                    && connection.phase == ConnectionPhase.RECEIVABLE
                    && !connection.liveness.expire(now).isEmpty();
            })) {
                expired.add(connection);
            }
        }
        expired.forEach(connection -> replace(connection, token));
    }

    private void replace(
        Connection connection,
        CompletableFuture<Void> activeTick) {
        remove(connection.connectionId, connection, activeTick);
    }

    private CompletionStage<Void> remove(
        String id,
        Connection expected,
        CompletableFuture<Void> activeTick) {
        CloseReservation reservation = inStateLane(() ->
            reserveCloseInLane(id, expected, activeTick));
        return startClose(reservation);
    }

    private CloseReservation reserveCloseInLane(
        String id,
        Connection expected,
        CompletableFuture<Void> activeTick) {
        Connection current = connections.get(id);
        if (current == null || (expected != null && current != expected)) {
            return null;
        }
        if (current.phase == ConnectionPhase.CLOSING) {
            return new CloseReservation(
                current, null, current.closeSettlement, false);
        }
        current.phase = ConnectionPhase.CLOSING;
        List<CompletableFuture<Void>> dependencies = new ArrayList<>(2);
        if (!current.openingSettlement.isDone()) {
            dependencies.add(current.openingSettlement);
        }
        if (admittedTick != null && admittedTick != activeTick) {
            dependencies.add(admittedTick);
        }
        return new CloseReservation(
            current, List.copyOf(dependencies), current.closeSettlement, true);
    }

    private CompletionStage<Void> startClose(CloseReservation reservation) {
        if (reservation == null) {
            return CompletableFuture.completedFuture(null);
        }
        if (reservation.owner()) {
            CompletionStage<Void> dependency = CompletableFuture.allOf(
                reservation.dependencies().toArray(CompletableFuture[]::new));
            dependency.whenComplete((ignored, failure) ->
                closeReserved(reservation.connection()));
        }
        return reservation.settlement();
    }

    private void closeReserved(Connection connection) {
        RuntimeException failure = null;
        try {
            connection.subscriber.disconnect(connection.endpoint);
        } catch (RuntimeException ignored) {
        }
        if (connection.monitor != null) {
            try {
                connection.monitor.close();
            } catch (RuntimeException closeFailure) {
                failure = closeFailure;
            }
        }
        try {
            connection.subscriber.close();
        } catch (RuntimeException closeFailure) {
            if (failure == null) {
                failure = closeFailure;
            } else {
                failure.addSuppressed(closeFailure);
            }
        }
        RuntimeException closeFailure = failure;
        try {
            inStateLane(() -> {
                if (connections.get(connection.connectionId) == connection) {
                    connections.remove(connection.connectionId);
                    connection.liveness.disconnect(
                        connection.publisherId, connection.connectionId);
                }
                return null;
            });
        } catch (RuntimeException stateFailure) {
            if (closeFailure == null) {
                closeFailure = stateFailure;
            } else {
                closeFailure.addSuppressed(stateFailure);
            }
        }
        if (closeFailure == null) {
            connection.closeSettlement.complete(null);
        } else {
            connection.closeSettlement.completeExceptionally(closeFailure);
        }
    }

    private static void awaitClose(CompletionStage<Void> close) {
        try {
            close.toCompletableFuture().join();
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

    List<PublisherSnapshot> publisherSnapshots(String channelName) {
        return inStateLane(() -> connections.values().stream()
            .filter(connection -> connection.channelName.equals(channelName))
            .filter(connection ->
                connection.phase == ConnectionPhase.RECEIVABLE)
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
            CompletableFuture<Void> tick = admittedTick;
            List<CompletableFuture<Void>> openings =
                List.copyOf(openingOperations.values());
            List<CloseReservation> reservations = connections.values().stream()
                .map(connection -> reserveCloseInLane(
                    connection.connectionId, connection, null))
                .toList();
            return new CloseState(task, tick, openings, reservations);
        });
        if (close.task() != null) close.task().cancel(false);
        List<CompletableFuture<Void>> settlements = new ArrayList<>();
        if (close.admittedTick() != null) {
            settlements.add(close.admittedTick());
        }
        settlements.addAll(close.openingOperations());
        close.reservations().stream()
            .map(this::startClose)
            .map(CompletionStage::toCompletableFuture)
            .forEach(settlements::add);
        awaitClose(CompletableFuture.allOf(
            settlements.toArray(CompletableFuture[]::new)));
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

    private record CloseState(
        ScheduledFuture<?> task,
        CompletableFuture<Void> admittedTick,
        List<CompletableFuture<Void>> openingOperations,
        List<CloseReservation> reservations) { }

    private record CloseReservation(
        Connection connection,
        List<CompletableFuture<Void>> dependencies,
        CompletableFuture<Void> settlement,
        boolean owner) { }

    private enum ConnectionPhase {
        OPENING,
        RECEIVABLE,
        CLOSING
    }

    private static final class Connection {
        private final String channelName;
        private final String endpoint;
        private final RoutingId publisherId;
        private final String connectionId;
        private final ZLinkBackendSubscriberSocket subscriber;
        private final ZLinkBackendSocketMonitor monitor;
        private final ZLinkClassicFanoutLiveness liveness =
            new ZLinkClassicFanoutLiveness();
        private final CompletableFuture<Void> openingSettlement =
            new CompletableFuture<>();
        private final CompletableFuture<Void> closeSettlement =
            new CompletableFuture<>();
        private ConnectionPhase phase = ConnectionPhase.OPENING;
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
