package systems.zlink.framework.runtime.channels;
import systems.zlink.framework.runtime.internal.transport.ZLinkEndpointNotation;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashSet;
import java.util.concurrent.ThreadLocalRandom;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.locks.LockSupport;
import java.util.stream.Collectors;
import systems.zlink.contracts.errors.ZlinkCloseException;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.IdentityHashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CompletionException;
import java.time.Duration;
import java.time.Instant;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.internal.locations.ZLinkClientServerServerDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkAutoConnectType;
import systems.zlink.framework.locations.ZLinkLocationRole;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendDealerSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendObject;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendPublisherSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRouterSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRecvMode;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotRouteBridge;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSubscriberSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitor;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkReceiveBatchBudget;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue;
import systems.zlink.framework.runtime.internal.dispatch
    .ZLinkApplicationJobReceiveFlowController;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.monitoring.ZLinkListenerKind;
import java.util.function.Supplier;

final class ZLinkChannelSocketRegistry {
    private static final long READY_POLL_INTERVAL_MILLIS = 5;

    private final Map<String, ChannelRegistration> registrations = new HashMap<>();
    private final ZLinkApplicationJobQueue applicationJobQueue;
    private final ZLinkStateLane stateLane = new ZLinkStateLane();
    private final Map<ZLinkBackendObject,
        ZLinkApplicationJobReceiveFlowController.Registration>
        receiveFlowRegistrations = new IdentityHashMap<>();
    private final Map<String, ZLinkBackendDealerSocket> clients = new HashMap<>();
    private final Map<String, ZLinkBackendRouterSocket> servers = new HashMap<>();
    private final Map<String, RoutingId> serverRoutingIds = new HashMap<>();
    private final Map<String, ZLinkBackendPublisherSocket> publishers = new HashMap<>();
    private final Map<String, RoutingId> publisherRoutingIds = new HashMap<>();
    private final Map<String, ZLinkBackendSubscriberSocket> subscribers = new HashMap<>();
    private final Map<String, ZLinkBackendRouterSocket> routeRouters = new HashMap<>();
    private final Map<String, ClientServerConnection> clientServerConnections =
        new HashMap<>();
    private final Map<String, ZLinkClientServerServerDescriptor>
        clientServerServerDescriptors = new HashMap<>();
    private final Map<String, Map<String, Long>> clientServerSelectionCurrents =
        new HashMap<>();
    private final Map<String, Object> routeSocketLocks = new HashMap<>();
    private final Map<String, ZLinkBackendSpotRouteBridge> spotRouteBridges =
        new ConcurrentHashMap<>();
    private final Map<String, ZLinkInternalSpotNode> spotRouterNodes = new HashMap<>();
    private final List<ZLinkBackendObject> ownedSockets = new ArrayList<>();
    private final Map<String, ClientServerServerPeer> clientServerServerPeers =
        new HashMap<>();
    private long nextClientServerProbeId = 1;
    private long clientServerControlCursor;
    private boolean unmanagedBackendClientMode;
    private static final long CLIENT_SERVER_PROBE_INTERVAL_NANOS =
        TimeUnit.SECONDS.toNanos(5);
    private static final long CLIENT_SERVER_DEADLINE_NANOS =
        TimeUnit.SECONDS.toNanos(15);

    ZLinkChannelSocketRegistry() {
        this(null);
    }

    ZLinkChannelSocketRegistry(ZLinkApplicationJobQueue applicationJobQueue) {
        this.applicationJobQueue = applicationJobQueue;
    }

    // The package-private surface remains synchronous: registration and
    // selection were complete before its callers returned under the monitor.
    // Do not post these turns asynchronously, or callers can observe a
    // partially registered channel.
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

    void registerChannel(ChannelRegistration registration) {
        inStateLane(() -> {
            registrations.put(registration.name(), registration);
            return null;
        });
    }

    ChannelRegistration registration(String channelName) {
        return inStateLane(() -> registrations.get(channelName));
    }

    void registerClient(String channelName, ZLinkBackendDealerSocket socket) {
        registerReceiveFlow(socket);
        inStateLane(() -> {
            clients.put(channelName, socket);
            ownedSockets.add(socket);
            return null;
        });
    }

    void registerServer(
        String channelName,
        RoutingId routingId,
        ZLinkBackendRouterSocket socket) {
        registerReceiveFlow(socket);
        inStateLane(() -> {
            servers.put(channelName, socket);
            serverRoutingIds.put(channelName, routingId);
            ownedSockets.add(socket);
            return null;
        });
    }

    void registerPublisher(
        String channelName,
        RoutingId routingId,
        ZLinkBackendPublisherSocket socket) {
        inStateLane(() -> {
            publishers.put(channelName, socket);
            publisherRoutingIds.put(channelName, routingId);
            ownedSockets.add(socket);
            return null;
        });
    }

    void registerSubscriber(String channelName, ZLinkBackendSubscriberSocket socket) {
        inStateLane(() -> {
            subscribers.put(channelName, socket);
            ownedSockets.add(socket);
            return null;
        });
    }

    void registerRouteRouter(String channelName, ZLinkBackendRouterSocket socket) {
        registerReceiveFlow(socket);
        inStateLane(() -> {
            routeRouters.put(channelName, socket);
            routeSocketLocks.put(channelName, new Object());
            ownedSockets.add(socket);
            return null;
        });
    }

    ZLinkBackendDealerSocket client(String channelName) {
        return inStateLane(() -> clients.get(channelName));
    }

    ZLinkBackendDealerSocket clientForOutbound(String channelName) {
        return inStateLane(() -> clientForOutboundCore(channelName));
    }

    private ZLinkBackendDealerSocket clientForOutboundCore(String channelName) {
        Set<ClientServerConnection> physical =
            Collections.newSetFromMap(new IdentityHashMap<>());
        physical.addAll(clientServerConnections.values());
        List<ClientServerConnection> admitted = physical
            .stream()
            .filter(connection -> connection.ready()
                && connection.descriptor().channelName().equals(channelName)
                && connection.descriptor().weight() > 0
                && connection.descriptor().state()
                    == systems.zlink.framework.runtime.host
                        .ZLinkFrameworkRuntimeState.SERVING)
            .sorted(Comparator.comparing(
                ClientServerConnection::connectionId))
            .toList();
        Map<String, ClientServerConnection> distinct =
            new LinkedHashMap<>();
        for (ClientServerConnection connection : admitted) {
            distinct.putIfAbsent(
                clientServerLogicalIdentity(connection.descriptor()),
                connection);
        }
        List<ClientServerConnection> eligible = distinct.values().stream()
            .sorted(Comparator.comparing(
                connection -> connection.descriptor().serverRid().toHex()))
            .toList();
        long total = 0;
        for (ClientServerConnection connection : eligible) {
            total = Math.addExact(
                total,
                connection.descriptor().weight());
        }
        if (total == 0) {
            return unmanagedBackendClientMode
                ? clients.get(channelName)
                : null;
        }
        Map<String, Long> currentByServer =
            clientServerSelectionCurrents.computeIfAbsent(
                channelName,
                ignored -> new HashMap<>());
        Set<String> eligibleServerIds = eligible.stream()
            .map(connection -> connection.descriptor().serverRid().toHex())
            .collect(Collectors.toSet());
        currentByServer.keySet().removeIf(
            serverId -> !eligibleServerIds.contains(serverId));
        ClientServerConnection selectedConnection = null;
        long selectedCurrent = Long.MIN_VALUE;
        for (ClientServerConnection connection : eligible) {
            String serverId = connection.descriptor().serverRid().toHex();
            long current = Math.addExact(
                currentByServer.getOrDefault(serverId, 0L),
                connection.descriptor().weight());
            currentByServer.put(serverId, current);
            if (selectedConnection == null
                || current > selectedCurrent
                || current == selectedCurrent
                    && serverId.compareTo(
                        selectedConnection.descriptor().serverRid().toHex()) < 0) {
                selectedConnection = connection;
                selectedCurrent = current;
            }
        }
        if (selectedConnection != null) {
            String selectedId = selectedConnection.descriptor().serverRid().toHex();
            currentByServer.put(
                selectedId,
                Math.subtractExact(currentByServer.get(selectedId), total));
            return selectedConnection.dealer();
        }
        throw new IllegalStateException(
            "ClientServer weighted selection did not select a connection");
    }

    /**
     * Waits for a ClientServer target within the caller's remaining budget.
     * Admission callbacks update this registry on the
     * monitor lane, so the wait never holds this registry's monitor.
     */
    ZLinkBackendDealerSocket awaitClientForOutbound(
        String channelName,
        Duration bound) {
        long deadline = System.nanoTime() + bound.toNanos();
        while (true) {
            ZLinkBackendDealerSocket ready = clientForOutbound(channelName);
            if (ready != null) {
                return ready;
            }
            long remaining = deadline - System.nanoTime();
            if (remaining <= 0) {
                return null;
            }
            LockSupport.parkNanos(
                Math.min(
                    TimeUnit.MILLISECONDS.toNanos(
                        READY_POLL_INTERVAL_MILLIS),
                    remaining));
            if (Thread.currentThread().isInterrupted()) {
                Thread.currentThread().interrupt();
                return clientForOutbound(channelName);
            }
        }
    }

    void addClientServerConnection(
        String connectionId,
        ZLinkClientServerServerDescriptor descriptor,
        ZLinkBackendDealerSocket dealer) {
        // This method used to hold the registry monitor while adding the
        // physical DEALER. The absolute flow-state application happens before
        // that monitor is acquired so a binding call cannot block routing.
        registerReceiveFlow(dealer);
        inStateLane(() -> {
            clientServerConnections.put(
                connectionId,
                new ClientServerConnection(
                    connectionId, descriptor, dealer, false));
            ownedSockets.add(dealer);
            return null;
        });
    }

    void registerClientServerMonitor(
        String connectionId,
        ZLinkBackendSocketMonitor monitor) {
        ClientServerConnection current;
        current = inStateLane(() -> {
            ClientServerConnection registered = clientServerConnections.get(connectionId);
            if (registered != null) {
                registered.monitor = monitor;
                ownedSockets.add(monitor);
            }
            return registered;
        });
        if (current == null) {
            monitor.close();
        }
    }

    void enableUnmanagedBackendClientMode() {
        inStateLane(() -> {
            unmanagedBackendClientMode = true;
            return null;
        });
    }

    AdmissionFence clientServerTransportReady(
        String connectionId) {
        return inStateLane(() -> {
            ClientServerConnection current =
                clientServerConnections.get(connectionId);
            return clientServerTransportReadyCore(
                connectionId, current == null ? null : current.dealer);
        });
    }

    AdmissionFence clientServerTransportReady(
        String connectionId,
        ZLinkBackendDealerSocket dealer) {
        return inStateLane(() -> clientServerTransportReadyCore(connectionId, dealer));
    }

    private AdmissionFence clientServerTransportReadyCore(
        String connectionId,
        ZLinkBackendDealerSocket dealer) {
        ClientServerConnection current =
            clientServerConnections.get(connectionId);
        if (current == null || current.dealer != dealer) {
            return null;
        }
        current.physicalGeneration++;
        current.admissionGeneration++;
        current.ready = false;
        current.outstandingProbeId = 0;
        return new AdmissionFence(
            current.physicalGeneration,
            current.admissionGeneration,
            current.dealer);
    }

    void clientServerTransportTerminated(
        String connectionId) {
        inStateLane(() -> {
            ClientServerConnection current =
                clientServerConnections.get(connectionId);
            clientServerTransportTerminatedCore(
                connectionId, current == null ? null : current.dealer);
            return null;
        });
    }

    void clientServerTransportTerminated(
        String connectionId,
        ZLinkBackendDealerSocket dealer) {
        inStateLane(() -> {
            clientServerTransportTerminatedCore(connectionId, dealer);
            return null;
        });
    }

    private void clientServerTransportTerminatedCore(
        String connectionId,
        ZLinkBackendDealerSocket dealer) {
        ClientServerConnection current =
            clientServerConnections.get(connectionId);
        if (current == null || current.dealer != dealer) {
            return;
        }
        current.physicalGeneration++;
        current.admissionGeneration++;
        current.ready = false;
        current.outstandingProbeId = 0;
    }

    void reconnectClientServerConnection(
        String connectionId) {
        reconnectClientServerConnection(connectionId, null);
    }

    void reconnectClientServerConnection(
        String connectionId,
        ZLinkBackendDealerSocket expectedDealer) {
        ClientServerConnection current;
        current = inStateLane(() -> {
            ClientServerConnection registered = clientServerConnections.get(connectionId);
            if (registered != null
                && (expectedDealer == null || registered.dealer == expectedDealer)) {
                registered.ready = false;
                return registered;
            }
            return null;
        });
        if (current != null) {
            reconnectClientServer(current);
        }
    }

    boolean admitClientServerConnection(
        String connectionId,
        ZLinkClientServerServerDescriptor descriptor) {
        AdmissionFence fence = inStateLane(() -> {
            ClientServerConnection current =
                clientServerConnections.get(connectionId);
            return current == null ? null : new AdmissionFence(
                current.physicalGeneration,
                current.admissionGeneration,
                current.dealer);
        });
        return fence != null && admitClientServerConnection(
            connectionId,
            descriptor,
            fence);
    }

    boolean admitClientServerConnection(
        String connectionId,
        ZLinkClientServerServerDescriptor descriptor,
        AdmissionFence fence) {
        AdmissionResult result = inStateLane(() -> {
            ClientServerConnection current =
                clientServerConnections.get(connectionId);
            if (current == null
                || fence == null
                || current.dealer != fence.dealer()
                || current.physicalGeneration != fence.physicalGeneration()
                || current.admissionGeneration != fence.admissionGeneration()) {
                return new AdmissionResult(false, null);
            }
            current.descriptor = descriptor;
            current.ready = true;
            current.nextProbeAtNanos =
                System.nanoTime() + CLIENT_SERVER_PROBE_INTERVAL_NANOS;
            current.deadlineAtNanos =
                System.nanoTime() + CLIENT_SERVER_DEADLINE_NANOS;
            current.outstandingProbeId = 0;
            ClientServerConnection shared =
                clientServerConnections.values().stream()
                    .filter(other -> other != current
                        && other.ready
                        && clientServerLogicalIdentity(other.descriptor)
                            .equals(clientServerLogicalIdentity(descriptor)))
                    .findFirst()
                    .orElse(null);
            if (shared != null) {
                for (String alias : List.copyOf(current.aliases)) {
                    clientServerConnections.put(alias, shared);
                    shared.aliases.add(alias);
                }
                current.aliases.clear();
                return new AdmissionResult(true, current);
            }
            return new AdmissionResult(true, null);
        });
        if (result.closeAfter() != null) {
            closeClientServerPhysical(result.closeAfter());
        }
        return result.admitted();
    }

    void updateClientServerConnection(
        String connectionId,
        ZLinkClientServerServerDescriptor descriptor,
        boolean ready) {
        inStateLane(() -> {
            ClientServerConnection current =
                clientServerConnections.get(connectionId);
            if (current != null) {
                current.descriptor = descriptor;
                current.ready = ready;
            }
            return null;
        });
    }

    ZLinkClientServerServerDescriptor
        clientServerConnectionDescriptor(String connectionId) {
        return inStateLane(() -> {
            ClientServerConnection current =
                clientServerConnections.get(connectionId);
            return current == null ? null : current.descriptor;
        });
    }

    boolean ownsClientServerPhysical(
        String connectionId,
        ZLinkBackendDealerSocket dealer) {
        return inStateLane(() -> {
            ClientServerConnection current =
                clientServerConnections.get(connectionId);
            return current != null && current.dealer == dealer;
        });
    }

    void removeClientServerConnection(String connectionId) {
        removeClientServerConnection(connectionId, null);
    }

    void removeClientServerConnection(
        String connectionId,
        ZLinkBackendDealerSocket expectedDealer) {
        Removal removal = inStateLane(() -> {
            ClientServerConnection registered = clientServerConnections.get(connectionId);
            if (registered == null
                || (expectedDealer != null && registered.dealer != expectedDealer)) {
                return null;
            }
            ClientServerConnection removed = clientServerConnections.remove(connectionId);
            if (removed != null) {
                removed.aliases.remove(connectionId);
            }
            return removed == null ? null : new Removal(
                removed, removed.aliases.isEmpty());
        });
        if (removal == null) {
            return;
        }
        if (removal.closePhysical()) {
            closeClientServerPhysical(removal.connection());
        }
    }

    int clientServerPhysicalConnectionCount() {
        return inStateLane(() -> {
            Set<ClientServerConnection> physical =
                Collections.newSetFromMap(new IdentityHashMap<>());
            physical.addAll(clientServerConnections.values());
            return physical.size();
        });
    }

    List<ClientServerTargetSnapshot>
        clientServerTargetSnapshots(String channelName) {
        return inStateLane(() -> clientServerTargetSnapshotsCore(channelName));
    }

    private List<ClientServerTargetSnapshot> clientServerTargetSnapshotsCore(
        String channelName) {
        Map<String, ClientServerTargetSnapshot> targets =
            new LinkedHashMap<>();
        Set<ClientServerConnection> physical =
            Collections.newSetFromMap(new IdentityHashMap<>());
        physical.addAll(clientServerConnections.values());
        for (ClientServerConnection connection : physical) {
            ZLinkClientServerServerDescriptor descriptor =
                connection.descriptor();
            if (!descriptor.channelName().equals(channelName)) {
                continue;
            }
            String key = descriptor.serverRid().toHex()
                + '\0' + descriptor.lifecycleGeneration();
            targets.put(
                key,
                new ClientServerTargetSnapshot(
                    descriptor.serverRid(),
                    descriptor.weight(),
                    connection.ready(),
                    connection.ready()
                        && descriptor.state()
                            == systems.zlink.framework.runtime.host
                                .ZLinkFrameworkRuntimeState.SERVING));
        }
        return List.copyOf(targets.values());
    }

    boolean hasClientRegistration(String channelName) {
        return inStateLane(() -> {
            ChannelRegistration registration = registrations.get(channelName);
            return registration != null
                && registration.kind() == ChannelKind.CLIENT_SERVER
                && registration.clientEnabled();
        });
    }

    boolean hasServerRegistration(String channelName) {
        return inStateLane(() -> {
            ChannelRegistration registration = registrations.get(channelName);
            return registration != null
                && registration.kind() == ChannelKind.CLIENT_SERVER
                && registration.clientServerServerEnabled();
        });
    }

    void setClientServerServerDescriptor(
        String channelName,
        ZLinkClientServerServerDescriptor descriptor) {
        inStateLane(() -> {
            if (descriptor == null) {
                clientServerServerDescriptors.remove(channelName);
            } else {
                clientServerServerDescriptors.put(channelName, descriptor);
            }
            return null;
        });
        if (descriptor != null) {
            pushClientServerDescriptorUpdate(channelName, descriptor);
        }
    }

    ZLinkClientServerServerDescriptor
        clientServerServerDescriptor(String channelName) {
        return inStateLane(() -> clientServerServerDescriptors.get(channelName));
    }

    int clientServerServerWeight(
        String channelName,
        int fallback) {
        ZLinkBackendRouterSocket server =
            inStateLane(() -> servers.get(channelName));
        return server == null ? fallback : server.peerWeight();
    }

    void initializeClientServerServerDescriptors(String ownerId) {
        List<ServerDescriptorInput> inputs = inStateLane(() ->
            servers.entrySet().stream()
                .map(entry -> new ServerDescriptorInput(
                    entry.getKey(), entry.getValue(),
                    serverRoutingIds.get(entry.getKey()),
                    registrations.get(entry.getKey())))
                .toList());
        List<ServerDescriptorValue> descriptors = new ArrayList<>();
        for (ServerDescriptorInput input : inputs) {
            ChannelRegistration registration = input.registration();
            if (registration == null
                || registration.serverBinds().isEmpty()) {
                continue;
            }
            String endpoint = advertisedEndpoint(
                registration.serverBinds().get(0),
                input.router(),
                registration.clientServerAdvertiseHost());
            descriptors.add(new ServerDescriptorValue(
                input.channelName(),
                new ZLinkClientServerServerDescriptor(
                    input.channelName(),
                    input.routingId(),
                    ThreadLocalRandom.current()
                        .nextLong(1, Long.MAX_VALUE),
                    1,
                    endpoint,
                    input.router().peerWeight(),
                    systems.zlink.framework.runtime.host
                        .ZLinkFrameworkRuntimeState.SERVING,
                    "default",
                    ownerId,
                    1,
                    Instant.EPOCH)));
        }
        inStateLane(() -> {
            for (ServerDescriptorValue descriptor : descriptors) {
                clientServerServerDescriptors.put(
                    descriptor.channelName(), descriptor.descriptor());
            }
            return null;
        });
    }

    boolean tryHandleClientServerControl(
        String channelName,
        ZLinkBackendRouterSocket router,
        ZLinkBackendReceived received) {
        if (received.parts().isEmpty()) {
            return false;
        }
        byte[] frame = received.parts().get(0).toByteArray();
        if (!ZLinkClientServerServiceWire.isControlFrame(frame)) {
            return false;
        }
        byte[] reply = null;
        try {
            ZLinkClientServerServiceWire.Control control =
                ZLinkClientServerServiceWire.decode(frame);
            if (control instanceof ZLinkClientServerServiceWire.LivenessAck ack
                && received.routingId().isPresent()
                && !received.isRequest()) {
                acceptClientServerServerAck(
                    channelName, received.routingId().get(), ack.probeId());
                received.close();
                return true;
            }
            if (control
                    instanceof ZLinkClientServerServiceWire.LivenessProbe probe
                && received.routingId().isPresent()) {
                byte[] ack = ZLinkClientServerServiceWire.encodeLivenessAck(
                    probe.probeId());
                if (received.isRequest()) {
                    reply = ack;
                } else {
                    try (Message message = Message.from(ack)) {
                        router.send(
                            received.routingId().get(),
                            List.of(message));
                    }
                    received.close();
                    return true;
                }
            } else {
            ZLinkClientServerServerDescriptor descriptor = inStateLane(
                () -> clientServerServerDescriptors.get(channelName));
            if (!(control instanceof ZLinkClientServerServiceWire.Hello hello)
                || descriptor == null
                || !hello.channelName().equals(channelName)
                || !hello.securityIdentity().equals(
                    descriptor.securityIdentity())) {
                reply = ZLinkClientServerServiceWire.encodeReject(2);
            } else {
                reply = ZLinkClientServerServiceWire.encodeAdmit(
                    descriptor,
                    normalizedMessageLimit(router.maxMessageSize()));
                if (received.routingId().isPresent()) {
                    admitClientServerServerPeer(
                        channelName,
                        received.routingId().get(),
                        router);
                }
            }
            }
        } catch (RuntimeException failure) {
            if (received.isRequest()) {
                reply = ZLinkClientServerServiceWire.encodeReject(1);
            }
        }
        if (reply != null && received.isRequest()) {
            ZLinkChannelDispatchReporter.replyAndClose(
                router, received, Message.from(reply));
        } else if (reply != null && received.routingId().isPresent()) {
            try {
                router.disconnectPeer(received.routingId().get());
            } catch (RuntimeException ignored) {
            }
        }
        received.close();
        return true;
    }

    void tickClientServerLiveness(long nowNanos) {
        LivenessSnapshot snapshot = inStateLane(() -> {
            Set<ClientServerConnection> physical =
                Collections.newSetFromMap(new IdentityHashMap<>());
            physical.addAll(clientServerConnections.values());
            return new LivenessSnapshot(
                new ArrayList<>(physical),
                List.copyOf(clientServerServerPeers.values()),
                clientServerControlCursor);
        });
        List<ClientServerConnection> clientConnections = snapshot.clientConnections();
        List<ClientServerServerPeer> serverPeers = snapshot.serverPeers();
        clientConnections.sort(Comparator.comparing(
            ClientServerConnection::connectionId));
        int connectionCount = clientConnections.size();
        int connectionStart = connectionCount == 0
            ? 0
            : (int) Math.floorMod(
                snapshot.controlCursor(), connectionCount);
        for (int offset = 0; offset < connectionCount; offset++) {
            ClientServerConnection connection = clientConnections.get(
                (connectionStart + offset) % connectionCount);
            int nextCursor = (connectionStart + offset + 1) % connectionCount;
            inStateLane(() -> {
                clientServerControlCursor = nextCursor;
                return null;
            });
            drainClientServerControls(connection);
            flushClientServerLivenessAck(connection);
            ClientLivenessAction action = inStateLane(() -> {
                if (!connection.aliases.stream().anyMatch(
                        alias -> clientServerConnections.get(alias)
                            == connection)
                    || !connection.ready) {
                    return ClientLivenessAction.NONE;
                }
                if (nowNanos >= connection.deadlineAtNanos) {
                    connection.ready = false;
                    return ClientLivenessAction.RECONNECT;
                } else if (nowNanos >= connection.nextProbeAtNanos) {
                    connection.nextProbeAtNanos =
                        nowNanos + CLIENT_SERVER_PROBE_INTERVAL_NANOS;
                    long probeId = connection.outstandingProbeId == 0
                        ? allocateProbeIdCore()
                        : connection.outstandingProbeId;
                    connection.outstandingProbeId = probeId;
                    return new ClientLivenessAction(false, probeId);
                }
                return ClientLivenessAction.NONE;
            });
            if (action.reconnect()) {
                reconnectClientServer(connection);
                continue;
            }
            if (action.probeId() == 0) {
                continue;
            }
            sendClientServerProbe(connection, action.probeId());
        }
        for (ClientServerServerPeer peer : serverPeers) {
            ServerPeerLivenessAction action = inStateLane(() -> {
                ClientServerServerPeer current =
                    clientServerServerPeers.get(peer.key);
                if (current != peer) {
                    return ServerPeerLivenessAction.NONE;
                }
                if (nowNanos >= current.deadlineAtNanos) {
                    clientServerServerPeers.remove(peer.key);
                    return ServerPeerLivenessAction.DISCONNECT;
                } else if (nowNanos >= current.nextProbeAtNanos) {
                    current.nextProbeAtNanos =
                        nowNanos + CLIENT_SERVER_PROBE_INTERVAL_NANOS;
                    long probeId = current.outstandingProbeId == 0
                        ? allocateProbeIdCore()
                        : current.outstandingProbeId;
                    current.outstandingProbeId = probeId;
                    return new ServerPeerLivenessAction(false, probeId);
                } else {
                    return ServerPeerLivenessAction.NONE;
                }
            });
            if (action.disconnect()) {
                try {
                    peer.router.disconnectPeer(peer.routingId);
                } catch (RuntimeException ignored) {
                }
                continue;
            }
            if (action.probeId() == 0) {
                continue;
            }
            try (Message message = Message.from(
                ZLinkClientServerServiceWire.encodeLivenessProbe(action.probeId()))) {
                try {
                    peer.router.send(
                        peer.routingId,
                        List.of(message));
                } catch (ZlinkSubmitException ignored) {
                    // The peer may lose admission between the liveness
                    // schedule check and the non-blocking send. Its existing
                    // deadline and connection lifecycle perform cleanup.
                }
            }
        }
    }

    private void drainClientServerControls(
        ClientServerConnection connection) {
        ZLinkReceiveBatchBudget batch = new ZLinkReceiveBatchBudget();
        while (batch.canReceiveNext()) {
            if (!connection.dealer.waitForReadable(Duration.ZERO)) {
                return;
            }
            ZLinkBackendReceived received =
                connection.dealer.recv(ZLinkBackendRecvMode.DONT_WAIT);
            if (received == null) {
                return;
            }
            batch.record(ZLinkReceiveBatchBudget.bytesOf(received.parts()));
            try (received) {
                if (received.parts().size() != 1) {
                    terminateClientServerProtocol(connection);
                    continue;
                }
                ZLinkClientServerServiceWire.Control control =
                    ZLinkClientServerServiceWire.decode(
                        received.parts().get(0).toByteArray());
                if (control
                        instanceof ZLinkClientServerServiceWire.LivenessProbe probe) {
                    inStateLane(() -> {
                        connection.pendingLivenessAckId = probe.probeId();
                        return null;
                    });
                    try (Message ack = Message.from(
                        ZLinkClientServerServiceWire.encodeLivenessAck(
                            probe.probeId()))) {
                        try {
                            connection.dealer.send(List.of(ack))
                                .whenComplete((ignored, failure) -> {
                                    if (failure == null) {
                                        inStateLane(() -> {
                                            connection.pendingLivenessAckId = 0;
                                            return null;
                                        });
                                    }
                                });
                        } catch (ZlinkSubmitException ignored) {
                            // A DEALER may reject an unsolicited control
                            // reply while its accepted application request is
                            // still awaiting the matching completion. Keep
                            // the physical connection and let the next
                            // liveness probe retry after the application
                            // completion; terminating here would violate the
                            // in-flight request completion contract.
                        }
                    }
                } else if (control
                        instanceof ZLinkClientServerServiceWire.Update update) {
                    applyClientServerUpdate(connection, update.admission());
                } else if (control
                        instanceof ZLinkClientServerServiceWire.LivenessAck ack) {
                    acceptClientServerClientAck(connection, ack.probeId());
                } else {
                    terminateClientServerProtocol(connection);
                }
            } catch (RuntimeException failure) {
                terminateClientServerProtocol(connection);
            }
        }
    }

    private void flushClientServerLivenessAck(
        ClientServerConnection connection) {
        long probeId = inStateLane(() -> connection.pendingLivenessAckId);
        if (probeId == 0) {
            return;
        }
        try (Message ack = Message.from(
            ZLinkClientServerServiceWire.encodeLivenessAck(probeId))) {
            connection.dealer.send(List.of(ack))
                .whenComplete((ignored, failure) -> {
                    if (failure == null) {
                        inStateLane(() -> {
                            connection.pendingLivenessAckId = 0;
                            return null;
                        });
                    }
                });
        } catch (ZlinkSubmitException ignored) {
        }
    }

    private void sendClientServerProbe(
        ClientServerConnection connection,
        long probeId) {
        try (Message message = Message.from(
            ZLinkClientServerServiceWire.encodeLivenessProbe(probeId))) {
            connection.dealer.send(List.of(message));
        } catch (ZlinkSubmitException ignored) {
            // The same outstanding probe ID is retried on the next interval.
            // A transient send-admission race is not a connection result.
        }
    }

    private void acceptClientServerClientAck(
        ClientServerConnection connection,
        long probeId) {
        inStateLane(() -> {
            if (!connection.aliases.stream().anyMatch(
                    alias -> clientServerConnections.get(alias) == connection)
                || connection.outstandingProbeId != probeId) {
                return null;
            }
            connection.outstandingProbeId = 0;
            connection.deadlineAtNanos =
                System.nanoTime() + CLIENT_SERVER_DEADLINE_NANOS;
            return null;
        });
    }

    private void applyClientServerUpdate(
        ClientServerConnection connection,
        ZLinkClientServerServiceWire.Admission update) {
        boolean reconnect = inStateLane(() -> {
            if (!connection.aliases.stream().anyMatch(
                    alias -> clientServerConnections.get(alias) == connection)
                || !connection.ready) {
                return false;
            }
            ZLinkClientServerServerDescriptor before = connection.descriptor;
            if (!update.channelName().equals(before.channelName())
                || !update.serverRid().equals(before.serverRid())
                || update.lifecycleGeneration()
                    != before.lifecycleGeneration()
                || !update.securityIdentity().equals(
                    before.securityIdentity())
                || !update.advertisedEndpoint().equals(before.endpoint())) {
                connection.ready = false;
                return true;
            } else if (update.descriptorRevision()
                >= before.descriptorRevision()) {
                ZLinkClientServerServerDescriptor candidate =
                    descriptorFromAdmission(update, before);
                if (update.descriptorRevision()
                    == before.descriptorRevision()) {
                    if (!sameClientServerDescriptor(candidate, before)) {
                        connection.ready = false;
                        return true;
                    }
                } else {
                    connection.descriptor = candidate;
                }
            }
            return false;
        });
        if (reconnect) {
            reconnectClientServer(connection);
        }
    }

    private void terminateClientServerProtocol(
        ClientServerConnection connection) {
        boolean active = inStateLane(() -> {
            if (!connection.aliases.stream().anyMatch(
                    alias -> clientServerConnections.get(alias) == connection)) {
                return false;
            }
            connection.ready = false;
            return true;
        });
        if (active) {
            reconnectClientServer(connection);
        }
    }

    // Transport liveness spec 55 section 6: terminal cleanup never leaves a
    // monitor subscription behind its connection, so the monitor closes before
    // the DEALER it observes.
    private void closeClientServerPhysical(
        ClientServerConnection connection) {
        ZLinkBackendSocketMonitor monitor;
        ZLinkApplicationJobReceiveFlowController.Registration receiveFlow;
        ClientServerClose close = inStateLane(() -> {
            if (connection.physicalClosed) {
                return null;
            }
            connection.physicalClosed = true;
            ZLinkBackendSocketMonitor registeredMonitor = connection.monitor;
            connection.monitor = null;
            if (registeredMonitor != null) {
                ownedSockets.removeIf(candidate -> candidate == registeredMonitor);
            }
            ownedSockets.removeIf(candidate -> candidate == connection.dealer);
            return new ClientServerClose(
                registeredMonitor,
                receiveFlowRegistrations.remove(connection.dealer));
        });
        if (close == null) {
            return;
        }
        monitor = close.monitor();
        receiveFlow = close.receiveFlow();
        closeReceiveFlowRegistration(receiveFlow);
        synchronized (connection.transportLock) {
            if (monitor != null) {
                try {
                    monitor.close();
                } catch (RuntimeException ignored) {
                }
            }
            try {
                connection.dealer.close();
            } catch (RuntimeException ignored) {
            }
        }
    }

    private void reconnectClientServer(ClientServerConnection connection) {
        String endpoint = inStateLane(() -> {
            if (!connection.aliases.stream().anyMatch(
                    alias -> clientServerConnections.get(alias) == connection)) {
                return null;
            }
            connection.physicalGeneration++;
            connection.admissionGeneration++;
            connection.outstandingProbeId = 0;
            return connection.descriptor.endpoint();
        });
        if (endpoint == null) {
            return;
        }
        synchronized (connection.transportLock) {
            try {
                connection.dealer.disconnect(endpoint);
                connection.dealer.connect(endpoint);
            } catch (RuntimeException ignored) {
            }
        }
    }

    private void admitClientServerServerPeer(
        String channelName,
        RoutingId routingId,
        ZLinkBackendRouterSocket router) {
        inStateLane(() -> {
            String key = serverPeerKey(channelName, routingId);
            long now = System.nanoTime();
            clientServerServerPeers.put(
                key,
                new ClientServerServerPeer(
                    key,
                    channelName,
                    routingId,
                    router,
                    now + CLIENT_SERVER_PROBE_INTERVAL_NANOS,
                    now + CLIENT_SERVER_DEADLINE_NANOS));
            return null;
        });
    }

    private void acceptClientServerServerAck(
        String channelName,
        RoutingId routingId,
        long probeId) {
        inStateLane(() -> {
            ClientServerServerPeer peer =
                clientServerServerPeers.get(
                    serverPeerKey(channelName, routingId));
            if (peer == null || peer.outstandingProbeId != probeId) {
                return null;
            }
            peer.outstandingProbeId = 0;
            peer.deadlineAtNanos =
                System.nanoTime() + CLIENT_SERVER_DEADLINE_NANOS;
            return null;
        });
    }

    private void pushClientServerDescriptorUpdate(
        String channelName,
        ZLinkClientServerServerDescriptor descriptor) {
        List<ClientServerServerPeer> peers;
        peers = inStateLane(
            () -> List.copyOf(clientServerServerPeers.values()));
        for (ClientServerServerPeer peer : peers) {
            if (!peer.channelName.equals(channelName)) {
                continue;
            }
            try (Message message = Message.from(
                ZLinkClientServerServiceWire.encodeUpdate(
                    descriptor,
                    normalizedMessageLimit(
                        peer.router.maxMessageSize())))) {
                try {
                    peer.router.send(
                        peer.routingId,
                        List.of(message));
                } catch (ZlinkSubmitException ignored) {
                    // The Location Store update remains authoritative. A peer
                    // that has already lost admission cannot receive this
                    // best-effort descriptor projection and is removed by the
                    // normal connection lifecycle.
                }
            }
        }
    }

    private long allocateProbeIdCore() {
        long result = nextClientServerProbeId;
        nextClientServerProbeId =
            result == Long.MAX_VALUE ? 1 : result + 1;
        return result;
    }

    ZLinkBackendRouterSocket server(String channelName) {
        return inStateLane(() -> servers.get(channelName));
    }

    ZLinkBackendPublisherSocket publisher(String channelName) {
        return inStateLane(() -> publishers.get(channelName));
    }

    String listenerEndpoint(
        ZLinkListenerKind kind,
        String channelName) {
        return switch (kind) {
            case CLIENT_SERVER -> {
                RouterListener listener = inStateLane(() -> {
                    ChannelRegistration registration = registrations.get(channelName);
                    if (registration == null) {
                        throw new ZLinkConfigurationException(
                            "channel is not configured: " + channelName);
                    }
                    if (registration.kind() != ChannelKind.CLIENT_SERVER
                        || !registration.clientServerServerEnabled()) {
                        throw new ZLinkConfigurationException(
                            "ClientServer server is not configured: " + channelName);
                    }
                    ZLinkBackendRouterSocket router = servers.get(channelName);
                    if (router == null || registration.serverBinds().isEmpty()) {
                        throw new ZLinkConfigurationException(
                            "ClientServer listener is not started: " + channelName);
                    }
                    return new RouterListener(
                        registration.serverBinds().getFirst(), router,
                        registration.clientServerAdvertiseHost());
                });
                yield advertisedEndpoint(
                    listener.endpoint(), listener.router(), listener.advertiseHost());
            }
            case FANOUT -> {
                PublisherListener listener = inStateLane(() -> {
                    ChannelRegistration registration = registrations.get(channelName);
                    if (registration == null) {
                        throw new ZLinkConfigurationException(
                            "channel is not configured: " + channelName);
                    }
                    if (registration.kind() != ChannelKind.FANOUT
                        || !registration.publisherEnabled()) {
                        throw new ZLinkConfigurationException(
                            "fanout publisher is not configured: " + channelName);
                    }
                    ZLinkBackendPublisherSocket publisher = publishers.get(channelName);
                    if (publisher == null || registration.publisherBinds().isEmpty()) {
                        throw new ZLinkConfigurationException(
                            "fanout listener is not started: " + channelName);
                    }
                    return new PublisherListener(
                        registration.publisherBinds().getFirst(), publisher,
                        registration.fanoutAdvertiseHost());
                });
                yield advertisedEndpoint(
                    listener.endpoint(), listener.publisher(), listener.advertiseHost());
            }
            default -> throw new ZLinkConfigurationException(
                "listener kind is not a Channel listener: " + kind);
        };
    }

    ZLinkBackendSubscriberSocket subscriber(String channelName) {
        return inStateLane(() -> subscribers.get(channelName));
    }

    ZLinkBackendRouterSocket routeRouter(String channelName) {
        return inStateLane(() -> routeRouters.get(channelName));
    }

    Object routeSocketLock(String channelName, Object fallback) {
        return inStateLane(
            () -> routeSocketLocks.getOrDefault(channelName, fallback));
    }

    Map<String, ZLinkBackendSpotRouteBridge> spotRouteBridges() {
        return spotRouteBridges;
    }

    ZLinkBackendSpotRouteBridge spotRouteBridge(String channelName) {
        return spotRouteBridges.get(channelName);
    }

    void registerSpotRouteBridge(String channelName, ZLinkBackendSpotRouteBridge bridge) {
        spotRouteBridges.put(channelName, bridge);
    }

    List<String> spotRouteBridgeChannelNames() {
        return List.copyOf(spotRouteBridges.keySet());
    }

    void registerSpotRouterNode(String channelName, ZLinkInternalSpotNode node) {
        inStateLane(() -> {
            spotRouterNodes.put(channelName, node);
            return null;
        });
    }

    ZLinkInternalSpotNode spotRouterNode(String channelName) {
        return inStateLane(() -> spotRouterNodes.get(channelName));
    }

    Map<String, ZLinkBackendSocket> monitoringSocketSources() {
        return inStateLane(() -> {
            Map<String, ZLinkBackendSocket> sources = new HashMap<>();
            sources.putAll(clients);
            sources.putAll(servers);
            sources.putAll(publishers);
            sources.putAll(subscribers);
            sources.putAll(routeRouters);
            return Map.copyOf(sources);
        });
    }

    List<ZLinkChannelRuntime.AutoConnectSurface> autoConnectSurfaces() {
        AutoConnectSnapshot snapshot = inStateLane(() -> new AutoConnectSnapshot(
            List.copyOf(registrations.values()),
            Map.copyOf(clients),
            Map.copyOf(servers),
            Map.copyOf(serverRoutingIds),
            Map.copyOf(publishers),
            Map.copyOf(publisherRoutingIds),
            Map.copyOf(routeRouters)));
        List<ZLinkChannelRuntime.AutoConnectSurface> surfaces = new ArrayList<>();
        for (ChannelRegistration channel : snapshot.registrations()) {
            addAutoConnectSurfaces(channel, surfaces, snapshot);
        }
        return List.copyOf(surfaces);
    }

    void closeSpotRouteBridges() {
        List<ZLinkBackendSpotRouteBridge> bridges = List.copyOf(
            spotRouteBridges.values());
        spotRouteBridges.clear();
        closeAll(bridges, Collections.newSetFromMap(
            new IdentityHashMap<>()));
    }

    void closeAll() {
        List<ChannelRegistration> registrationsToDetach = inStateLane(
            () -> List.copyOf(registrations.values()));
        for (ChannelRegistration registration : registrationsToDetach) {
            registration.detachRuntimeConnections();
        }
        // ClientServer physical connections close first so that spec 55
        // section 6 holds here too: the admission monitor never outlives the
        // DEALER it observes. Draining ownedSockets in insertion order would
        // close the DEALER before its monitor.
        Set<ClientServerConnection> physical =
            Collections.newSetFromMap(new IdentityHashMap<>());
        physical.addAll(inStateLane(() -> {
            Set<ClientServerConnection> connections =
                Collections.newSetFromMap(new IdentityHashMap<>());
            connections.addAll(clientServerConnections.values());
            clientServerConnections.clear();
            clientServerServerDescriptors.clear();
            clientServerServerPeers.clear();
            return connections;
        }));
        for (ClientServerConnection connection : physical) {
            closeClientServerPhysical(connection);
        }
        List<ZLinkBackendObject> owned = inStateLane(() -> {
            // closeClientServerPhysical removes its monitor and DEALER from
            // the owned set. Snapshot only after that ordered close so the
            // generic drain cannot close either physical object a second time.
            List<ZLinkBackendObject> snapshot = List.copyOf(ownedSockets);
            ownedSockets.clear();
            return snapshot;
        });
        owned.forEach(this::deregisterReceiveFlow);
        closeAll(owned, Collections.newSetFromMap(
            new IdentityHashMap<>()));
    }

    private void registerReceiveFlow(ZLinkBackendDealerSocket socket) {
        if (applicationJobQueue == null) {
            return;
        }
        ZLinkApplicationJobReceiveFlowController.Registration registration =
            applicationJobQueue.registerReceiveFlowTarget(
                socket::setReceiveFlowState);
        ZLinkApplicationJobReceiveFlowController.Registration previous;
        previous = inStateLane(
            () -> receiveFlowRegistrations.put(socket, registration));
        closeReceiveFlowRegistration(previous);
    }

    private void registerReceiveFlow(ZLinkBackendRouterSocket socket) {
        if (applicationJobQueue == null) {
            return;
        }
        ZLinkApplicationJobReceiveFlowController.Registration registration =
            applicationJobQueue.registerReceiveFlowTarget(
                socket::setReceiveFlowState);
        ZLinkApplicationJobReceiveFlowController.Registration previous;
        previous = inStateLane(
            () -> receiveFlowRegistrations.put(socket, registration));
        closeReceiveFlowRegistration(previous);
    }

    private void deregisterReceiveFlow(ZLinkBackendObject socket) {
        ZLinkApplicationJobReceiveFlowController.Registration registration;
        registration = inStateLane(
            () -> receiveFlowRegistrations.remove(socket));
        closeReceiveFlowRegistration(registration);
    }

    private static void closeReceiveFlowRegistration(
        ZLinkApplicationJobReceiveFlowController.Registration registration) {
        if (registration != null) {
            registration.close();
        }
    }

    private void addAutoConnectSurfaces(
        ChannelRegistration channel,
        List<ZLinkChannelRuntime.AutoConnectSurface> surfaces,
        AutoConnectSnapshot snapshot) {
        if (channel.kind() == ChannelKind.CLIENT_SERVER) {
            addClientServerSurfaces(channel, surfaces, snapshot);
        } else if (channel.kind() == ChannelKind.FANOUT) {
            addFanoutSurfaces(channel, surfaces, snapshot);
        } else if (channel.kind() == ChannelKind.ROUTE_MESH) {
            addRouteSurfaces(channel, surfaces, snapshot);
        }
    }

    private void addClientServerSurfaces(
        ChannelRegistration channel,
        List<ZLinkChannelRuntime.AutoConnectSurface> surfaces,
        AutoConnectSnapshot snapshot) {
        ZLinkBackendRouterSocket server = snapshot.servers().get(channel.name());
        if (server != null) {
            for (String endpoint : channel.serverBinds()) {
                surfaces.add(new ZLinkChannelRuntime.AutoConnectSurface(
                    ZLinkAutoConnectType.CLIENT_SERVER,
                    channel.name(),
                    ZLinkLocationRole.ROUTER,
                    snapshot.serverRoutingIds().get(channel.name()),
                    advertisedEndpoint(
                        endpoint,
                        server,
                        channel.clientServerAdvertiseHost()),
                    server.peerWeight(),
                    null,
                    List.of()));
            }
        }
        if (channel.clientEnabled()) {
            surfaces.add(new ZLinkChannelRuntime.AutoConnectSurface(
                ZLinkAutoConnectType.CLIENT_SERVER,
                channel.name(),
                ZLinkLocationRole.DEALER,
                channel.routingId(),
                "",
                100,
                snapshot.clients().get(channel.name()),
                channel.clientManualEndpoints()));
        }
    }

    private void addFanoutSurfaces(
        ChannelRegistration channel,
        List<ZLinkChannelRuntime.AutoConnectSurface> surfaces,
        AutoConnectSnapshot snapshot) {
        if (snapshot.publishers().containsKey(channel.name())) {
            for (String endpoint : channel.publisherBinds()) {
                surfaces.add(new ZLinkChannelRuntime.AutoConnectSurface(
                    ZLinkAutoConnectType.FANOUT,
                    channel.name(),
                    ZLinkLocationRole.PUB,
                    snapshot.publisherRoutingIds().get(channel.name()),
                    advertisedEndpoint(
                        endpoint,
                        snapshot.publishers().get(channel.name()),
                        channel.fanoutAdvertiseHost()),
                    100,
                    null,
                    List.of()));
            }
        }
        if (channel.automaticSubscriberEnabled()) {
            surfaces.add(new ZLinkChannelRuntime.AutoConnectSurface(
                ZLinkAutoConnectType.FANOUT,
                channel.name(),
                ZLinkLocationRole.SUB,
                channel.routingId(),
                "",
                100,
                null,
                channel.subscriberManualEndpoints()));
        }
    }

    private void addRouteSurfaces(
        ChannelRegistration channel,
        List<ZLinkChannelRuntime.AutoConnectSurface> surfaces,
        AutoConnectSnapshot snapshot) {
        ZLinkBackendRouterSocket router = snapshot.routeRouters().get(channel.name());
        if (router == null) {
            return;
        }
        for (String endpoint : channel.routeBinds()) {
            surfaces.add(new ZLinkChannelRuntime.AutoConnectSurface(
                ZLinkAutoConnectType.ROUTE_MESH,
                channel.name(),
                ZLinkLocationRole.ROUTER,
                channel.routeRoutingId(),
                advertisedEndpoint(endpoint, router),
                router.peerWeight(),
                router,
                channel.routeManualEndpoints()));
        }
        if (channel.routeBinds().isEmpty()) {
            surfaces.add(new ZLinkChannelRuntime.AutoConnectSurface(
                ZLinkAutoConnectType.ROUTE_MESH,
                channel.name(),
                ZLinkLocationRole.ROUTER,
                channel.routeRoutingId(),
                "",
                router.peerWeight(),
                router,
                channel.routeManualEndpoints()));
        }
    }

    private static String advertisedEndpoint(
        String configuredEndpoint,
        ZLinkBackendRouterSocket router) {
        return advertisedEndpoint(configuredEndpoint, router, null);
    }

    private static String advertisedEndpoint(
        String configuredEndpoint,
        ZLinkBackendRouterSocket router,
        String advertiseHost) {
        String endpoint = configuredEndpoint;
        if (!configuredEndpoint.endsWith(":0")) {
            endpoint = configuredEndpoint;
        } else {
            String boundEndpoint = router.lastEndpoint();
            endpoint = boundEndpoint == null || boundEndpoint.isBlank()
                ? configuredEndpoint
                : boundEndpoint;
        }
        //  Write-time normalization (endpoint notation policy §2.3): built
        //  with an IPv6-safe host substitution (never lastIndexOf(':')),
        //  then normalized once here rather than at every comparison site.
        //  The former java.net.URI 7-arg reconstruction threw for
        //  otherwise-legal hosts (e.g. underscore-bearing Docker service
        //  names) it treated as illegal reg-names.
        if (advertiseHost != null && !advertiseHost.isBlank()) {
            endpoint = ZLinkEndpointNotation.withHost(endpoint, advertiseHost);
        }
        return ZLinkEndpointNotation.normalize(endpoint);
    }

    private static String advertisedEndpoint(
        String configuredEndpoint,
        ZLinkBackendPublisherSocket publisher) {
        return advertisedEndpoint(configuredEndpoint, publisher, null);
    }

    private static String advertisedEndpoint(
        String configuredEndpoint,
        ZLinkBackendPublisherSocket publisher,
        String advertiseHost) {
        String endpoint;
        if (!configuredEndpoint.endsWith(":0")) {
            endpoint = configuredEndpoint;
        } else {
            String boundEndpoint = publisher.lastEndpoint();
            endpoint = boundEndpoint == null || boundEndpoint.isBlank()
                ? configuredEndpoint
                : boundEndpoint;
        }
        //  Write-time normalization (endpoint notation policy §2.3): see
        //  the ClientServer advertisedEndpoint overload above.
        if (advertiseHost != null && !advertiseHost.isBlank()) {
            endpoint = ZLinkEndpointNotation.withHost(endpoint, advertiseHost);
        }
        return ZLinkEndpointNotation.normalize(endpoint);
    }

    private static void closeAll(
        Iterable<? extends ZLinkBackendObject> closeables,
        Set<ZLinkBackendObject> closed) {
        for (ZLinkBackendObject closeable : closeables) {
            if (closeable != null && closed.add(closeable)) {
                try {
                    closeable.close();
                } catch (ZlinkCloseException ignored) {
                }
            }
        }
    }

    private static int normalizedMessageLimit(long configured) {
        return configured > 0 && configured <= Integer.MAX_VALUE
            ? (int) configured
            : Integer.MAX_VALUE;
    }

    record AdmissionFence(
        long physicalGeneration,
        long admissionGeneration,
        ZLinkBackendDealerSocket dealer) {
    }

    record ClientServerTargetSnapshot(
        RoutingId nodeRid,
        int weight,
        boolean connectionReady,
        boolean ready) {
    }

    private record AdmissionResult(
        boolean admitted,
        ClientServerConnection closeAfter) {
    }

    private record Removal(
        ClientServerConnection connection,
        boolean closePhysical) {
    }

    private record LivenessSnapshot(
        List<ClientServerConnection> clientConnections,
        List<ClientServerServerPeer> serverPeers,
        long controlCursor) {
    }

    private record ClientLivenessAction(boolean reconnect, long probeId) {
        private static final ClientLivenessAction NONE =
            new ClientLivenessAction(false, 0);
        private static final ClientLivenessAction RECONNECT =
            new ClientLivenessAction(true, 0);
    }

    private record ServerPeerLivenessAction(boolean disconnect, long probeId) {
        private static final ServerPeerLivenessAction NONE =
            new ServerPeerLivenessAction(false, 0);
        private static final ServerPeerLivenessAction DISCONNECT =
            new ServerPeerLivenessAction(true, 0);
    }

    private record ClientServerClose(
        ZLinkBackendSocketMonitor monitor,
        ZLinkApplicationJobReceiveFlowController.Registration receiveFlow) {
    }

    private record RouterListener(
        String endpoint,
        ZLinkBackendRouterSocket router,
        String advertiseHost) {
    }

    private record PublisherListener(
        String endpoint,
        ZLinkBackendPublisherSocket publisher,
        String advertiseHost) {
    }

    private record ServerDescriptorInput(
        String channelName,
        ZLinkBackendRouterSocket router,
        RoutingId routingId,
        ChannelRegistration registration) {
    }

    private record ServerDescriptorValue(
        String channelName,
        ZLinkClientServerServerDescriptor descriptor) {
    }

    private record AutoConnectSnapshot(
        List<ChannelRegistration> registrations,
        Map<String, ZLinkBackendDealerSocket> clients,
        Map<String, ZLinkBackendRouterSocket> servers,
        Map<String, RoutingId> serverRoutingIds,
        Map<String, ZLinkBackendPublisherSocket> publishers,
        Map<String, RoutingId> publisherRoutingIds,
        Map<String, ZLinkBackendRouterSocket> routeRouters) {
    }

    private static final class ClientServerConnection {
        private final String connectionId;
        private final Object transportLock = new Object();
        private final Set<String> aliases = new HashSet<>();
        private ZLinkClientServerServerDescriptor descriptor;
        private final ZLinkBackendDealerSocket dealer;
        private ZLinkBackendSocketMonitor monitor;
        private boolean ready;
        private boolean physicalClosed;
        private long pendingLivenessAckId;
        private long physicalGeneration = 1;
        private long admissionGeneration = 1;
        private long nextProbeAtNanos;
        private long deadlineAtNanos;
        private long outstandingProbeId;

        private ClientServerConnection(
            String connectionId,
            ZLinkClientServerServerDescriptor descriptor,
            ZLinkBackendDealerSocket dealer,
            boolean ready) {
            this.connectionId = connectionId;
            this.descriptor = descriptor;
            this.dealer = dealer;
            this.ready = ready;
            this.aliases.add(connectionId);
        }

        String connectionId() {
            return connectionId;
        }

        ZLinkClientServerServerDescriptor descriptor() {
            return descriptor;
        }

        ZLinkBackendDealerSocket dealer() {
            return dealer;
        }

        boolean ready() {
            return ready;
        }
    }

    private static final class ClientServerServerPeer {
        private final String key;
        private final String channelName;
        private final RoutingId routingId;
        private final ZLinkBackendRouterSocket router;
        private long nextProbeAtNanos;
        private long deadlineAtNanos;
        private long outstandingProbeId;

        private ClientServerServerPeer(
            String key,
            String channelName,
            RoutingId routingId,
            ZLinkBackendRouterSocket router,
            long nextProbeAtNanos,
            long deadlineAtNanos) {
            this.key = key;
            this.channelName = channelName;
            this.routingId = routingId;
            this.router = router;
            this.nextProbeAtNanos = nextProbeAtNanos;
            this.deadlineAtNanos = deadlineAtNanos;
        }
    }

    private static String serverPeerKey(
        String channelName,
        RoutingId routingId) {
        return channelName + '\0' + routingId.toHex();
    }

    private static String clientServerLogicalIdentity(
        ZLinkClientServerServerDescriptor descriptor) {
        return descriptor.channelName()
            + '\0' + descriptor.serverRid().toHex()
            + '\0' + descriptor.lifecycleGeneration();
    }

    private static ZLinkClientServerServerDescriptor
        descriptorFromAdmission(
            ZLinkClientServerServiceWire.Admission value,
            ZLinkClientServerServerDescriptor before) {
        return new ZLinkClientServerServerDescriptor(
            value.channelName(),
            value.serverRid(),
            value.lifecycleGeneration(),
            value.descriptorRevision(),
            value.advertisedEndpoint(),
            value.weight(),
            value.state(),
            value.securityIdentity(),
            before.ownerId(),
            before.leaseGeneration(),
            Instant.EPOCH);
    }

    private static boolean sameClientServerDescriptor(
        ZLinkClientServerServerDescriptor left,
        ZLinkClientServerServerDescriptor right) {
        return left.channelName().equals(right.channelName())
            && left.serverRid().equals(right.serverRid())
            && left.lifecycleGeneration()
                == right.lifecycleGeneration()
            && left.descriptorRevision()
                == right.descriptorRevision()
            && left.endpoint().equals(right.endpoint())
            && left.weight() == right.weight()
            && left.state() == right.state()
            && left.securityIdentity().equals(
                right.securityIdentity());
    }
}
