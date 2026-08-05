package systems.zlink.framework.runtime.channels;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.IdentityHashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
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
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotRouteBridge;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSubscriberSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitor;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkReceiveBatchBudget;

final class ZLinkChannelSocketRegistry {
    private static final long READY_POLL_INTERVAL_MILLIS = 5;

    private final Map<String, ChannelRegistration> registrations = new HashMap<>();
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
        java.util.concurrent.TimeUnit.SECONDS.toNanos(5);
    private static final long CLIENT_SERVER_DEADLINE_NANOS =
        java.util.concurrent.TimeUnit.SECONDS.toNanos(15);

    void registerChannel(ChannelRegistration registration) {
        registrations.put(registration.name(), registration);
    }

    ChannelRegistration registration(String channelName) {
        return registrations.get(channelName);
    }

    void registerClient(String channelName, ZLinkBackendDealerSocket socket) {
        clients.put(channelName, socket);
        ownedSockets.add(socket);
    }

    void registerServer(
        String channelName,
        RoutingId routingId,
        ZLinkBackendRouterSocket socket) {
        servers.put(channelName, socket);
        serverRoutingIds.put(channelName, routingId);
        ownedSockets.add(socket);
    }

    void registerPublisher(
        String channelName,
        RoutingId routingId,
        ZLinkBackendPublisherSocket socket) {
        publishers.put(channelName, socket);
        publisherRoutingIds.put(channelName, routingId);
        ownedSockets.add(socket);
    }

    void registerSubscriber(String channelName, ZLinkBackendSubscriberSocket socket) {
        subscribers.put(channelName, socket);
        ownedSockets.add(socket);
    }

    void registerRouteRouter(String channelName, ZLinkBackendRouterSocket socket) {
        routeRouters.put(channelName, socket);
        routeSocketLocks.put(channelName, new Object());
        ownedSockets.add(socket);
    }

    ZLinkBackendDealerSocket client(String channelName) {
        return clients.get(channelName);
    }

    synchronized ZLinkBackendDealerSocket clientForOutbound(String channelName) {
        Set<ClientServerConnection> physical =
            java.util.Collections.newSetFromMap(new IdentityHashMap<>());
        physical.addAll(clientServerConnections.values());
        List<ClientServerConnection> admitted = physical
            .stream()
            .filter(connection -> connection.ready()
                && connection.descriptor().channelName().equals(channelName)
                && connection.descriptor().weight() > 0
                && connection.descriptor().state()
                    == systems.zlink.framework.runtime.host
                        .ZLinkFrameworkRuntimeState.SERVING)
            .sorted(java.util.Comparator.comparing(
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
            .sorted(java.util.Comparator.comparing(
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
            .collect(java.util.stream.Collectors.toSet());
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
     * Waits for a ClientServer send target to become selectable, as
     * {@code framework/doc/framework/common/spec/08-channel-messaging.ko.md} §3.2 requires when the
     * ready candidate set is still empty at call time. The wait observes admission that is already
     * in flight; it never starts or restarts one, so no connection is opened here.
     *
     * <p>Deliberately not {@code synchronized}: admission completes through
     * {@link #admitClientServerConnection} on the monitor thread, which needs this monitor. Only the
     * per-attempt {@link #clientForOutbound} call takes it, never the sleep.
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
            if (System.nanoTime() >= deadline) {
                return null;
            }
            try {
                Thread.sleep(READY_POLL_INTERVAL_MILLIS);
            } catch (InterruptedException interrupted) {
                Thread.currentThread().interrupt();
                return clientForOutbound(channelName);
            }
        }
    }

    synchronized void addClientServerConnection(
        String connectionId,
        ZLinkClientServerServerDescriptor descriptor,
        ZLinkBackendDealerSocket dealer) {
        clientServerConnections.put(
            connectionId,
            new ClientServerConnection(
                connectionId, descriptor, dealer, false));
        ownedSockets.add(dealer);
    }

    synchronized void registerClientServerMonitor(
        String connectionId,
        ZLinkBackendSocketMonitor monitor) {
        ClientServerConnection current =
            clientServerConnections.get(connectionId);
        if (current == null) {
            monitor.close();
            return;
        }
        current.monitor = monitor;
        ownedSockets.add(monitor);
    }

    synchronized void enableUnmanagedBackendClientMode() {
        unmanagedBackendClientMode = true;
    }

    synchronized AdmissionFence clientServerTransportReady(
        String connectionId) {
        ClientServerConnection current =
            clientServerConnections.get(connectionId);
        return clientServerTransportReady(
            connectionId,
            current == null ? null : current.dealer);
    }

    synchronized AdmissionFence clientServerTransportReady(
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

    synchronized void clientServerTransportTerminated(
        String connectionId) {
        ClientServerConnection current =
            clientServerConnections.get(connectionId);
        clientServerTransportTerminated(
            connectionId,
            current == null ? null : current.dealer);
    }

    synchronized void clientServerTransportTerminated(
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

    synchronized void reconnectClientServerConnection(
        String connectionId) {
        ClientServerConnection current =
            clientServerConnections.get(connectionId);
        if (current != null) {
            current.ready = false;
            reconnectClientServer(current);
        }
    }

    synchronized boolean admitClientServerConnection(
        String connectionId,
        ZLinkClientServerServerDescriptor descriptor) {
        ClientServerConnection current =
            clientServerConnections.get(connectionId);
        return current != null && admitClientServerConnection(
            connectionId,
            descriptor,
            new AdmissionFence(
                current.physicalGeneration,
                current.admissionGeneration,
                current.dealer));
    }

    synchronized boolean admitClientServerConnection(
        String connectionId,
        ZLinkClientServerServerDescriptor descriptor,
        AdmissionFence fence) {
        ClientServerConnection current =
            clientServerConnections.get(connectionId);
        if (current == null
            || fence == null
            || current.dealer != fence.dealer()
            || current.physicalGeneration != fence.physicalGeneration()
            || current.admissionGeneration != fence.admissionGeneration()) {
            return false;
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
            closeClientServerPhysical(current);
        }
        return true;
    }

    synchronized void updateClientServerConnection(
        String connectionId,
        ZLinkClientServerServerDescriptor descriptor,
        boolean ready) {
        ClientServerConnection current =
            clientServerConnections.get(connectionId);
        if (current != null) {
            current.descriptor = descriptor;
            current.ready = ready;
        }
    }

    synchronized ZLinkClientServerServerDescriptor
        clientServerConnectionDescriptor(String connectionId) {
        ClientServerConnection current =
            clientServerConnections.get(connectionId);
        return current == null ? null : current.descriptor;
    }

    synchronized boolean ownsClientServerPhysical(
        String connectionId,
        ZLinkBackendDealerSocket dealer) {
        ClientServerConnection current =
            clientServerConnections.get(connectionId);
        return current != null && current.dealer == dealer;
    }

    synchronized void removeClientServerConnection(String connectionId) {
        ClientServerConnection current =
            clientServerConnections.remove(connectionId);
        if (current == null) {
            return;
        }
        current.aliases.remove(connectionId);
        if (current.aliases.isEmpty()) {
            closeClientServerPhysical(current);
        }
    }

    synchronized int clientServerPhysicalConnectionCount() {
        Set<ClientServerConnection> physical =
            java.util.Collections.newSetFromMap(new IdentityHashMap<>());
        physical.addAll(clientServerConnections.values());
        return physical.size();
    }

    synchronized List<ClientServerTargetSnapshot>
        clientServerTargetSnapshots(String channelName) {
        Map<String, ClientServerTargetSnapshot> targets =
            new java.util.LinkedHashMap<>();
        Set<ClientServerConnection> physical =
            java.util.Collections.newSetFromMap(new IdentityHashMap<>());
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
                    connection.ready()
                        && descriptor.state()
                            == systems.zlink.framework.runtime.host
                                .ZLinkFrameworkRuntimeState.SERVING));
        }
        return List.copyOf(targets.values());
    }

    synchronized boolean hasClientRegistration(String channelName) {
        ChannelRegistration registration = registrations.get(channelName);
        return registration != null
            && registration.kind() == ChannelKind.CLIENT_SERVER
            && registration.clientEnabled();
    }

    synchronized void setClientServerServerDescriptor(
        String channelName,
        ZLinkClientServerServerDescriptor descriptor) {
        if (descriptor == null) {
            clientServerServerDescriptors.remove(channelName);
        } else {
            clientServerServerDescriptors.put(channelName, descriptor);
            pushClientServerDescriptorUpdate(channelName, descriptor);
        }
    }

    synchronized ZLinkClientServerServerDescriptor
        clientServerServerDescriptor(String channelName) {
        return clientServerServerDescriptors.get(channelName);
    }

    synchronized int clientServerServerWeight(
        String channelName,
        int fallback) {
        ZLinkBackendRouterSocket server = servers.get(channelName);
        return server == null ? fallback : server.peerWeight();
    }

    synchronized void initializeClientServerServerDescriptors(String ownerId) {
        for (Map.Entry<String, ZLinkBackendRouterSocket> entry
            : servers.entrySet()) {
            ChannelRegistration registration =
                registrations.get(entry.getKey());
            if (registration == null
                || registration.serverBinds().isEmpty()) {
                continue;
            }
            String endpoint = advertisedEndpoint(
                registration.serverBinds().get(0),
                entry.getValue(),
                registration.clientServerAdvertiseHost());
            clientServerServerDescriptors.put(
                entry.getKey(),
                new ZLinkClientServerServerDescriptor(
                    entry.getKey(),
                    serverRoutingIds.get(entry.getKey()),
                    java.util.concurrent.ThreadLocalRandom.current()
                        .nextLong(1, Long.MAX_VALUE),
                    1,
                    endpoint,
                    entry.getValue().peerWeight(),
                    systems.zlink.framework.runtime.host
                        .ZLinkFrameworkRuntimeState.SERVING,
                    "default",
                    ownerId,
                    1,
                    java.time.Instant.EPOCH));
        }
    }

    boolean tryHandleClientServerControl(
        String channelName,
        ZLinkBackendRouterSocket router,
        systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived received) {
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
                && received.requestSeq().isEmpty()) {
                acceptClientServerServerAck(
                    channelName, received.routingId().get(), ack.probeId());
                received.close();
                return true;
            }
            if (control
                    instanceof ZLinkClientServerServiceWire.LivenessProbe probe
                && received.requestSeq().isPresent()) {
                reply = ZLinkClientServerServiceWire.encodeLivenessAck(
                    probe.probeId());
            } else {
            ZLinkClientServerServerDescriptor descriptor;
            synchronized (this) {
                descriptor = clientServerServerDescriptors.get(channelName);
            }
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
            if (received.requestSeq().isPresent()) {
                reply = ZLinkClientServerServiceWire.encodeReject(1);
            }
        }
        if (reply != null && received.requestSeq().isPresent()) {
            try (Message message = Message.from(reply)) {
                received.reply(List.of(message));
            }
        } else if (reply != null && received.routingId().isPresent()) {
            try {
                router.disconnectPeer(received.routingId().get());
            } catch (RuntimeException ignored) {
            }
        }
        received.close();
        return true;
    }

    void tickClientServerLiveness(long nowNanos, Duration requestTimeout) {
        List<ClientServerConnection> clientConnections;
        List<ClientServerServerPeer> serverPeers;
        synchronized (this) {
            Set<ClientServerConnection> physical =
                java.util.Collections.newSetFromMap(new IdentityHashMap<>());
            physical.addAll(clientServerConnections.values());
            clientConnections = new ArrayList<>(physical);
            serverPeers = List.copyOf(clientServerServerPeers.values());
        }
        clientConnections.sort(java.util.Comparator.comparing(
            ClientServerConnection::connectionId));
        int connectionCount = clientConnections.size();
        int connectionStart = connectionCount == 0
            ? 0
            : (int) Math.floorMod(
                clientServerControlCursor, connectionCount);
        for (int offset = 0; offset < connectionCount; offset++) {
            ClientServerConnection connection = clientConnections.get(
                (connectionStart + offset) % connectionCount);
            clientServerControlCursor =
                (connectionStart + offset + 1) % connectionCount;
            drainClientServerControls(connection);
            long probeId = 0;
            long physicalGeneration = 0;
            synchronized (this) {
                if (!connection.aliases.stream().anyMatch(
                        alias -> clientServerConnections.get(alias)
                            == connection)
                    || !connection.ready) {
                    continue;
                }
                if (nowNanos >= connection.deadlineAtNanos) {
                    connection.ready = false;
                    reconnectClientServer(connection);
                    continue;
                }
                if (nowNanos < connection.nextProbeAtNanos) {
                    continue;
                }
                connection.nextProbeAtNanos =
                    nowNanos + CLIENT_SERVER_PROBE_INTERVAL_NANOS;
                probeId = connection.outstandingProbeId == 0
                    ? allocateProbeId()
                    : connection.outstandingProbeId;
                connection.outstandingProbeId = probeId;
                physicalGeneration = connection.physicalGeneration;
            }
            requestClientServerProbe(
                connection,
                physicalGeneration,
                probeId,
                requestTimeout);
        }
        for (ClientServerServerPeer peer : serverPeers) {
            long probeId;
            synchronized (this) {
                ClientServerServerPeer current =
                    clientServerServerPeers.get(peer.key);
                if (current != peer) {
                    continue;
                }
                if (nowNanos >= current.deadlineAtNanos) {
                    clientServerServerPeers.remove(peer.key);
                    try {
                        current.router.disconnectPeer(
                            current.routingId);
                    } catch (RuntimeException ignored) {
                    }
                    continue;
                }
                if (nowNanos < current.nextProbeAtNanos) {
                    continue;
                }
                current.nextProbeAtNanos =
                    nowNanos + CLIENT_SERVER_PROBE_INTERVAL_NANOS;
                probeId = current.outstandingProbeId == 0
                    ? allocateProbeId()
                    : current.outstandingProbeId;
                current.outstandingProbeId = probeId;
            }
            try (Message message = Message.from(
                ZLinkClientServerServiceWire.encodeLivenessProbe(probeId))) {
                try {
                    peer.router.send(
                        peer.routingId,
                        List.of(message),
                        SendFlags.DONT_WAIT);
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
                    try (Message ack = Message.from(
                        ZLinkClientServerServiceWire.encodeLivenessAck(
                            probe.probeId()))) {
                        connection.dealer.send(
                            List.of(ack), SendFlags.DONT_WAIT);
                    }
                } else if (control
                        instanceof ZLinkClientServerServiceWire.Update update) {
                    applyClientServerUpdate(connection, update.admission());
                } else {
                    terminateClientServerProtocol(connection);
                }
            } catch (RuntimeException failure) {
                terminateClientServerProtocol(connection);
            }
        }
    }

    private void requestClientServerProbe(
        ClientServerConnection connection,
        long physicalGeneration,
        long probeId,
        Duration requestTimeout) {
        try (Message message = Message.from(
            ZLinkClientServerServiceWire.encodeLivenessProbe(probeId))) {
            connection.dealer.request(
                List.of(message),
                reply -> {
                    try (reply) {
                        if (reply.result() != ZLinkBackendRequestResult.OK
                            || reply.parts().size() != 1) {
                            return;
                        }
                        ZLinkClientServerServiceWire.Control control =
                            ZLinkClientServerServiceWire.decode(
                                reply.parts().get(0).toByteArray());
                        if (!(control
                                instanceof ZLinkClientServerServiceWire
                                    .LivenessAck ack)
                            || ack.probeId() != probeId) {
                            return;
                        }
                        synchronized (this) {
                            if (connection.aliases.stream().anyMatch(
                                    alias -> clientServerConnections.get(alias)
                                        == connection)
                                && connection.physicalGeneration
                                    == physicalGeneration
                                && connection.outstandingProbeId == probeId) {
                                connection.outstandingProbeId = 0;
                                connection.deadlineAtNanos =
                                    System.nanoTime()
                                        + CLIENT_SERVER_DEADLINE_NANOS;
                            }
                        }
                    }
                },
                SendFlags.DONT_WAIT,
                requestTimeout);
        }
    }

    private synchronized void applyClientServerUpdate(
        ClientServerConnection connection,
        ZLinkClientServerServiceWire.Admission update) {
        if (!connection.aliases.stream().anyMatch(
                alias -> clientServerConnections.get(alias) == connection)
            || !connection.ready) {
            return;
        }
        ZLinkClientServerServerDescriptor before = connection.descriptor;
        if (!update.channelName().equals(before.channelName())
            || !update.serverRid().equals(before.serverRid())
            || update.lifecycleGeneration()
                != before.lifecycleGeneration()
            || !update.securityIdentity().equals(
                before.securityIdentity())
            || !update.advertisedEndpoint().equals(before.endpoint())) {
            terminateClientServerProtocol(connection);
            return;
        }
        if (update.descriptorRevision()
            < before.descriptorRevision()) {
            return;
        }
        ZLinkClientServerServerDescriptor candidate =
            descriptorFromAdmission(update, before);
        if (update.descriptorRevision()
            == before.descriptorRevision()) {
            if (!sameClientServerDescriptor(candidate, before)) {
                terminateClientServerProtocol(connection);
            }
            return;
        }
        connection.descriptor = candidate;
    }

    private synchronized void terminateClientServerProtocol(
        ClientServerConnection connection) {
        if (!connection.aliases.stream().anyMatch(
                alias -> clientServerConnections.get(alias) == connection)) {
            return;
        }
        connection.ready = false;
        reconnectClientServer(connection);
    }

    // Transport liveness spec 55 section 6: terminal cleanup never leaves a
    // monitor subscription behind its connection, so the monitor closes before
    // the DEALER it observes.
    private void closeClientServerPhysical(
        ClientServerConnection connection) {
        if (connection.monitor != null) {
            ownedSockets.removeIf(
                candidate -> candidate == connection.monitor);
            try {
                connection.monitor.close();
            } catch (RuntimeException ignored) {
            }
            connection.monitor = null;
        }
        ownedSockets.removeIf(candidate -> candidate == connection.dealer);
        try {
            connection.dealer.close();
        } catch (RuntimeException ignored) {
        }
    }

    private void reconnectClientServer(ClientServerConnection connection) {
        connection.physicalGeneration++;
        connection.admissionGeneration++;
        connection.outstandingProbeId = 0;
        try {
            connection.dealer.disconnect(
                connection.descriptor.endpoint());
            connection.dealer.connect(connection.descriptor.endpoint());
        } catch (RuntimeException ignored) {
        }
    }

    private synchronized void admitClientServerServerPeer(
        String channelName,
        RoutingId routingId,
        ZLinkBackendRouterSocket router) {
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
    }

    private synchronized void acceptClientServerServerAck(
        String channelName,
        RoutingId routingId,
        long probeId) {
        ClientServerServerPeer peer =
            clientServerServerPeers.get(
                serverPeerKey(channelName, routingId));
        if (peer == null || peer.outstandingProbeId != probeId) {
            return;
        }
        peer.outstandingProbeId = 0;
        peer.deadlineAtNanos =
            System.nanoTime() + CLIENT_SERVER_DEADLINE_NANOS;
    }

    private synchronized void pushClientServerDescriptorUpdate(
        String channelName,
        ZLinkClientServerServerDescriptor descriptor) {
        for (ClientServerServerPeer peer :
            List.copyOf(clientServerServerPeers.values())) {
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
                        List.of(message),
                        SendFlags.DONT_WAIT);
                } catch (ZlinkSubmitException ignored) {
                    // The Location Store update remains authoritative. A peer
                    // that has already lost admission cannot receive this
                    // best-effort descriptor projection and is removed by the
                    // normal connection lifecycle.
                }
            }
        }
    }

    private synchronized long allocateProbeId() {
        long result = nextClientServerProbeId;
        nextClientServerProbeId =
            result == Long.MAX_VALUE ? 1 : result + 1;
        return result;
    }

    ZLinkBackendRouterSocket server(String channelName) {
        return servers.get(channelName);
    }

    ZLinkBackendPublisherSocket publisher(String channelName) {
        return publishers.get(channelName);
    }

    ZLinkBackendSubscriberSocket subscriber(String channelName) {
        return subscribers.get(channelName);
    }

    ZLinkBackendRouterSocket routeRouter(String channelName) {
        return routeRouters.get(channelName);
    }

    Object routeSocketLock(String channelName, Object fallback) {
        return routeSocketLocks.getOrDefault(channelName, fallback);
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
        spotRouterNodes.put(channelName, node);
    }

    ZLinkInternalSpotNode spotRouterNode(String channelName) {
        return spotRouterNodes.get(channelName);
    }

    Map<String, ZLinkBackendSocket> monitoringSocketSources() {
        Map<String, ZLinkBackendSocket> sources = new HashMap<>();
        sources.putAll(clients);
        sources.putAll(servers);
        sources.putAll(publishers);
        sources.putAll(subscribers);
        sources.putAll(routeRouters);
        return Map.copyOf(sources);
    }

    List<ZLinkChannelRuntime.AutoConnectSurface> autoConnectSurfaces() {
        List<ZLinkChannelRuntime.AutoConnectSurface> surfaces = new ArrayList<>();
        for (ChannelRegistration channel : registrations.values()) {
            addAutoConnectSurfaces(channel, surfaces);
        }
        return List.copyOf(surfaces);
    }

    void closeSpotRouteBridges() {
        closeAll(spotRouteBridges.values(), java.util.Collections.newSetFromMap(
            new IdentityHashMap<>()));
        spotRouteBridges.clear();
    }

    void closeAll() {
        for (ChannelRegistration registration : registrations.values()) {
            registration.detachRuntimeConnections();
        }
        // ClientServer physical connections close first so that spec 55
        // section 6 holds here too: the admission monitor never outlives the
        // DEALER it observes. Draining ownedSockets in insertion order would
        // close the DEALER before its monitor.
        Set<ClientServerConnection> physical =
            java.util.Collections.newSetFromMap(new IdentityHashMap<>());
        physical.addAll(clientServerConnections.values());
        for (ClientServerConnection connection : physical) {
            closeClientServerPhysical(connection);
        }
        closeAll(ownedSockets, java.util.Collections.newSetFromMap(new IdentityHashMap<>()));
        ownedSockets.clear();
        clientServerConnections.clear();
        clientServerServerDescriptors.clear();
        clientServerServerPeers.clear();
    }

    private void addAutoConnectSurfaces(
        ChannelRegistration channel,
        List<ZLinkChannelRuntime.AutoConnectSurface> surfaces) {
        if (channel.kind() == ChannelKind.CLIENT_SERVER) {
            addClientServerSurfaces(channel, surfaces);
        } else if (channel.kind() == ChannelKind.FANOUT) {
            addFanoutSurfaces(channel, surfaces);
        } else if (channel.kind() == ChannelKind.ROUTE_MESH) {
            addRouteSurfaces(channel, surfaces);
        }
    }

    private void addClientServerSurfaces(
        ChannelRegistration channel,
        List<ZLinkChannelRuntime.AutoConnectSurface> surfaces) {
        ZLinkBackendRouterSocket server = servers.get(channel.name());
        if (server != null) {
            for (String endpoint : channel.serverBinds()) {
                surfaces.add(new ZLinkChannelRuntime.AutoConnectSurface(
                    ZLinkAutoConnectType.CLIENT_SERVER,
                    channel.name(),
                    ZLinkLocationRole.ROUTER,
                    serverRoutingIds.get(channel.name()),
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
                clients.get(channel.name()),
                channel.clientManualEndpoints()));
        }
    }

    private void addFanoutSurfaces(
        ChannelRegistration channel,
        List<ZLinkChannelRuntime.AutoConnectSurface> surfaces) {
        if (publishers.containsKey(channel.name())) {
            for (String endpoint : channel.publisherBinds()) {
                surfaces.add(new ZLinkChannelRuntime.AutoConnectSurface(
                    ZLinkAutoConnectType.FANOUT,
                    channel.name(),
                    ZLinkLocationRole.PUB,
                    publisherRoutingIds.get(channel.name()),
                    advertisedEndpoint(
                        endpoint,
                        publishers.get(channel.name()),
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
        List<ZLinkChannelRuntime.AutoConnectSurface> surfaces) {
        ZLinkBackendRouterSocket router = routeRouters.get(channel.name());
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
        if (advertiseHost == null || advertiseHost.isBlank()) {
            return endpoint;
        }
        java.net.URI value = java.net.URI.create(endpoint);
        try {
            return new java.net.URI(
                value.getScheme(),
                value.getUserInfo(),
                advertiseHost,
                value.getPort(),
                value.getPath(),
                value.getQuery(),
                value.getFragment()).toString();
        } catch (java.net.URISyntaxException invalid) {
            throw new IllegalArgumentException(
                "Invalid ClientServer advertise host.", invalid);
        }
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
        if (advertiseHost == null || advertiseHost.isBlank()) {
            return endpoint;
        }
        java.net.URI value = java.net.URI.create(endpoint);
        try {
            return new java.net.URI(
                value.getScheme(), value.getUserInfo(), advertiseHost,
                value.getPort(), value.getPath(), value.getQuery(), value.getFragment())
                .toString();
        } catch (java.net.URISyntaxException invalid) {
            throw new IllegalArgumentException(
                "Invalid Fanout advertise host.", invalid);
        }
    }

    private static void closeAll(
        Iterable<? extends ZLinkBackendObject> closeables,
        Set<ZLinkBackendObject> closed) {
        for (ZLinkBackendObject closeable : closeables) {
            if (closeable != null && closed.add(closeable)) {
                try {
                    closeable.close();
                } catch (systems.zlink.contracts.errors.ZlinkCloseException ignored) {
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
        boolean ready) {
    }

    private static final class ClientServerConnection {
        private final String connectionId;
        private final Set<String> aliases = new java.util.HashSet<>();
        private ZLinkClientServerServerDescriptor descriptor;
        private final ZLinkBackendDealerSocket dealer;
        private ZLinkBackendSocketMonitor monitor;
        private boolean ready;
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
