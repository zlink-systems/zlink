package systems.zlink.framework.runtime.channels;

import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
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
    private final Map<String, Published> published = new LinkedHashMap<>();
    private final Map<String, Connection> connections =
        new java.util.concurrent.ConcurrentHashMap<>();
    private final Set<String> automaticChannels = new HashSet<>();
    private final ScheduledExecutorService executor =
        Executors.newSingleThreadScheduledExecutor(task -> {
            Thread thread = new Thread(
                task, "zlink-java-fanout-location");
            thread.setDaemon(true);
            return thread;
        });
    private volatile boolean running;
    private volatile long nextReconcileNanos;
    private volatile long nextBeaconNanos;
    private long receiveCursor;
    private volatile long lifecycleEpoch;
    private volatile boolean reconciling;

    ZLinkFanoutLocationRuntime(
        ZLinkLocationRepository store,
        Supplier<ZLinkLocationOwnerToken> owner,
        ZLinkChannelBackendAdapter backend,
        ZLinkMonitoringBackendAdapter monitoring,
        ZLinkBackendContext context,
        ZLinkChannelSocketRegistry sockets,
        Duration pollingInterval,
        int pageSize,
        BiConsumer<String, ZLinkBackendTopicMessage> dispatch) {
        this.store = Objects.requireNonNull(store, "store");
        this.owner = Objects.requireNonNull(owner, "owner");
        this.backend = Objects.requireNonNull(backend, "backend");
        this.monitoring = Objects.requireNonNull(monitoring, "monitoring");
        this.context = Objects.requireNonNull(context, "context");
        this.sockets = Objects.requireNonNull(sockets, "sockets");
        this.pollingInterval = Objects.requireNonNull(
            pollingInterval, "pollingInterval");
        this.pageSize = Math.max(1, Math.min(pageSize, 1000));
        this.dispatch = Objects.requireNonNull(dispatch, "dispatch");
    }

    CompletionStage<Void> start(
        List<ZLinkChannelRuntime.AutoConnectSurface> surfaces) {
        if (running) {
            return CompletableFuture.completedFuture(null);
        }
        initialize(surfaces);
        lifecycleEpoch = Math.addExact(lifecycleEpoch, 1);
        running = true;
        long now = System.nanoTime();
        nextBeaconNanos = now;
        nextReconcileNanos = now;
        executor.scheduleAtFixedRate(
            this::tickSafely,
            0,
            10,
            TimeUnit.MILLISECONDS);
        return publishAll(ZLinkFrameworkRuntimeState.SERVING);
    }

    CompletionStage<Void> markDraining() {
        return publishAll(ZLinkFrameworkRuntimeState.DRAINING);
    }

    CompletionStage<Void> stop() {
        running = false;
        lifecycleEpoch = Math.addExact(lifecycleEpoch, 1);
        if (published.isEmpty()) {
            closeConnections();
            return CompletableFuture.completedFuture(null);
        }
        List<CompletionStage<?>> removals = new ArrayList<>();
        ZLinkLocationOwnerToken token = owner.get();
        for (Published value : published.values()) {
            removals.add(store.removeFanoutPublisher(
                new ZLinkFanoutPublisherDescriptorKey(
                    value.channelName,
                    value.publisherRid),
                token));
        }
        closeConnections();
        return CompletableFuture.allOf(removals.stream()
            .map(stage -> stage.toCompletableFuture())
            .toArray(CompletableFuture[]::new));
    }

    private void initialize(
        List<ZLinkChannelRuntime.AutoConnectSurface> surfaces) {
        for (ZLinkChannelRuntime.AutoConnectSurface surface : surfaces) {
            if (surface.type()
                != systems.zlink.framework.runtime.internal.locations.ZLinkAutoConnectType.FANOUT) {
                continue;
            }
            if (surface.role()
                == systems.zlink.framework.locations.ZLinkLocationRole.PUB) {
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
                == systems.zlink.framework.locations.ZLinkLocationRole.SUB) {
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
                        != systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteStatus.STORED) {
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

    private void tickSafely() {
        if (!running) {
            return;
        }
        try {
            long now = System.nanoTime();
            if (now >= nextBeaconNanos) {
                publishBeacons();
                nextBeaconNanos = now
                    + ZLinkClassicFanoutLiveness.DEFAULT_BEACON_INTERVAL
                        .toNanos();
            }
            receiveAvailable(now);
            expireConnections(now);
            if (now >= nextReconcileNanos && !reconciling) {
                reconciling = true;
                nextReconcileNanos = now + pollingInterval.toNanos();
                long epoch = lifecycleEpoch;
                reconcile(epoch).whenComplete((ignored, failure) -> {
                    reconciling = false;
                    if (failure != null) {
                        LOGGER.log(
                            Level.WARNING,
                            "fanout location reconcile failed",
                            failure);
                    }
                });
            }
        } catch (RuntimeException failure) {
            LOGGER.log(
                Level.WARNING,
                "fanout location tick failed; the next bounded tick retries",
                failure);
        }
    }

    private void publishBeacons() {
        List<byte[]> record = ZLinkClassicFanoutLiveness.beaconRecord();
        String topic = new String(
            record.getFirst(),
            StandardCharsets.UTF_8);
        for (Published value : published.values()) {
            ZLinkBackendPublisherSocket publisher =
                sockets.publisher(value.channelName);
            if (publisher == null) {
                continue;
            }
            try (Message payload = Message.from(record.get(1))) {
                publisher.publish(
                    topic,
                    List.of(payload),
                    SendFlags.DONT_WAIT);
            }
        }
    }

    private CompletionStage<Void> reconcile(long epoch) {
        List<CompletionStage<?>> reads = new ArrayList<>();
        for (String channelName : automaticChannels) {
            reads.add(readChannel(channelName).thenAccept(
                rows -> reconcileChannel(channelName, rows, epoch)));
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

    private synchronized void reconcileChannel(
        String channelName,
        List<ZLinkFanoutPublisherDescriptor> rows) {
        reconcileChannel(channelName, rows, lifecycleEpoch);
    }

    private synchronized void reconcileChannel(
        String channelName,
        List<ZLinkFanoutPublisherDescriptor> rows,
        long epoch) {
        if (!running || epoch != lifecycleEpoch) {
            return;
        }
        Set<String> desired = new HashSet<>();
        for (ZLinkFanoutPublisherDescriptor row : rows) {
            if (row.state() != ZLinkFrameworkRuntimeState.SERVING) {
                continue;
            }
            String id = connectionId(row);
            desired.add(id);
            if (!connections.containsKey(id)) {
                open(row, id);
            }
        }
        List<String> removed = connections.entrySet().stream()
            .filter(entry -> entry.getValue().descriptor.channelName()
                .equals(channelName))
            .map(Map.Entry::getKey)
            .filter(id -> !desired.contains(id))
            .toList();
        removed.forEach(this::remove);
    }

    private void open(
        ZLinkFanoutPublisherDescriptor descriptor,
        String connectionId) {
        removeByPublisher(
            descriptor.channelName(),
            descriptor.publisherRid());
        ZLinkBackendSubscriberSocket subscriber =
            backend.createSubscriberSocket(context);
        subscriber.setChannelName(descriptor.channelName());
        subscriber.setSubscription("");
        ZLinkBackendSocketMonitor monitor =
            monitoring.openSocketMonitor(subscriber);
        Connection connection = new Connection(
            descriptor,
            connectionId,
            subscriber,
            monitor);
        connections.put(connectionId, connection);
        connection.liveness.connect(
            descriptor.publisherRid(),
            connectionId,
            System.nanoTime());
        monitor.onEvent(event -> {
            if (isReadyEvent(event.event())) {
                connection.nativeReady = true;
            } else if (isTerminatedEvent(event.event())) {
                remove(connectionId, connection);
            }
        });
        try {
            subscriber.connect(descriptor.endpoint());
        } catch (RuntimeException failure) {
            LOGGER.log(
                Level.WARNING,
                "fanout subscriber connect failed for publisher "
                    + descriptor.publisherRid().toHex()
                    + " at " + descriptor.endpoint(),
                failure);
            remove(connectionId, connection);
        }
    }

    private synchronized void receiveAvailable(long nowNanos) {
        List<Connection> ordered = new ArrayList<>(connections.values());
        ordered.sort((left, right) ->
            left.connectionId.compareTo(right.connectionId));
        if (ordered.isEmpty()) {
            return;
        }
        ZLinkReceiveBatchBudget batch = new ZLinkReceiveBatchBudget();
        int cursor = (int) Math.floorMod(receiveCursor, ordered.size());
        int idleConnections = 0;
        while (batch.canReceiveNext() && idleConnections < ordered.size()) {
            Connection connection = ordered.get(cursor);
            cursor = (cursor + 1) % ordered.size();
            receiveCursor = cursor;
            if (!connection.subscriber.waitForReadable(Duration.ZERO)) {
                idleConnections++;
                continue;
            }
            ZLinkBackendTopicMessage received = connection.subscriber.subscribe(
                ZLinkBackendRecvMode.DONT_WAIT);
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
                kind = connection.liveness.receive(
                        connection.descriptor.publisherRid(),
                        connection.connectionId,
                        frames,
                        System.nanoTime());
            } catch (IllegalArgumentException malformed) {
                received.parts().forEach(Message::close);
                remove(connection.connectionId, connection);
                continue;
            }
            if (kind == ZLinkClassicFanoutLiveness.ReceiveKind.APPLICATION) {
                dispatch.accept(connection.descriptor.channelName(), received);
            } else {
                received.parts().forEach(Message::close);
            }
            connection.ready = connection.nativeReady
                && connection.liveness.isReady(
                    connection.descriptor.publisherRid());
        }
    }

    private synchronized void expireConnections(long nowNanos) {
        connections.values().stream()
            .filter(connection ->
                !connection.liveness.expire(nowNanos).isEmpty())
            .map(connection -> connection.connectionId)
            .toList()
            .forEach(this::remove);
    }

    private synchronized void removeByPublisher(
        String channelName,
        RoutingId rid) {
        connections.entrySet().stream()
            .filter(entry ->
                entry.getValue().descriptor.channelName()
                    .equals(channelName)
                    && entry.getValue().descriptor.publisherRid()
                        .equals(rid))
            .map(Map.Entry::getKey)
            .toList()
            .forEach(this::remove);
    }

    private synchronized void remove(String connectionId) {
        Connection connection = connections.remove(connectionId);
        if (connection == null) {
            return;
        }
        closeConnection(connection);
    }

    private synchronized void remove(
        String connectionId,
        Connection expected) {
        if (connections.get(connectionId) != expected) {
            return;
        }
        connections.remove(connectionId);
        closeConnection(expected);
    }

    private static void closeConnection(Connection connection) {
        connection.liveness.disconnect(
            connection.descriptor.publisherRid(),
            connection.connectionId);
        try {
            connection.subscriber.disconnect(
                connection.descriptor.endpoint());
        } catch (RuntimeException ignored) {
        }
        connection.monitor.close();
        connection.subscriber.close();
    }

    private synchronized void closeConnections() {
        List.copyOf(connections.keySet()).forEach(this::remove);
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
        long value = java.util.concurrent.ThreadLocalRandom.current()
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
        running = false;
        lifecycleEpoch = Math.addExact(lifecycleEpoch, 1);
        closeConnections();
        executor.shutdown();
    }

    List<FanoutPublisherSnapshot> publisherSnapshots(String channelName) {
        return connections.values().stream()
            .filter(connection ->
                connection.descriptor.channelName().equals(channelName))
            .map(connection -> new FanoutPublisherSnapshot(
                connection.descriptor.publisherRid(),
                connection.ready))
            .sorted(java.util.Comparator.comparing(
                snapshot -> snapshot.nodeRid().toHex()))
            .toList();
    }

    record FanoutPublisherSnapshot(
        RoutingId nodeRid,
        boolean ready) {
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
        private volatile boolean nativeReady;
        private volatile boolean ready;

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
