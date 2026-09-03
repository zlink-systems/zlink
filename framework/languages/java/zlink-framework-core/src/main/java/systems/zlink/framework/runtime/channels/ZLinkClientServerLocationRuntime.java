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
import java.util.concurrent.Executor;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
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
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

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
    private final ZLinkStateLane stateLane = new ZLinkStateLane();
    private final Map<String, PublishedServer> published = new LinkedHashMap<>();
    private final Map<String, Connection> connections = new HashMap<>();
    private final ScheduledExecutorService scheduler;
    private final Executor infrastructureExecutor;
    private ScheduledFuture<?> scheduledTick;
    private volatile boolean running;
    private long lifecycleEpoch;
    private CompletableFuture<Void> admittedTick;
    private CompletableFuture<Void> stopCompletion;

    ZLinkClientServerLocationRuntime(
        ZLinkLocationRepository store,
        Supplier<ZLinkLocationOwnerToken> owner,
        ZLinkBackendAdapterProvider backendFactory,
        ZLinkBackendContext context,
        ZLinkBackendAdapterOptions adapterOptions,
        ZLinkChannelSocketRegistry sockets,
        ScheduledExecutorService scheduler,
        Executor infrastructureExecutor,
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
        this.scheduler = Objects.requireNonNull(scheduler, "scheduler");
        this.infrastructureExecutor = Objects.requireNonNull(
            infrastructureExecutor, "infrastructureExecutor");
        this.pollingInterval = Objects.requireNonNull(
            pollingInterval, "pollingInterval");
        this.pageSize = pageSize;
    }

    private <T> T inStateLane(Supplier<T> work) {
        try {
            return stateLane.runAsync(work).toCompletableFuture().join();
        } catch (java.util.concurrent.CompletionException failure) {
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
            lifecycleEpoch = Math.addExact(lifecycleEpoch, 1);
            running = true;
            stopCompletion = null;
            if (surfaces.stream().anyMatch(surface ->
                    surface.type()
                        == ZLinkAutoConnectType.CLIENT_SERVER
                    && surface.role()
                        == ZLinkLocationRole.DEALER)
                && monitoring == null) {
                monitoring = backendFactory.createMonitoringAdapter(
                    adapterOptions);
            }
            return StartState.started(lifecycleEpoch);
        });
        if (start.stopping()) {
            return CompletableFuture.failedFuture(new IllegalStateException(
                "ClientServer location runtime is stopping"));
        }
        if (!start.started()) {
            return CompletableFuture.completedFuture(null);
        }
        streamTrace(STREAM_TRACE ? "client-server-location start surfaces=" + surfaces.size()
            + " owner=" + owner.get().ownerId() : null);
        initializePublishedServers(surfaces);
        return admitTick(surfaces, start.epoch(), false);
    }

    CompletionStage<Void> markDraining() {
        List<CompletionStage<?>> writes = new ArrayList<>();
        List<ZLinkClientServerServerDescriptor> updates = new ArrayList<>();
        ZLinkLocationOwnerToken ownerToken = owner.get();
        inStateLane(() -> {
            for (Map.Entry<String, PublishedServer> entry
                : published.entrySet()) {
                PublishedServer current = entry.getValue();
                ZLinkClientServerServerDescriptor descriptor =
                    descriptor(ownerToken,
                        entry.getKey(),
                        current,
                        current.revision() + 1,
                        current.weight(),
                        ZLinkFrameworkRuntimeState.DRAINING);
                entry.setValue(current.withDescriptor(descriptor));
                updates.add(descriptor);
            }
            return null;
        });
        for (ZLinkClientServerServerDescriptor descriptor : updates) {
            sockets.setClientServerServerDescriptor(
                descriptor.channelName(), descriptor);
            writes.add(store.updateClientServer(
                descriptor, ZLinkLocationWriteIntent.RENEW));
        }
        return all(writes);
    }

    CompletionStage<Void> stop() {
        StopState stop = inStateLane(() -> {
            if (!running) {
                return StopState.alreadyStopped(stopCompletion);
            }
            ScheduledFuture<?> task = scheduledTick;
            scheduledTick = null;
            running = false;
            lifecycleEpoch = Math.addExact(lifecycleEpoch, 1);
            Map<String, PublishedServer> servers = Map.copyOf(published);
            List<String> connectionIds = List.copyOf(connections.keySet());
            CompletableFuture<Void> pendingTick = admittedTick;
            CompletableFuture<Void> completion = new CompletableFuture<>();
            stopCompletion = completion;
            return StopState.stopping(
                servers, connectionIds, pendingTick, completion, task);
        });
        if (!stop.stopping()) {
            return stop.completion() == null
                ? CompletableFuture.completedFuture(null)
                : stop.completion();
        }
        if (stop.task() != null) stop.task().cancel(false);
        for (String connectionId : stop.connectionIds()) {
            removeConnection(connectionId);
        }
        CompletionStage<Void> settled = stop.pendingTick() == null
            ? CompletableFuture.completedFuture(null)
            : stop.pendingTick().handle((ignored, failure) -> null);
        settled.thenCompose(ignored -> removePublishedServers(stop.servers()))
            .whenComplete((ignored, failure) -> {
                if (failure == null) {
                    stop.completion().completeAsync(() -> null);
                } else {
                    stop.completion().completeAsync(() -> {
                        throw new java.util.concurrent.CompletionException(failure);
                    });
                }
            });
        return stop.completion();
    }

    private CompletionStage<Void> removePublishedServers(
        Map<String, PublishedServer> servers) {
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
            if (inStateLane(() -> published.containsKey(surface.meshName()))) {
                continue;
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
            ZLinkClientServerServerDescriptor descriptor = descriptor(owner.get(),
                surface.meshName(),
                server,
                server.revision(),
                server.weight(),
                ZLinkFrameworkRuntimeState.SERVING);
            boolean publishedNow = inStateLane(() -> {
                if (published.containsKey(surface.meshName())) {
                    return false;
                }
                published.put(
                    surface.meshName(), server.withDescriptor(descriptor));
                return true;
            });
            if (!publishedNow) continue;
            sockets.setClientServerServerDescriptor(
                surface.meshName(), descriptor);
            streamTrace(STREAM_TRACE ? "client-server-location publish channel="
                + surface.meshName()
                + " serverRid=" + descriptor.serverRid()
                + " endpoint=" + descriptor.endpoint() : null);
        }
    }

    private CompletionStage<Void> tick(
        List<ZLinkChannelRuntime.AutoConnectSurface> surfaces,
        long epoch) {
        List<CompletionStage<?>> work = new ArrayList<>();
        List<PublishedUpdate> publishes = new ArrayList<>();
        ZLinkLocationOwnerToken ownerToken = owner.get();
        boolean active = inStateLane(() -> {
            if (!running || lifecycleEpoch != epoch) {
                return false;
            }
            for (Map.Entry<String, PublishedServer> entry
                : published.entrySet()) {
                PublishedServer current = entry.getValue();
                publishes.add(new PublishedUpdate(entry.getKey(), current));
            }
            return true;
        });
        if (!active) return CompletableFuture.completedFuture(null);
        for (PublishedUpdate publishing : publishes) {
            String channelName = publishing.channelName();
            PublishedServer server = publishing.server();
            int weight = sockets.clientServerServerWeight(
                channelName, server.weight());
            if (weight != server.weight()) {
                server = inStateLane(() -> {
                    PublishedServer current = published.get(channelName);
                    if (current == null || current != publishing.server()) {
                        return null;
                    }
                    PublishedServer changed = current.withWeight(
                        weight, current.revision() + 1);
                    ZLinkClientServerServerDescriptor descriptor = descriptor(ownerToken,
                        channelName, changed, changed.revision(), weight,
                        ZLinkFrameworkRuntimeState.SERVING);
                    changed = changed.withDescriptor(descriptor);
                    published.put(channelName, changed);
                    return changed;
                });
                if (server == null) continue;
            }
            PublishedServer toPublish = server;
            sockets.setClientServerServerDescriptor(
                channelName, toPublish.descriptor());
            work.add(store.updateClientServer(
                toPublish.descriptor(),
                toPublish.claimed()
                    ? ZLinkLocationWriteIntent.RENEW
                    : ZLinkLocationWriteIntent.NEW_CLAIM)
                .thenAccept(result -> {
                    streamTrace(STREAM_TRACE ? "client-server-location publish-result channel="
                        + channelName
                        + " serverRid=" + toPublish.serverRid()
                        + " status=" + result.status() : null);
                    if (result.status()
                        != ZLinkLocationWriteStatus.STORED) {
                        throw new IllegalStateException(
                            "ClientServer descriptor publication was fenced: "
                                + channelName + "/"
                                + result.status());
                    }
                    inStateLane(() -> {
                        PublishedServer owned = published.get(channelName);
                        if (running
                            && lifecycleEpoch == epoch
                            && owned != null
                            && owned.lifecycleGeneration()
                                == toPublish.lifecycleGeneration()
                            && owned.revision() == toPublish.revision()) {
                            published.put(
                                channelName, owned.withClaimed());
                        }
                        return null;
                    });
                }));
        }
        Set<String> clientChannels = clientChannels(surfaces);
        for (String channelName : clientChannels) {
            streamTrace(STREAM_TRACE ? "client-server-location list-start channel=" + channelName : null);
            work.add(listAll(channelName).thenAccept(
                descriptors -> reconcile(channelName, descriptors, epoch)));
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
                streamTrace(STREAM_TRACE ? "client-server-location list-page channel="
                    + channelName + " count=" + page.items().size()
                    + " continuation=" + page.continuationToken() : null);
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
        List<ZLinkClientServerServerDescriptor> descriptors,
        long epoch) {
        Map<String, Connection> currentConnections = inStateLane(() -> {
            if (!running || lifecycleEpoch != epoch) {
                return null;
            }
            return Map.copyOf(connections);
        });
        if (currentConnections == null) return;
        streamTrace(STREAM_TRACE ? "client-server-location reconcile channel=" + channelName
            + " descriptors=" + descriptors.size() : null);
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
        ZLinkMonitoringBackendAdapter monitoringAdapter = inStateLane(() -> {
            if (!running || connections.containsKey(connectionId)) {
                return null;
            }
            return monitoring;
        });
        if (monitoringAdapter == null) {
            throw new IllegalStateException("ClientServer monitoring is unavailable");
        }
        streamTrace(STREAM_TRACE ? "client-server-location connect channel="
            + descriptor.channelName()
            + " serverRid=" + descriptor.serverRid()
            + " endpoint=" + descriptor.endpoint() : null);
        ZLinkBackendDealerSocket dealer = null;
        ZLinkBackendSocketMonitor monitor = null;
        Connection connection = null;
        try {
            dealer = backend.createDealerSocket(context);
            dealer.setChannelName(descriptor.channelName());
            monitor = monitoringAdapter.openSocketMonitor(dealer);
            connection = new Connection(
                connectionId, descriptor, dealer, false);
            Connection candidate = connection;
            boolean accepted = inStateLane(() -> {
                if (!running || connections.containsKey(connectionId)) {
                    return false;
                }
                connections.put(connectionId, candidate);
                return true;
            });
            if (!accepted) {
                closeUnregistered(dealer, monitor);
                return;
            }
            sockets.addClientServerConnection(connectionId, descriptor, dealer);
            sockets.registerClientServerMonitor(connectionId, monitor);
            Connection acceptedConnection = connection;
            ZLinkBackendDealerSocket acceptedDealer = dealer;
            ZLinkSocketMonitorDrainLoop.start(
                "zlink-client-server-location-monitor", monitor, event -> {
                    if (isConnectionReady(event.event())) {
                        ZLinkChannelSocketRegistry.AdmissionFence fence =
                            sockets.clientServerTransportReady(
                                connectionId, acceptedDealer);
                        requestAdmission(acceptedConnection, fence);
                    } else if (isConnectionTerminated(event.event())) {
                        sockets.clientServerTransportTerminated(
                            connectionId, acceptedDealer);
                        inStateLane(() -> {
                            Connection current = connections.get(connectionId);
                            if (current != null
                                && current.dealer() == acceptedDealer) {
                                connections.put(
                                    connectionId,
                                        current.withExpected(
                                            current.expected(), false));
                            }
                            return null;
                        });
                    }
                });
            dealer.connect(descriptor.endpoint());
        } catch (Throwable failure) {
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
            connection.dealer()
                .request(List.of(message), adapterOptions.defaultRequestTimeout())
                .whenComplete((reply, failure) -> {
                    if (failure == null) {
                        completeAdmission(connection, fence, reply);
                    } else {
                        removeConnection(connection.connectionId(), connection);
                    }
                });
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
            ZLinkClientServerServerDescriptor expected = inStateLane(() -> {
                Connection current = connections.get(connection.connectionId());
                if (current == null
                    || current.dealer() != connection.dealer()
                    || current.expected().descriptorRevision()
                        != connection.expected().descriptorRevision()) {
                    return null;
                }
                return current.expected();
            });
            if (expected == null) return;
            if (reply.result()
                    != systems.zlink.framework.runtime.internal.backend
                        .ZLinkBackendRequestResult.OK
                || reply.parts().size() != 1) {
                removeConnection(connection.connectionId(), connection);
                streamTrace(STREAM_TRACE ? "client-server-location admission-failed connection="
                    + connection.connectionId() + " result=" + reply.result()
                    + " parts=" + reply.parts().size() : null);
                return;
            }
            ZLinkClientServerServiceWire.Control control =
                ZLinkClientServerServiceWire.decode(
                    reply.parts().get(0).toByteArray());
            if (!(control instanceof ZLinkClientServerServiceWire.Admit admit)
                || !matches(admit.admission(), expected)) {
                removeConnection(connection.connectionId(), connection);
                streamTrace(STREAM_TRACE ? "client-server-location admission-mismatch connection="
                    + connection.connectionId() : null);
                return;
            }
            boolean admitted = inStateLane(() -> {
                Connection current = connections.get(connection.connectionId());
                if (current == null
                    || current.dealer() != connection.dealer()
                    || current.expected().descriptorRevision()
                        != expected.descriptorRevision()) {
                    return false;
                }
                connections.put(
                    connection.connectionId(), current.withExpected(expected, true));
                return true;
            });
            if (!admitted) return;
            if (!sockets.admitClientServerConnection(
                connection.connectionId(), expected, fence)) {
                inStateLane(() -> {
                    Connection current = connections.get(connection.connectionId());
                    if (current != null
                        && current.dealer() == connection.dealer()
                        && current.expected().descriptorRevision()
                            == expected.descriptorRevision()) {
                        connections.put(
                            connection.connectionId(),
                            current.withExpected(expected, false));
                    }
                    return null;
                });
                streamTrace(STREAM_TRACE ? "client-server-location admission-fence-rejected connection="
                    + connection.connectionId() : null);
                removeConnection(connection.connectionId(), connection);
                return;
            }
            streamTrace(STREAM_TRACE ? "client-server-location admission-ready channel="
                + expected.channelName()
                + " serverRid=" + expected.serverRid() : null);
            List<String> superseded = inStateLane(() -> {
                List<String> values = new ArrayList<>();
                for (Connection other : connections.values()) {
                    if (!other.connectionId().equals(connection.connectionId())
                        && other.expected().channelName().equals(
                            expected.channelName())
                        && other.expected().serverRid().equals(
                            expected.serverRid())) {
                        values.add(other.connectionId());
                    }
                }
                return values;
            });
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
        Connection current = inStateLane(() -> {
            Connection candidate = connections.get(connectionId);
            if (candidate == null
                || (expectedConnection != null
                    && candidate != expectedConnection)) {
                return null;
            }
            connections.remove(connectionId);
            return candidate;
        });
        if (current == null) return;
        sockets.removeClientServerConnection(connectionId, current.dealer());
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
        inStateLane(() -> {
            if (connections.get(connectionId) == expectedCurrent) {
                connections.put(connectionId, replacement);
            }
            return null;
        });
    }

    private static ZLinkClientServerServerDescriptor descriptor(
        ZLinkLocationOwnerToken ownerToken,
        String channelName,
        PublishedServer server,
        long revision,
        int weight,
        ZLinkFrameworkRuntimeState state) {
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
        List<ZLinkChannelRuntime.AutoConnectSurface> surfaces,
        long epoch) {
        boolean active = inStateLane(() ->
            running && lifecycleEpoch == epoch);
        if (!active) return;
        ScheduledFuture<?> scheduled = scheduler.schedule(
            () -> admitTick(surfaces, epoch, true),
            pollingInterval.toMillis(),
            TimeUnit.MILLISECONDS);
        boolean cancel = inStateLane(() -> {
            if (!running || lifecycleEpoch != epoch) return true;
            scheduledTick = scheduled;
            return false;
        });
        if (cancel) scheduled.cancel(false);
    }

    private CompletionStage<Void> admitTick(
        List<ZLinkChannelRuntime.AutoConnectSurface> surfaces,
        long epoch,
        boolean retryAfterFailure) {
        CompletableFuture<Void> settlement = inStateLane(() -> {
            if (!running || lifecycleEpoch != epoch) {
                return null;
            }
            if (admittedTick != null) {
                return admittedTick;
            }
            CompletableFuture<Void> next = new CompletableFuture<>();
            admittedTick = next;
            return next;
        });
        if (settlement == null) return CompletableFuture.completedFuture(null);
        try {
            infrastructureExecutor.execute(
                () -> runAdmittedTick(
                    surfaces, epoch, retryAfterFailure, settlement));
        } catch (RejectedExecutionException closing) {
            settleTick(
                surfaces,
                epoch,
                retryAfterFailure,
                settlement,
                closing);
        }
        return settlement;
    }

    private void runAdmittedTick(
        List<ZLinkChannelRuntime.AutoConnectSurface> surfaces,
        long epoch,
        boolean retryAfterFailure,
        CompletableFuture<Void> settlement) {
        CompletionStage<Void> work;
        try {
            work = tick(surfaces, epoch);
        } catch (RuntimeException failure) {
            settleTick(
                surfaces,
                epoch,
                retryAfterFailure,
                settlement,
                failure);
            return;
        }
        work.whenComplete((ignored, failure) -> settleTick(
            surfaces,
            epoch,
            retryAfterFailure,
            settlement,
            failure));
    }

    private void settleTick(
        List<ZLinkChannelRuntime.AutoConnectSurface> surfaces,
        long epoch,
        boolean retryAfterFailure,
        CompletableFuture<Void> settlement,
        Throwable failure) {
        boolean scheduleNext = inStateLane(() -> {
            if (admittedTick == settlement) {
                admittedTick = null;
            }
            return running
                && lifecycleEpoch == epoch
                && (failure == null || retryAfterFailure);
        });
        Throwable terminalFailure = failure;
        if (scheduleNext) {
            try {
                schedule(surfaces, epoch);
            } catch (RuntimeException rejected) {
                terminalFailure = rejected;
            }
        }
        if (terminalFailure == null) {
            settlement.completeAsync(() -> null);
        } else {
            Throwable failureToComplete = terminalFailure;
            settlement.completeAsync(() -> {
                throw new java.util.concurrent.CompletionException(failureToComplete);
            });
        }
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

    private record StartState(boolean started, boolean stopping, long epoch) {
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
        Map<String, PublishedServer> servers,
        List<String> connectionIds,
        CompletableFuture<Void> pendingTick,
        CompletableFuture<Void> completion,
        ScheduledFuture<?> task) {
        private static StopState alreadyStopped(
            CompletableFuture<Void> completion) {
            return new StopState(false, Map.of(), List.of(), null, completion, null);
        }

        private static StopState stopping(
            Map<String, PublishedServer> servers,
            List<String> connectionIds,
            CompletableFuture<Void> pendingTick,
            CompletableFuture<Void> completion,
            ScheduledFuture<?> task) {
            return new StopState(
                true, servers, connectionIds, pendingTick, completion, task);
        }
    }
}
