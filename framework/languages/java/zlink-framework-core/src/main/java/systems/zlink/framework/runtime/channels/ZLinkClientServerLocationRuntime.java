package systems.zlink.framework.runtime.channels;
import java.util.concurrent.ThreadLocalRandom;
import systems.zlink.framework.locations.ZLinkLocationRole;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.locations.ZLinkAutoConnectType;

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
import java.util.function.Supplier;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.logging.Logger;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkClientServerServerDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkClientServerServerDescriptorKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteIntent;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteStatus;
import systems.zlink.framework.locations.ZLinkPageRequest;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterOptions;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendContext;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendDealerSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitor;
import systems.zlink.framework.runtime.internal.backend.ZLinkMonitoringBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkChannelBackendAdapter;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;

final class ZLinkClientServerLocationRuntime implements AutoCloseable {
    private static final String SECURITY_IDENTITY = "default";
    private static final boolean STREAM_TRACE =
        "1".equals(System.getenv("ZLINK_JAVA_STREAM_TRACE"));
    private static final Logger LOGGER =
        Logger.getLogger(ZLinkClientServerLocationRuntime.class.getName());

    private final ZLinkLocationRepository store;
    private final Supplier<ZLinkLocationOwnerToken> owner;
    private final ZLinkChannelBackendAdapter backend;
    private final ZLinkBackendAdapterProvider backendFactory;
    private final ZLinkBackendContext context;
    private final ZLinkBackendAdapterOptions adapterOptions;
    private final ZLinkChannelSocketRegistry sockets;
    private ZLinkMonitoringBackendAdapter monitoring;
    private final Duration pollingInterval;
    private final int pageSize;
    private final Map<String, PublishedServer> published = new LinkedHashMap<>();
    private final Map<String, Connection> connections = new HashMap<>();
    private final ScheduledExecutorService executor =
        Executors.newSingleThreadScheduledExecutor(task -> {
            Thread thread = new Thread(
                task, "zlink-java-client-server-location");
            thread.setDaemon(true);
            return thread;
        });
    private volatile boolean running;

    ZLinkClientServerLocationRuntime(
        ZLinkLocationRepository store,
        Supplier<ZLinkLocationOwnerToken> owner,
        ZLinkBackendAdapterProvider backendFactory,
        ZLinkBackendContext context,
        ZLinkBackendAdapterOptions adapterOptions,
        ZLinkChannelSocketRegistry sockets,
        Duration pollingInterval,
        int pageSize) {
        this.store = Objects.requireNonNull(store, "store");
        this.owner = Objects.requireNonNull(owner, "owner");
        this.backendFactory = Objects.requireNonNull(
            backendFactory, "backendFactory");
        this.backend = this.backendFactory
            .createChannelAdapter(adapterOptions);
        this.context = Objects.requireNonNull(context, "context");
        this.adapterOptions = Objects.requireNonNull(
            adapterOptions, "adapterOptions");
        this.sockets = Objects.requireNonNull(sockets, "sockets");
        this.pollingInterval = Objects.requireNonNull(
            pollingInterval, "pollingInterval");
        this.pageSize = pageSize;
    }

    CompletionStage<Void> start(
        List<ZLinkChannelRuntime.AutoConnectSurface> surfaces) {
        synchronized (this) {
            if (running) {
                return CompletableFuture.completedFuture(null);
            }
            running = true;
            if (surfaces.stream().anyMatch(surface ->
                    surface.type()
                        == ZLinkAutoConnectType.CLIENT_SERVER
                    && surface.role()
                        == ZLinkLocationRole.DEALER)
                && monitoring == null) {
                monitoring = backendFactory.createMonitoringAdapter(
                    adapterOptions);
            }
        }
        streamTrace("client-server-location start surfaces=" + surfaces.size()
            + " owner=" + owner.get().ownerId());
        initializePublishedServers(surfaces);
        return tick(surfaces).whenComplete((ignored, failure) -> {
            if (failure == null) {
                schedule(surfaces);
            }
        });
    }

    CompletionStage<Void> markDraining() {
        List<CompletionStage<?>> writes = new ArrayList<>();
        List<ZLinkClientServerServerDescriptor> updates = new ArrayList<>();
        synchronized (this) {
            for (Map.Entry<String, PublishedServer> entry
                : published.entrySet()) {
                PublishedServer current = entry.getValue();
                ZLinkClientServerServerDescriptor descriptor =
                    descriptor(
                        entry.getKey(),
                        current,
                        current.revision() + 1,
                        current.weight(),
                        ZLinkFrameworkRuntimeState.DRAINING);
                entry.setValue(current.withDescriptor(descriptor));
                updates.add(descriptor);
            }
        }
        for (ZLinkClientServerServerDescriptor descriptor : updates) {
            sockets.setClientServerServerDescriptor(
                descriptor.channelName(), descriptor);
            writes.add(store.updateClientServer(
                descriptor, ZLinkLocationWriteIntent.RENEW));
        }
        return all(writes);
    }

    CompletionStage<Void> stop() {
        Map<String, PublishedServer> servers;
        synchronized (this) {
            if (!running) {
                return CompletableFuture.completedFuture(null);
            }
            running = false;
            servers = Map.copyOf(published);
        }
        List<String> connectionIds;
        synchronized (this) {
            connectionIds = List.copyOf(connections.keySet());
        }
        for (String connectionId : connectionIds) {
            removeConnection(connectionId);
        }
        executor.shutdown();
        if (servers.isEmpty()) {
            return CompletableFuture.completedFuture(null);
        }
        ZLinkLocationOwnerToken ownerToken = owner.get();
        List<CompletionStage<?>> removals = new ArrayList<>();
        for (Map.Entry<String, PublishedServer> entry : servers.entrySet()) {
            PublishedServer server = entry.getValue();
            sockets.setClientServerServerDescriptor(entry.getKey(), null);
            removals.add(store.removeClientServer(
                new ZLinkClientServerServerDescriptorKey(
                    entry.getKey(), server.serverRid()),
                ownerToken));
        }
        return all(removals);
    }

    private void initializePublishedServers(
        List<ZLinkChannelRuntime.AutoConnectSurface> surfaces) {
        for (ZLinkChannelRuntime.AutoConnectSurface surface : surfaces) {
            if (surface.type()
                    != ZLinkAutoConnectType.CLIENT_SERVER
                || surface.role()
                    != ZLinkLocationRole.ROUTER) {
                continue;
            }
            synchronized (this) {
                if (published.containsKey(surface.meshName())) {
                    continue;
                }
            }
            ZLinkClientServerServerDescriptor local =
                sockets.clientServerServerDescriptor(surface.meshName());
            PublishedServer server = new PublishedServer(
                local == null ? surface.nodeRid() : local.serverRid(),
                local == null
                    ? positiveRandomLong()
                    : local.lifecycleGeneration(),
                local == null ? 1 : local.descriptorRevision(),
                local == null ? surface.endpoint() : local.endpoint(),
                local == null ? surface.weight() : local.weight(),
                null,
                false);
            ZLinkClientServerServerDescriptor descriptor = descriptor(
                surface.meshName(),
                server,
                server.revision(),
                server.weight(),
                ZLinkFrameworkRuntimeState.SERVING);
            synchronized (this) {
                if (published.containsKey(surface.meshName())) {
                    continue;
                }
                published.put(
                    surface.meshName(), server.withDescriptor(descriptor));
            }
            sockets.setClientServerServerDescriptor(
                surface.meshName(), descriptor);
            streamTrace("client-server-location publish channel="
                + surface.meshName()
                + " serverRid=" + descriptor.serverRid()
                + " endpoint=" + descriptor.endpoint());
        }
    }

    private CompletionStage<Void> tick(
        List<ZLinkChannelRuntime.AutoConnectSurface> surfaces) {
        List<CompletionStage<?>> work = new ArrayList<>();
        List<PublishedUpdate> publishes = new ArrayList<>();
        synchronized (this) {
            if (!running) {
                return CompletableFuture.completedFuture(null);
            }
            for (Map.Entry<String, PublishedServer> entry
                : published.entrySet()) {
                int weight = sockets.clientServerServerWeight(
                    entry.getKey(), entry.getValue().weight());
                PublishedServer current = entry.getValue();
                if (weight != current.weight()) {
                    current = current.withWeight(
                        weight, current.revision() + 1);
                    ZLinkClientServerServerDescriptor changed = descriptor(
                        entry.getKey(),
                        current,
                        current.revision(),
                        weight,
                        ZLinkFrameworkRuntimeState.SERVING);
                    current = current.withDescriptor(changed);
                    entry.setValue(current);
                }
                publishes.add(new PublishedUpdate(entry.getKey(), current));
            }
        }
        for (PublishedUpdate publishing : publishes) {
            String channelName = publishing.channelName();
            PublishedServer server = publishing.server();
            sockets.setClientServerServerDescriptor(
                channelName, server.descriptor());
            work.add(store.updateClientServer(
                server.descriptor(),
                server.claimed()
                    ? ZLinkLocationWriteIntent.RENEW
                    : ZLinkLocationWriteIntent.NEW_CLAIM)
                .thenAccept(result -> {
                    streamTrace("client-server-location publish-result channel="
                        + channelName
                        + " serverRid=" + server.serverRid()
                        + " status=" + result.status());
                    if (result.status()
                        != ZLinkLocationWriteStatus.STORED) {
                        throw new IllegalStateException(
                            "ClientServer descriptor publication was fenced: "
                                + channelName + "/"
                                + result.status());
                    }
                    synchronized (this) {
                        PublishedServer owned = published.get(channelName);
                        if (owned != null
                            && owned.lifecycleGeneration()
                                == server.lifecycleGeneration()
                            && owned.revision() == server.revision()) {
                            published.put(
                                channelName, owned.withClaimed());
                        }
                    }
                }));
        }
        Set<String> clientChannels = clientChannels(surfaces);
        for (String channelName : clientChannels) {
            streamTrace("client-server-location list-start channel=" + channelName);
            work.add(listAll(channelName).thenAccept(
                descriptors -> reconcile(channelName, descriptors)));
        }
        return all(work);
    }

    private CompletionStage<List<ZLinkClientServerServerDescriptor>> listAll(
        String channelName) {
        List<ZLinkClientServerServerDescriptor> result = new ArrayList<>();
        return listPage(channelName, null, result).thenApply(ignored -> result);
    }

    private CompletionStage<Void> listPage(
        String channelName,
        String cursor,
        List<ZLinkClientServerServerDescriptor> result) {
        return store.listClientServers(
            channelName, new ZLinkPageRequest(pageSize, cursor))
            .thenCompose(page -> {
                streamTrace("client-server-location list-page channel="
                    + channelName + " count=" + page.items().size()
                    + " continuation=" + page.continuationToken());
                result.addAll(page.items());
                return page.continuationToken() == null
                    || page.continuationToken().isBlank()
                        ? CompletableFuture.completedFuture(null)
                        : listPage(
                            channelName,
                            page.continuationToken(),
                            result);
            });
    }

    private void reconcile(
        String channelName,
        List<ZLinkClientServerServerDescriptor> descriptors) {
        Map<String, Connection> currentConnections;
        synchronized (this) {
            if (!running) {
                return;
            }
            currentConnections = Map.copyOf(connections);
        }
        streamTrace("client-server-location reconcile channel=" + channelName
            + " descriptors=" + descriptors.size());
        Map<String, ZLinkClientServerServerDescriptor> desired =
            new LinkedHashMap<>();
        for (ZLinkClientServerServerDescriptor descriptor : descriptors) {
            if (!descriptor.channelName().equals(channelName)
                || descriptor.state() == ZLinkFrameworkRuntimeState.STOPPED
                || descriptor.state() == ZLinkFrameworkRuntimeState.ERROR) {
                continue;
            }
            desired.put(connectionId(descriptor), descriptor);
        }

        for (Map.Entry<String, ZLinkClientServerServerDescriptor> entry
            : desired.entrySet()) {
            Connection current = currentConnections.get(entry.getKey());
            if (current == null) {
                openConnection(entry.getKey(), entry.getValue());
            } else if (entry.getValue().descriptorRevision()
                > current.expected().descriptorRevision()) {
                if (!sockets.ownsClientServerPhysical(
                        entry.getKey(), current.dealer())) {
                    replaceConnectionState(
                        entry.getKey(), current,
                        current.withExpected(entry.getValue(), true));
                    continue;
                }
                if (current.ready()) {
                    // A descriptor revision changes routing policy such as
                    // weight or serving state. The existing physical
                    // connection remains valid; re-admitting it would put a
                    // second request on the same DEALER while an accepted
                    // application request may still be waiting for its
                    // reply. The control update already carries the new
                    // descriptor to this connection, so preserve readiness
                    // and let in-flight work finish on the same transport.
                    replaceConnectionState(
                        entry.getKey(), current,
                        current.withExpected(entry.getValue(), true));
                    sockets.updateClientServerConnection(
                        entry.getKey(), entry.getValue(), true);
                    continue;
                }
                Connection pending = current.withExpected(
                    entry.getValue(), false);
                replaceConnectionState(entry.getKey(), current, pending);
                sockets.updateClientServerConnection(
                    entry.getKey(), entry.getValue(), false);
                ZLinkChannelSocketRegistry.AdmissionFence fence =
                    sockets.clientServerTransportReady(
                        entry.getKey(), current.dealer());
                requestAdmission(pending, fence);
            }
        }

        Set<String> desiredIds = desired.keySet();
        for (Connection current : currentConnections.values()) {
            if (!current.expected().channelName().equals(channelName)
                || desiredIds.contains(current.connectionId())) {
                continue;
            }
            boolean replacementPending = desired.values().stream().anyMatch(
                descriptor -> descriptor.serverRid().equals(
                    current.expected().serverRid())
                    && currentConnections.containsKey(connectionId(descriptor))
                    && !currentConnections.get(connectionId(descriptor)).ready());
            if (!replacementPending) {
                removeConnection(current.connectionId(), current);
            }
        }
    }

    private void openConnection(
        String connectionId,
        ZLinkClientServerServerDescriptor descriptor) {
        ZLinkMonitoringBackendAdapter monitoringAdapter;
        synchronized (this) {
            if (!running || connections.containsKey(connectionId)) {
                return;
            }
            monitoringAdapter = monitoring;
        }
        if (monitoringAdapter == null) {
            throw new IllegalStateException("ClientServer monitoring is unavailable");
        }
        streamTrace("client-server-location connect channel="
            + descriptor.channelName()
            + " serverRid=" + descriptor.serverRid()
            + " endpoint=" + descriptor.endpoint());
        ZLinkBackendDealerSocket dealer = null;
        ZLinkBackendSocketMonitor monitor = null;
        Connection connection = null;
        try {
            dealer = backend.createDealerSocket(context);
            dealer.setChannelName(descriptor.channelName());
            monitor = monitoringAdapter.openSocketMonitor(dealer);
            connection = new Connection(
                connectionId, descriptor, dealer, false);
            synchronized (connection) {
                boolean accepted;
                synchronized (this) {
                    accepted = running
                        && !connections.containsKey(connectionId);
                    if (accepted) {
                        connections.put(connectionId, connection);
                    }
                }
                if (!accepted) {
                    closeUnregistered(dealer, monitor);
                    return;
                }
                // Keep registration and the first connect under the same
                // per-connection ownership fence. stop/remove waits for this
                // block, so a removed dealer cannot be connected afterwards.
                sockets.addClientServerConnection(
                    connectionId, descriptor, dealer);
                sockets.registerClientServerMonitor(connectionId, monitor);
                Connection acceptedConnection = connection;
                ZLinkBackendDealerSocket acceptedDealer = dealer;
                monitor.onEvent(event -> {
                    if (isConnectionReady(event.event())) {
                        ZLinkChannelSocketRegistry.AdmissionFence fence =
                            sockets.clientServerTransportReady(
                                connectionId, acceptedDealer);
                        requestAdmission(acceptedConnection, fence);
                    } else if (isConnectionTerminated(event.event())) {
                        sockets.clientServerTransportTerminated(
                            connectionId, acceptedDealer);
                        synchronized (this) {
                            Connection current = connections.get(connectionId);
                            if (current != null
                                && current.dealer() == acceptedDealer) {
                                connections.put(
                                    connectionId,
                                    current.withExpected(
                                        current.expected(), false));
                            }
                        }
                    }
                });
                dealer.connect(descriptor.endpoint());
            }
        } catch (RuntimeException failure) {
            if (connection != null) {
                removeConnection(connectionId, connection);
            } else {
                closeUnregistered(dealer, monitor);
            }
            throw failure;
        }
    }

    private void requestAdmission(
        Connection connection,
        ZLinkChannelSocketRegistry.AdmissionFence fence) {
        if (fence == null) {
            return;
        }
        byte[] hello = ZLinkClientServerServiceWire.encodeHello(
            new ZLinkClientServerServiceWire.Hello(
                connection.expected().channelName(),
                connection.expected().securityIdentity(),
                Integer.MAX_VALUE));
        try (Message message = Message.from(hello)) {
            boolean submitted = connection.dealer().request(
                List.of(message),
                reply -> completeAdmission(connection, fence, reply),
                SendFlags.DONT_WAIT,
                adapterOptions.defaultRequestTimeout());
            if (!submitted) {
                removeConnection(connection.connectionId(), connection);
            }
        } catch (RuntimeException failure) {
            // A connection can terminate between the monitor readiness event
            // and the admission request. The monitor callback must not leak
            // that binding exception; removing the stale connection lets the
            // next location refresh establish a fresh admission attempt.
            removeConnection(connection.connectionId(), connection);
        }
    }

    private void completeAdmission(
        Connection connection,
        ZLinkChannelSocketRegistry.AdmissionFence fence,
        ZLinkBackendReceived reply) {
        try (reply) {
            ZLinkClientServerServerDescriptor expected;
            synchronized (this) {
                Connection current = connections.get(connection.connectionId());
                if (current == null
                    || current.dealer() != connection.dealer()
                    || current.expected().descriptorRevision()
                        != connection.expected().descriptorRevision()) {
                    return;
                }
                expected = current.expected();
            }
            if (reply.result()
                    != systems.zlink.framework.runtime.internal.backend
                        .ZLinkBackendRequestResult.OK
                || reply.parts().size() != 1) {
                removeConnection(connection.connectionId(), connection);
                streamTrace("client-server-location admission-failed connection="
                    + connection.connectionId() + " result=" + reply.result()
                    + " parts=" + reply.parts().size());
                return;
            }
            ZLinkClientServerServiceWire.Control control =
                ZLinkClientServerServiceWire.decode(
                    reply.parts().get(0).toByteArray());
            if (!(control instanceof ZLinkClientServerServiceWire.Admit admit)
                || !matches(admit.admission(), expected)) {
                removeConnection(connection.connectionId(), connection);
                streamTrace("client-server-location admission-mismatch connection="
                    + connection.connectionId());
                return;
            }
            synchronized (this) {
                Connection current = connections.get(connection.connectionId());
                if (current == null
                    || current.dealer() != connection.dealer()
                    || current.expected().descriptorRevision()
                        != expected.descriptorRevision()) {
                    return;
                }
                connections.put(
                    connection.connectionId(), current.withExpected(expected, true));
            }
            if (!sockets.admitClientServerConnection(
                connection.connectionId(), expected, fence)) {
                synchronized (this) {
                    Connection current = connections.get(connection.connectionId());
                    if (current != null
                        && current.dealer() == connection.dealer()
                        && current.expected().descriptorRevision()
                            == expected.descriptorRevision()) {
                        connections.put(
                            connection.connectionId(),
                            current.withExpected(expected, false));
                    }
                }
                streamTrace("client-server-location admission-fence-rejected connection="
                    + connection.connectionId());
                removeConnection(connection.connectionId(), connection);
                return;
            }
            streamTrace("client-server-location admission-ready channel="
                + expected.channelName()
                + " serverRid=" + expected.serverRid());
            List<String> superseded = new ArrayList<>();
            synchronized (this) {
                for (Connection other : connections.values()) {
                    if (!other.connectionId().equals(connection.connectionId())
                        && other.expected().channelName().equals(
                            expected.channelName())
                        && other.expected().serverRid().equals(
                            expected.serverRid())) {
                        superseded.add(other.connectionId());
                    }
                }
            }
            for (String supersededId : superseded) {
                removeConnection(supersededId);
            }
        } catch (RuntimeException failure) {
            removeConnection(connection.connectionId(), connection);
        }
    }

    private void removeConnection(String connectionId) {
        removeConnection(connectionId, null);
    }

    private void removeConnection(
        String connectionId,
        Connection expectedConnection) {
        Connection current;
        synchronized (this) {
            current = connections.get(connectionId);
            if (current == null
                || (expectedConnection != null
                    && current != expectedConnection)) {
                return;
            }
        }
        synchronized (current) {
            // Keep the per-connection fence before the runtime map fence.
            // openConnection uses the same order while it registers and
            // connects the dealer; removal must not invert it and deadlock
            // a concurrent stale cleanup.
            synchronized (this) {
                if (connections.get(connectionId) != current) {
                    return;
                }
                connections.remove(connectionId);
            }
            sockets.removeClientServerConnection(connectionId, current.dealer());
        }
    }

    private static void closeUnregistered(
        ZLinkBackendDealerSocket dealer,
        ZLinkBackendSocketMonitor monitor) {
        if (monitor != null) {
            try {
                monitor.close();
            } catch (RuntimeException ignored) {
            }
        }
        if (dealer != null) {
            try {
                dealer.close();
            } catch (RuntimeException ignored) {
            }
        }
    }

    private void replaceConnectionState(
        String connectionId,
        Connection expectedCurrent,
        Connection replacement) {
        synchronized (this) {
            if (connections.get(connectionId) == expectedCurrent) {
                connections.put(connectionId, replacement);
            }
        }
    }

    private ZLinkClientServerServerDescriptor descriptor(
        String channelName,
        PublishedServer server,
        long revision,
        int weight,
        ZLinkFrameworkRuntimeState state) {
        ZLinkLocationOwnerToken ownerToken = owner.get();
        return new ZLinkClientServerServerDescriptor(
            channelName,
            server.serverRid(),
            server.lifecycleGeneration(),
            revision,
            server.endpoint(),
            weight,
            state,
            SECURITY_IDENTITY,
            ownerToken.ownerId(),
            ownerToken.leaseGeneration(),
            Instant.EPOCH);
    }

    private void schedule(
        List<ZLinkChannelRuntime.AutoConnectSurface> surfaces) {
        if (!running) {
            return;
        }
        executor.schedule(
            () -> tick(surfaces).whenComplete(
                (ignored, failure) -> schedule(surfaces)),
            pollingInterval.toMillis(),
            TimeUnit.MILLISECONDS);
    }

    private static boolean matches(
        ZLinkClientServerServiceWire.Admission actual,
        ZLinkClientServerServerDescriptor expected) {
        return actual.channelName().equals(expected.channelName())
            && actual.serverRid().equals(expected.serverRid())
            && actual.lifecycleGeneration()
                == expected.lifecycleGeneration()
            && actual.descriptorRevision()
                == expected.descriptorRevision()
            && actual.weight() == expected.weight()
            && actual.state() == expected.state()
            && actual.securityIdentity().equals(
                expected.securityIdentity())
            && actual.advertisedEndpoint().equals(expected.endpoint());
    }

    private static Set<String> clientChannels(
        List<ZLinkChannelRuntime.AutoConnectSurface> surfaces) {
        Set<String> result = new HashSet<>();
        for (ZLinkChannelRuntime.AutoConnectSurface surface : surfaces) {
            if (surface.type()
                    == ZLinkAutoConnectType.CLIENT_SERVER
                && surface.role()
                    == ZLinkLocationRole.DEALER
                && surface.manualEndpoints().isEmpty()) {
                result.add(surface.meshName());
            }
        }
        return result;
    }

    private static boolean isConnectionReady(String event) {
        return "CONNECTION_READY".equals(event)
            || "ConnectionReady".equals(event);
    }

    private static void streamTrace(String message) {
        if (STREAM_TRACE) {
            LOGGER.warning("[zlink-java-stream-trace] " + message);
        }
    }

    private static boolean isConnectionTerminated(String event) {
        return "DISCONNECTED".equals(event)
            || "CLOSED".equals(event)
            || "HANDSHAKE_FAILED_NO_DETAIL".equals(event)
            || "HANDSHAKE_FAILED_PROTOCOL".equals(event)
            || "HANDSHAKE_FAILED_AUTH".equals(event)
            || "Disconnected".equals(event)
            || "Closed".equals(event);
    }

    static String connectionId(
        ZLinkClientServerServerDescriptor descriptor) {
        return descriptor.channelName()
            + '\0' + descriptor.serverRid().toHex()
            + '\0' + descriptor.lifecycleGeneration();
    }

    private static long positiveRandomLong() {
        return ThreadLocalRandom.current()
            .nextLong(1, Long.MAX_VALUE);
    }

    private static CompletionStage<Void> all(
        List<CompletionStage<?>> stages) {
        return CompletableFuture.allOf(stages.stream()
            .map(CompletionStage::toCompletableFuture)
            .toArray(CompletableFuture[]::new));
    }

    @Override
    public void close() {
        stop();
    }

    private record PublishedServer(
        RoutingId serverRid,
        long lifecycleGeneration,
        long revision,
        String endpoint,
        int weight,
        ZLinkClientServerServerDescriptor descriptor,
        boolean claimed) {
        PublishedServer withDescriptor(
            ZLinkClientServerServerDescriptor value) {
            return new PublishedServer(
                serverRid,
                lifecycleGeneration,
                revision,
                endpoint,
                weight,
                value,
                claimed);
        }

        PublishedServer withWeight(int value, long nextRevision) {
            return new PublishedServer(
                serverRid,
                lifecycleGeneration,
                nextRevision,
                endpoint,
                value,
                descriptor,
                claimed);
        }

        PublishedServer withClaimed() {
            return new PublishedServer(
                serverRid,
                lifecycleGeneration,
                revision,
                endpoint,
                weight,
                descriptor,
                true);
        }
    }

    private record Connection(
        String connectionId,
        ZLinkClientServerServerDescriptor expected,
        ZLinkBackendDealerSocket dealer,
        boolean ready) {
        Connection withExpected(
            ZLinkClientServerServerDescriptor value,
            boolean nextReady) {
            return new Connection(
                connectionId, value, dealer, nextReady);
        }
    }

    private record PublishedUpdate(
        String channelName,
        PublishedServer server) {
    }
}
