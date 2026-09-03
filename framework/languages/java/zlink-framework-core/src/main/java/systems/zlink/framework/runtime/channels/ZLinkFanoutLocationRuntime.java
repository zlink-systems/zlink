package systems.zlink.framework.runtime.channels;
import java.util.Comparator;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ThreadLocalRandom;
import systems.zlink.framework.locations.ZLinkLocationRole;
import systems.zlink.framework.runtime.internal.locations.ZLinkAutoConnectType;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteStatus;

import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CompletionException;
import java.util.concurrent.Executor;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.function.BiConsumer;
import java.util.function.Supplier;
import java.util.logging.Level;
import java.util.logging.Logger;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkFanoutPublisherDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkFanoutPublisherDescriptorKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteIntent;
import systems.zlink.framework.locations.ZLinkPageRequest;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendContext;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendPublisherSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRecvMode;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitor;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSubscriberSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendTopicMessage;
import systems.zlink.framework.runtime.internal.backend.ZLinkChannelBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkMonitoringBackendAdapter;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkReceiveBatchBudget;
import systems.zlink.framework.runtime.internal.service.ZLinkClassicFanoutLiveness;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

/**
 * Dedicated classic fanout descriptor publication and subscriber discovery.
 */
final class ZLinkFanoutLocationRuntime implements AutoCloseable {
    private static final String SECURITY_IDENTITY = "default";
    private static final int MAX_DESCRIPTORS_PER_CHANNEL = 1024;
    private static final Logger LOGGER =
        Logger.getLogger(ZLinkFanoutLocationRuntime.class.getName());

    private final ZLinkLocationRepository store;
    private final Supplier<ZLinkLocationOwnerToken> owner;
    private final ZLinkChannelBackendAdapter backend;
    private final ZLinkMonitoringBackendAdapter monitoring;
    private final ZLinkBackendContext context;
    private final ZLinkChannelSocketRegistry sockets;
    private final Duration pollingInterval;
    private final int pageSize;
    private final BiConsumer<String, ZLinkBackendTopicMessage> dispatch;
    private final ZLinkStateLane stateLane = new ZLinkStateLane();
    private final Map<String, Published> published =
        new ConcurrentHashMap<>();
    // The state lane exclusively owns connection membership and phase.
    private final Map<String, Connection> connections =
        new LinkedHashMap<>();
    private final Set<String> automaticChannels =
        ConcurrentHashMap.newKeySet();
    private final ScheduledExecutorService scheduler;
    private final Executor infrastructureExecutor;
    private ScheduledFuture<?> tickTask;
    private volatile boolean running;
    private volatile long nextReconcileNanos;
    private long receiveCursor;
    private volatile long lifecycleEpoch;
    private CompletableFuture<Void> admittedTick;
    private CompletableFuture<Void> stopCompletion;

    ZLinkFanoutLocationRuntime(
        ZLinkLocationRepository store,
        Supplier<ZLinkLocationOwnerToken> owner,
        ZLinkChannelBackendAdapter backend,
        ZLinkMonitoringBackendAdapter monitoring,
        ZLinkBackendContext context,
        ZLinkChannelSocketRegistry sockets,
        ScheduledExecutorService scheduler,
        Executor infrastructureExecutor,
        Duration pollingInterval,
        int pageSize,
        BiConsumer<String, ZLinkBackendTopicMessage> dispatch) {
        this.store = Objects.requireNonNull(store, "store");
        this.owner = Objects.requireNonNull(owner, "owner");
        this.backend = Objects.requireNonNull(backend, "backend");
        this.monitoring = Objects.requireNonNull(monitoring, "monitoring");
        this.context = Objects.requireNonNull(context, "context");
        this.sockets = Objects.requireNonNull(sockets, "sockets");
        this.scheduler = Objects.requireNonNull(scheduler, "scheduler");
        this.infrastructureExecutor = Objects.requireNonNull(
            infrastructureExecutor, "infrastructureExecutor");
        this.pollingInterval = Objects.requireNonNull(
            pollingInterval, "pollingInterval");
        this.pageSize = Math.max(1, Math.min(pageSize, 1000));
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

    CompletionStage<Void> start(
        List<ZLinkChannelRuntime.AutoConnectSurface> surfaces) {
        StartState start = inStateLane(() -> {
            if (running) {
                return StartState.alreadyRunning();
            }
            if (stopCompletion != null && !stopCompletion.isDone()) {
                return StartState.stoppingState();
            }
            initialize(surfaces);
            lifecycleEpoch = Math.addExact(lifecycleEpoch, 1);
            running = true;
            stopCompletion = null;
            long now = System.nanoTime();
            nextReconcileNanos = now;
            long epoch = lifecycleEpoch;
            return StartState.started(epoch);
        });
        if (start.stopping()) {
            return CompletableFuture.failedFuture(new IllegalStateException(
                "fanout location runtime is stopping"));
        }
        if (!start.started()) {
            return CompletableFuture.completedFuture(null);
        }
        if (start.started()) {
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
        return publishAll(ZLinkFrameworkRuntimeState.SERVING);
    }

    CompletionStage<Void> markDraining() {
        return publishAll(ZLinkFrameworkRuntimeState.DRAINING);
    }

    CompletionStage<Void> stop() {
        StopState stop = inStateLane(() -> {
            if (!running) {
                return StopState.alreadyStopped(stopCompletion);
            }
            ScheduledFuture<?> task = tickTask;
            tickTask = null;
            running = false;
            lifecycleEpoch = Math.addExact(lifecycleEpoch, 1);
            List<Published> servers = List.copyOf(published.values());
            published.clear();
            CompletableFuture<Void> pendingTick = admittedTick;
            CompletableFuture<Void> completion = new CompletableFuture<>();
            stopCompletion = completion;
            return StopState.stopping(servers, pendingTick, completion, task);
        });
        if (!stop.stopping()) {
            return stop.completion() == null
                ? CompletableFuture.completedFuture(null)
                : stop.completion();
        }
        if (stop.task() != null) {
            stop.task().cancel(false);
        }
        List<Published> servers = stop.servers();
        CompletableFuture<Void> pendingTick = stop.pendingTick();
        CompletableFuture<Void> completion = stop.completion();
        CompletionStage<Void> settled = pendingTick == null
            ? CompletableFuture.completedFuture(null)
            : pendingTick.handle((ignored, failure) -> null);
        settled.thenCompose(ignored -> removePublished(servers))
            .handle((ignored, failure) -> completionCause(failure))
            .thenCompose(publishFailure -> closeConnections()
                .handle((ignored, closeFailure) -> {
                    Throwable closeCause = completionCause(closeFailure);
                    if (publishFailure != null) {
                        if (closeCause != null && closeCause != publishFailure) {
                            publishFailure.addSuppressed(closeCause);
                        }
                        throw new CompletionException(publishFailure);
                    }
                    if (closeCause != null) {
                        throw new CompletionException(closeCause);
                    }
                    return null;
                }))
            .whenComplete((ignored, failure) -> {
                if (failure == null) {
                    completion.complete(null);
                } else {
                    completion.completeExceptionally(completionCause(failure));
                }
            });
        return completion;
    }

    private static Throwable completionCause(Throwable failure) {
        Throwable current = failure;
        while (current instanceof CompletionException
            && current.getCause() != null) {
            current = current.getCause();
        }
        return current;
    }

    private CompletionStage<Void> removePublished(
        List<Published> servers) {
        if (servers.isEmpty()) {
            return CompletableFuture.completedFuture(null);
        }
        List<CompletionStage<?>> removals = new ArrayList<>();
        ZLinkLocationOwnerToken token = owner.get();
        for (Published value : servers) {
            removals.add(store.removeFanoutPublisher(
                new ZLinkFanoutPublisherDescriptorKey(
                    value.channelName,
                    value.publisherRid),
                token));
        }
        return CompletableFuture.allOf(removals.stream()
            .map(stage -> stage.toCompletableFuture())
            .toArray(CompletableFuture[]::new));
    }

    private void initialize(
        List<ZLinkChannelRuntime.AutoConnectSurface> surfaces) {
        for (ZLinkChannelRuntime.AutoConnectSurface surface : surfaces) {
            if (surface.type()
                != ZLinkAutoConnectType.FANOUT) {
                continue;
            }
            if (surface.role()
                == ZLinkLocationRole.PUB) {
                if (published.putIfAbsent(
                        surface.meshName(),
                        new Published(
                            surface.meshName(),
                            Objects.requireNonNull(
                                surface.nodeRid(),
                                "publisher RID"),
                            positiveNonce(),
                            1,
                            surface.endpoint())) != null) {
                    throw new systems.zlink.framework.errors
                        .ZLinkConfigurationException(
                            "automatic fanout publisher supports one "
                                + "listener endpoint per channel: "
                                + surface.meshName());
                }
            } else if (surface.role()
                == ZLinkLocationRole.SUB) {
                automaticChannels.add(surface.meshName());
            }
        }
    }

    private CompletionStage<Void> publishAll(
        ZLinkFrameworkRuntimeState state) {
        if (published.isEmpty()) {
            return CompletableFuture.completedFuture(null);
        }
        ZLinkLocationOwnerToken token = owner.get();
        List<CompletionStage<?>> writes = new ArrayList<>();
        for (Map.Entry<String, Published> entry
            : published.entrySet()) {
            Published current = entry.getValue();
            long revision = current.state == state
                ? current.revision
                : Math.addExact(current.revision, 1);
            Published next = new Published(
                current.channelName,
                current.publisherRid,
                current.lifecycleGeneration,
                revision,
                current.endpoint,
                state);
            published.put(entry.getKey(), next);
            writes.add(store.updateFanoutPublisher(
                descriptor(next, token),
                current.stored
                    ? ZLinkLocationWriteIntent.RENEW
                    : ZLinkLocationWriteIntent.NEW_CLAIM)
                .thenAccept(result -> {
                    if (result.status()
                        != ZLinkLocationWriteStatus.STORED) {
                        throw new IllegalStateException(
                            "fanout publisher descriptor write was fenced");
                    }
                    next.stored = true;
                }));
        }
        return CompletableFuture.allOf(writes.stream()
            .map(stage -> stage.toCompletableFuture())
            .toArray(CompletableFuture[]::new));
    }

    private void signalTick(long epoch) {
        CompletableFuture<Void> settlement = inStateLane(() -> {
            if (!running || lifecycleEpoch != epoch || admittedTick != null) {
                return null;
            }
            CompletableFuture<Void> admitted = new CompletableFuture<>();
            admittedTick = admitted;
            return admitted;
        });
        if (settlement == null) {
            return;
        }
        try {
            infrastructureExecutor.execute(
                () -> runAdmittedTick(epoch, settlement));
        } catch (RejectedExecutionException closing) {
            settleTick(settlement, closing);
        }
    }

    private void runAdmittedTick(
        long epoch,
        CompletableFuture<Void> settlement) {
        CompletionStage<Void> work;
        try {
            if (!running || lifecycleEpoch != epoch) {
                settleTick(settlement, null);
                return;
            }
            long now = System.nanoTime();
            receiveAvailable(epoch, settlement);
            expireConnections(now, epoch, settlement);
            if (now < nextReconcileNanos) {
                settleTick(settlement, null);
                return;
            }
            nextReconcileNanos = now + pollingInterval.toNanos();
            work = reconcile(epoch, settlement);
        } catch (Throwable failure) {
            settleTick(settlement, failure);
            return;
        }
        work.whenComplete((ignored, failure) ->
            settleTick(settlement, failure));
    }

    private void settleTick(
        CompletableFuture<Void> settlement,
        Throwable failure) {
        inStateLane(() -> {
            if (admittedTick == settlement) {
                admittedTick = null;
            }
            return null;
        });
        if (failure != null) {
            LOGGER.log(
                Level.WARNING,
                "fanout location tick failed; the next bounded tick retries",
                failure);
            settlement.completeAsync(() -> {
                throw new java.util.concurrent.CompletionException(failure);
            });
        } else {
            settlement.completeAsync(() -> null);
        }
    }

    private CompletionStage<Void> reconcile(
        long epoch,
        CompletableFuture<Void> settlement) {
        List<CompletionStage<?>> reads = new ArrayList<>();
        for (String channelName : automaticChannels) {
            reads.add(readChannel(channelName).thenAccept(
                rows -> reconcileChannel(
                    channelName, rows, epoch, settlement)));
        }
        return CompletableFuture.allOf(reads.stream()
            .map(stage -> stage.toCompletableFuture())
            .toArray(CompletableFuture[]::new));
    }

    private CompletionStage<List<ZLinkFanoutPublisherDescriptor>>
        readChannel(String channelName) {
        List<ZLinkFanoutPublisherDescriptor> rows = new ArrayList<>();
        return readPage(channelName, null, rows);
    }

    private CompletionStage<List<ZLinkFanoutPublisherDescriptor>>
        readPage(
            String channelName,
            String cursor,
            List<ZLinkFanoutPublisherDescriptor> rows) {
        return store.listFanoutPublishers(
                channelName,
                new ZLinkPageRequest(pageSize, cursor))
            .thenCompose(page -> {
                for (ZLinkFanoutPublisherDescriptor row : page.items()) {
                    if (rows.size() == MAX_DESCRIPTORS_PER_CHANNEL) {
                        break;
                    }
                    rows.add(row);
                }
                if (page.continuationToken() == null
                    || rows.size() == MAX_DESCRIPTORS_PER_CHANNEL) {
                    return CompletableFuture.completedFuture(
                        List.copyOf(rows));
                }
                return readPage(
                    channelName,
                    page.continuationToken(),
                    rows);
            });
    }

    private void reconcileChannel(
        String channelName,
        List<ZLinkFanoutPublisherDescriptor> rows,
        long epoch,
        CompletableFuture<Void> settlement) {
        boolean current = inStateLane(() -> running
            && epoch == lifecycleEpoch
            && admittedTick == settlement);
        if (!current) {
            return;
        }
        Set<String> desired = new HashSet<>();
        for (ZLinkFanoutPublisherDescriptor row : rows) {
            if (row.state() != ZLinkFrameworkRuntimeState.SERVING) {
                continue;
            }
            String id = connectionId(row);
            desired.add(id);
            boolean absent = inStateLane(() -> !connections.containsKey(id));
            if (absent) {
                open(row, id, epoch, settlement);
            }
        }
        List<Connection> removed = inStateLane(() -> connections.values().stream()
            .filter(connection -> connection.descriptor.channelName()
                .equals(channelName))
            .filter(connection -> !desired.contains(connection.connectionId))
            .toList());
        removed.forEach(connection -> remove(
            connection.connectionId, connection, settlement));
    }

    private void open(
        ZLinkFanoutPublisherDescriptor descriptor,
        String connectionId,
        long epoch,
        CompletableFuture<Void> settlement) {
        removeByPublisher(
            descriptor.channelName(),
            descriptor.publisherRid(),
            settlement);
        ZLinkBackendSubscriberSocket subscriber =
            backend.createSubscriberSocket(context);
        ZLinkBackendSocketMonitor monitor = null;
        Connection connection = null;
        try {
            subscriber.setChannelName(descriptor.channelName());
            subscriber.setSubscription("");
            monitor = monitoring.openSocketMonitor(subscriber);
            connection = new Connection(
                descriptor,
                connectionId,
                subscriber,
                monitor);
            Connection candidate = connection;
            ZLinkSocketMonitorDrainLoop.start(
                "zlink-fanout-location-monitor", monitor, event -> {
                if (isReadyEvent(event.event())) {
                    inStateLane(() -> {
                        if (connections.get(connectionId) == candidate
                            && candidate.phase != ConnectionPhase.CLOSING) {
                            candidate.nativeReady = true;
                        }
                        return null;
                    });
                } else if (isTerminatedEvent(event.event())) {
                    remove(connectionId, candidate, null);
                }
                });
            boolean accepted = inStateLane(() -> {
                boolean current = running
                    && epoch == lifecycleEpoch
                    && admittedTick == settlement
                    && !connections.containsKey(connectionId);
                if (current) {
                    candidate.liveness.connect(
                        descriptor.publisherRid(),
                        connectionId,
                        System.nanoTime());
                    connections.put(connectionId, candidate);
                }
                return current;
            });
            if (!accepted) {
                closeUnregistered(candidate);
                return;
            }
            subscriber.connect(descriptor.endpoint());
            boolean committed = inStateLane(() -> {
                if (connections.get(connectionId) != candidate
                    || candidate.phase != ConnectionPhase.OPENING
                    || !running
                    || lifecycleEpoch != epoch
                    || admittedTick != settlement) {
                    return false;
                }
                candidate.phase = ConnectionPhase.RECEIVABLE;
                return true;
            });
            if (!committed) {
                remove(connectionId, candidate, settlement);
            }
        } catch (RuntimeException failure) {
            LOGGER.log(
                Level.WARNING,
                "fanout subscriber connect failed for publisher "
                    + descriptor.publisherRid().toHex()
                    + " at " + descriptor.endpoint(),
                failure);
            if (connection != null) {
                remove(connectionId, connection, settlement);
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

    private static void closeUnregistered(Connection connection) {
        try {
            connection.monitor.close();
        } catch (RuntimeException ignored) {
        }
        try {
            connection.subscriber.close();
        } catch (RuntimeException ignored) {
        }
    }

    private void receiveAvailable(
        long epoch,
        CompletableFuture<Void> settlement) {
        ReceiveSnapshot snapshot = inStateLane(() -> {
            if (!running
                || lifecycleEpoch != epoch
                || admittedTick != settlement) {
                return ReceiveSnapshot.empty();
            }
            List<Connection> ordered = connections.values().stream()
                .filter(connection ->
                    connection.phase == ConnectionPhase.RECEIVABLE)
                .sorted(Comparator.comparing(
                    connection -> connection.connectionId))
                .toList();
            if (ordered.isEmpty()) {
                return ReceiveSnapshot.empty();
            }
            int cursor = (int) Math.floorMod(
                receiveCursor, ordered.size());
            return new ReceiveSnapshot(List.copyOf(ordered), cursor);
        });
        List<Connection> ordered = snapshot.connections();
        if (ordered.isEmpty()) {
            return;
        }
        ZLinkReceiveBatchBudget batch = new ZLinkReceiveBatchBudget();
        int cursor = snapshot.cursor();
        int idleConnections = 0;
        try {
            while (batch.canReceiveNext()
                && idleConnections < ordered.size()) {
                Connection connection = ordered.get(cursor);
                cursor = (cursor + 1) % ordered.size();
                ZLinkBackendTopicMessage received;
                if (!connection.subscriber.waitForReadable(Duration.ZERO)) {
                    received = null;
                } else {
                    received = connection.subscriber.subscribe(
                        ZLinkBackendRecvMode.DONT_WAIT);
                }
                if (received == null) {
                    idleConnections++;
                    continue;
                }
                idleConnections = 0;
                batch.record(ZLinkReceiveBatchBudget.bytesOf(
                    received.parts(),
                    received.applicationMetadataSize(),
                    received.topic().getBytes(StandardCharsets.UTF_8).length));
                List<byte[]> frames = new ArrayList<>();
                frames.add(received.topic().getBytes(StandardCharsets.UTF_8));
                frames.addAll(received.parts().stream()
                    .map(Message::toByteArray)
                    .toList());
                ZLinkClassicFanoutLiveness.ReceiveKind kind;
                try {
                    kind = inStateLane(() -> {
                        if (admittedTick != settlement
                            || connections.get(connection.connectionId) != connection
                            || connection.phase != ConnectionPhase.RECEIVABLE) {
                            return null;
                        }
                        ZLinkClassicFanoutLiveness.ReceiveKind accepted =
                            connection.liveness.receive(
                                connection.descriptor.publisherRid(),
                                connection.connectionId,
                                frames,
                                System.nanoTime());
                        connection.ready = connection.nativeReady
                            && connection.liveness.isReady(
                                connection.descriptor.publisherRid());
                        return accepted;
                    });
                    if (kind == null) {
                        received.parts().forEach(Message::close);
                        continue;
                    }
                } catch (IllegalArgumentException malformed) {
                    received.parts().forEach(Message::close);
                    remove(connection.connectionId, connection, settlement);
                    continue;
                }
                if (kind == ZLinkClassicFanoutLiveness.ReceiveKind.APPLICATION) {
                    // Dispatch owns the received message. Keep user-facing queue
                    // admission outside the connection registry monitor.
                    dispatch.accept(connection.descriptor.channelName(), received);
                } else {
                    received.parts().forEach(Message::close);
                }
            }
        } finally {
            int nextCursor = cursor;
            inStateLane(() -> {
                if (admittedTick == settlement) {
                    receiveCursor = nextCursor;
                }
                return null;
            });
        }
    }

    private void expireConnections(
        long nowNanos,
        long epoch,
        CompletableFuture<Void> settlement) {
        List<Connection> expired = inStateLane(() -> {
            if (!running
                || lifecycleEpoch != epoch
                || admittedTick != settlement) {
                return List.of();
            }
            return connections.values().stream()
                .filter(connection ->
                    connection.phase == ConnectionPhase.RECEIVABLE)
                .filter(connection ->
                    !connection.liveness.expire(nowNanos).isEmpty())
                .toList();
        });
        expired.forEach(connection -> remove(
            connection.connectionId, connection, settlement));
    }

    private void removeByPublisher(
        String channelName,
        RoutingId rid,
        CompletableFuture<Void> settlement) {
        List<Connection> matching = inStateLane(() ->
            connections.values().stream()
            .filter(connection ->
                connection.descriptor.channelName()
                    .equals(channelName)
                && connection.descriptor.publisherRid()
                    .equals(rid))
            .toList());
        matching.forEach(connection -> remove(
            connection.connectionId, connection, settlement));
    }

    private CompletionStage<Void> remove(
        String connectionId,
        Connection expected,
        CompletableFuture<Void> currentTick) {
        CloseReservation reservation = inStateLane(() -> {
            if (connections.get(connectionId) != expected) {
                return null;
            }
            if (expected.phase == ConnectionPhase.CLOSING) {
                return CloseReservation.joining(expected);
            }
            expected.phase = ConnectionPhase.CLOSING;
            expected.liveness.disconnect(
                expected.descriptor.publisherRid(),
                expected.connectionId);
            return CloseReservation.owned(expected, admittedTick);
        });
        if (reservation == null) {
            return CompletableFuture.completedFuture(null);
        }
        if (!reservation.owner()) {
            return reservation.connection().closeSettlement;
        }
        // A receive snapshot is admission for the whole bounded tick. Closing
        // prevents a later snapshot, then joins the already-admitted tick.
        CompletionStage<Void> gate = reservation.pendingTick() == null
            || reservation.pendingTick() == currentTick
            ? CompletableFuture.completedFuture(null)
            : reservation.pendingTick().handle((ignored, failure) -> null);
        gate.thenRun(() -> closeNative(reservation.connection()))
            .whenComplete((ignored, failure) -> finishClose(
                reservation.connection(), failure));
        return reservation.connection().closeSettlement;
    }

    private void finishClose(
        Connection connection,
        Throwable failure) {
        Throwable completionFailure = failure;
        try {
            inStateLane(() -> {
                if (connections.get(connection.connectionId) == connection) {
                    connections.remove(connection.connectionId);
                }
                return null;
            });
        } catch (RuntimeException stateFailure) {
            if (completionFailure == null) {
                completionFailure = stateFailure;
            } else {
                completionFailure.addSuppressed(stateFailure);
            }
        }
        if (completionFailure == null) {
            connection.closeSettlement.complete(null);
        } else {
            connection.closeSettlement.completeExceptionally(
                completionFailure);
        }
    }

    private static void closeNative(Connection connection) {
        try {
            connection.subscriber.disconnect(connection.descriptor.endpoint());
        } catch (RuntimeException ignored) {
        }
        RuntimeException failure = null;
        try {
            connection.monitor.close();
        } catch (RuntimeException monitorFailure) {
            failure = monitorFailure;
        }
        try {
            connection.subscriber.close();
        } catch (RuntimeException subscriberFailure) {
            if (failure == null) {
                failure = subscriberFailure;
            } else {
                failure.addSuppressed(subscriberFailure);
            }
        }
        if (failure != null) {
            throw failure;
        }
    }

    private CompletionStage<Void> closeConnections() {
        List<Connection> snapshot = inStateLane(() ->
            List.copyOf(connections.values()));
        List<CompletionStage<Void>> closes = snapshot.stream()
            .map(connection -> remove(
                connection.connectionId, connection, null))
            .toList();
        return CompletableFuture.allOf(closes.stream()
            .map(CompletionStage::toCompletableFuture)
            .toArray(CompletableFuture[]::new));
    }

    private static ZLinkFanoutPublisherDescriptor descriptor(
        Published value,
        ZLinkLocationOwnerToken owner) {
        return new ZLinkFanoutPublisherDescriptor(
            value.channelName,
            value.publisherRid,
            value.lifecycleGeneration,
            value.revision,
            value.endpoint,
            value.state,
            SECURITY_IDENTITY,
            owner.ownerId(),
            owner.leaseGeneration(),
            Instant.now());
    }

    private static String connectionId(
        ZLinkFanoutPublisherDescriptor descriptor) {
        return descriptor.channelName()
            + "/"
            + descriptor.publisherRid().toHex()
            + "/"
            + descriptor.lifecycleGeneration();
    }

    private static long positiveNonce() {
        long value = ThreadLocalRandom.current()
            .nextLong(1, Long.MAX_VALUE);
        return value == 0 ? 1 : value;
    }

    private static boolean isReadyEvent(String event) {
        return "CONNECTION_READY".equals(event)
            || "ConnectionReady".equals(event);
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

    @Override
    public void close() {
        stop().toCompletableFuture().join();
    }

    private void cancelTickTask() {
        if (tickTask != null) {
            tickTask.cancel(false);
            tickTask = null;
        }
    }

    List<FanoutPublisherSnapshot> publisherSnapshots(String channelName) {
        return inStateLane(() -> connections.values().stream()
            .filter(connection ->
                connection.phase == ConnectionPhase.RECEIVABLE)
            .filter(connection ->
                connection.descriptor.channelName().equals(channelName))
            .map(connection -> new FanoutPublisherSnapshot(
                connection.descriptor.publisherRid(),
                connection.ready))
            .sorted(Comparator.comparing(
                snapshot -> snapshot.nodeRid().toHex()))
            .toList());
    }

    record FanoutPublisherSnapshot(
        RoutingId nodeRid,
        boolean ready) {
    }

    private record ReceiveSnapshot(
        List<Connection> connections,
        int cursor) {
        private static ReceiveSnapshot empty() {
            return new ReceiveSnapshot(List.of(), 0);
        }
    }

    private record CloseReservation(
        Connection connection,
        CompletableFuture<Void> pendingTick,
        boolean owner) {
        private static CloseReservation owned(
            Connection connection,
            CompletableFuture<Void> pendingTick) {
            return new CloseReservation(connection, pendingTick, true);
        }

        private static CloseReservation joining(Connection connection) {
            return new CloseReservation(connection, null, false);
        }
    }

    private enum ConnectionPhase {
        OPENING,
        RECEIVABLE,
        CLOSING
    }

    private record StartState(
        boolean started,
        boolean stopping,
        long epoch) {
        private static StartState alreadyRunning() {
            return new StartState(false, false, 0);
        }

        private static StartState stoppingState() {
            return new StartState(false, true, 0);
        }

        private static StartState started(long epoch) {
            return new StartState(true, false, epoch);
        }
    }

    private record StopState(
        boolean stopping,
        List<Published> servers,
        CompletableFuture<Void> pendingTick,
        CompletableFuture<Void> completion,
        ScheduledFuture<?> task) {
        private static StopState alreadyStopped(
            CompletableFuture<Void> completion) {
            return new StopState(false, List.of(), null, completion, null);
        }

        private static StopState stopping(
            List<Published> servers,
            CompletableFuture<Void> pendingTick,
            CompletableFuture<Void> completion,
            ScheduledFuture<?> task) {
            return new StopState(true, servers, pendingTick, completion, task);
        }
    }

    private static final class Published {
        private final String channelName;
        private final RoutingId publisherRid;
        private final long lifecycleGeneration;
        private final long revision;
        private final String endpoint;
        private final ZLinkFrameworkRuntimeState state;
        private volatile boolean stored;

        private Published(
            String channelName,
            RoutingId publisherRid,
            long lifecycleGeneration,
            long revision,
            String endpoint) {
            this(
                channelName,
                publisherRid,
                lifecycleGeneration,
                revision,
                endpoint,
                ZLinkFrameworkRuntimeState.SERVING);
        }

        private Published(
            String channelName,
            RoutingId publisherRid,
            long lifecycleGeneration,
            long revision,
            String endpoint,
            ZLinkFrameworkRuntimeState state) {
            this.channelName = channelName;
            this.publisherRid = publisherRid;
            this.lifecycleGeneration = lifecycleGeneration;
            this.revision = revision;
            this.endpoint = endpoint;
            this.state = state;
        }
    }

    private static final class Connection {
        private final ZLinkFanoutPublisherDescriptor descriptor;
        private final String connectionId;
        private final ZLinkBackendSubscriberSocket subscriber;
        private final ZLinkBackendSocketMonitor monitor;
        private final ZLinkClassicFanoutLiveness liveness =
            new ZLinkClassicFanoutLiveness();
        private final CompletableFuture<Void> closeSettlement =
            new CompletableFuture<>();
        private ConnectionPhase phase = ConnectionPhase.OPENING;
        private boolean nativeReady;
        private boolean ready;

        private Connection(
            ZLinkFanoutPublisherDescriptor descriptor,
            String connectionId,
            ZLinkBackendSubscriberSocket subscriber,
            ZLinkBackendSocketMonitor monitor) {
            this.descriptor = descriptor;
            this.connectionId = connectionId;
            this.subscriber = subscriber;
            this.monitor = monitor;
        }
    }
}
