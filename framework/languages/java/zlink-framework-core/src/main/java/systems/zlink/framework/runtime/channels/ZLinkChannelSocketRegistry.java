package systems.zlink.framework.runtime.channels;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkCloseException;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locations.ZLinkLocationRole;
import systems.zlink.framework.monitoring.ZLinkListenerKind;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.ZLinkCompletionBridge;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendDealerSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendObject;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendPublisherSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRecvMode;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRouterSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitor;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotRouteBridge;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSubscriberSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobReceiveFlowController;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkReceiveBatchBudget;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.locations.ZLinkAutoConnectType;
import systems.zlink.framework.runtime.internal.locations.ZLinkClientServerServerDescriptor;
import systems.zlink.framework.runtime.internal.transport.ZLinkListenerIdentity;

import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashMap;
import java.util.HashSet;
import java.util.IdentityHashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.Executor;
import java.util.concurrent.ThreadLocalRandom;
import java.util.concurrent.TimeUnit;
import java.util.function.BiConsumer;
import java.util.function.BiFunction;
import java.util.function.Consumer;
import java.util.function.Supplier;
import java.util.logging.Level;
import java.util.logging.Logger;
import java.util.stream.Collectors;

final class ZLinkChannelSocketRegistry {
    private static final Logger LOGGER =
            Logger.getLogger(ZLinkChannelSocketRegistry.class.getName());

    private final Map<String, ChannelRegistration> registrations = new ConcurrentHashMap<>();
    private final ZLinkApplicationJobQueue applicationJobQueue;

    private final ZLinkStateLane stateLane = new ZLinkStateLane();
    private final Map<ZLinkBackendObject, ZLinkApplicationJobReceiveFlowController.Registration>
            receiveFlowRegistrations = new IdentityHashMap<>();
    private final Map<String, ZLinkBackendDealerSocket> clients = new HashMap<>();
    // Bound listener records: written when a listener finishes binding. The listener status query
    // reads only this map.
    private final Map<ListenerKey, String> listenerEndpoints = new HashMap<>();
    private final Map<String, ZLinkBackendRouterSocket> servers = new HashMap<>();
    private final Map<String, RoutingId> serverRoutingIds = new HashMap<>();
    private final Map<String, ZLinkBackendPublisherSocket> publishers = new HashMap<>();
    private final Map<String, RoutingId> publisherRoutingIds = new HashMap<>();
    private final Map<String, ZLinkBackendSubscriberSocket> subscribers = new HashMap<>();
    private final Map<String, ZLinkBackendRouterSocket> routeRouters = new HashMap<>();
    private final Map<String, ClientServerConnection> clientServerConnections = new HashMap<>();
    private final Map<String, ZLinkClientServerServerDescriptor> clientServerServerDescriptors =
            new HashMap<>();
    private final Map<String, Map<String, Long>> clientServerSelectionCurrents = new HashMap<>();
    private final Map<String, Object> routeSocketLocks = new HashMap<>();
    private final Map<String, ZLinkBackendSpotRouteBridge> spotRouteBridges =
            new ConcurrentHashMap<>();
    private final Map<String, ZLinkInternalSpotNode> spotRouterNodes = new ConcurrentHashMap<>();
    private final List<ZLinkBackendObject> ownedSockets = new ArrayList<>();
    private final Map<String, ClientServerServerPeer> clientServerServerPeers = new HashMap<>();
    private long nextClientServerProbeId = 1;
    private long clientServerControlCursor;
    private boolean unmanagedBackendClientMode;
    private static final long CLIENT_SERVER_PROBE_INTERVAL_NANOS = TimeUnit.SECONDS.toNanos(5);
    private static final long CLIENT_SERVER_DEADLINE_NANOS = TimeUnit.SECONDS.toNanos(15);
    // The public Poller has no wake-up call, and a DEALER may close only after its receive owner
    // left the poller wait (a Core socket is not thread safe). This interval bounds how long a
    // close waits for that owner; data and completions end the wait at once. Same value and
    // reason as the .NET ClientServer client ControlReceivePollInterval.
    private static final Duration CLIENT_SERVER_CONTROL_RECEIVE_WAIT = Duration.ofMillis(100);
    private volatile BiConsumer<String, Throwable> clientServerReceiveFailures = (channel, f) -> {};

    ZLinkChannelSocketRegistry() {
        this(null);
    }

    ZLinkChannelSocketRegistry(ZLinkApplicationJobQueue applicationJobQueue) {
        this(applicationJobQueue, System::nanoTime, () -> false);
    }

    ZLinkChannelSocketRegistry(
            ZLinkApplicationJobQueue applicationJobQueue,
            java.util.function.LongSupplier requestNanoTime,
            java.util.function.BooleanSupplier closing) {
        this.applicationJobQueue = applicationJobQueue;
        this.requestNanoTime = requestNanoTime;
        this.closing = closing;
    }

    private final java.util.function.LongSupplier requestNanoTime;
    private final java.util.function.BooleanSupplier closing;

    private record TopologySignal(Runnable changed, Runnable shutdown) {}

    private final java.util.List<TopologySignal> topologySignals =
            new java.util.concurrent.CopyOnWriteArrayList<>();

    void rejectPendingAdmission() {
        inStateLane(
                () -> {
                    topologySignals.forEach(signal -> signal.shutdown().run());
                    return null;
                });
    }

    void onTopologyChanged(Runnable signal) {
        topologySignals.add(new TopologySignal(signal, () -> {}));
    }

    void signalTopologyChanged() {
        topologySignals.forEach(signal -> signal.changed().run());
    }

    private <T> T inTopologyTurn(Supplier<T> work) {
        T result = inStateLane(work);
        signalTopologyChanged();
        return result;
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

    void registerChannel(ChannelRegistration registration) {
        inStateLane(
                () -> {
                    registrations.put(registration.name(), registration);
                    return null;
                });
    }

    ChannelRegistration registration(String channelName) {
        return inStateLane(() -> registrations.get(channelName));
    }

    void registerClient(String channelName, ZLinkBackendDealerSocket socket) {
        registerReceiveFlow(socket);
        inTopologyTurn(
                () -> {
                    clients.put(channelName, socket);
                    ownedSockets.add(socket);
                    return null;
                });
    }

    void registerServer(String channelName, RoutingId routingId, ZLinkBackendRouterSocket socket) {
        registerReceiveFlow(socket);
        inTopologyTurn(
                () -> {
                    servers.put(channelName, socket);
                    serverRoutingIds.put(channelName, routingId);
                    ownedSockets.add(socket);
                    return null;
                });
    }

    void registerPublisher(
            String channelName, RoutingId routingId, ZLinkBackendPublisherSocket socket) {
        inTopologyTurn(
                () -> {
                    publishers.put(channelName, socket);
                    publisherRoutingIds.put(channelName, routingId);
                    ownedSockets.add(socket);
                    return null;
                });
    }

    void registerSubscriber(String channelName, ZLinkBackendSubscriberSocket socket) {
        inTopologyTurn(
                () -> {
                    subscribers.put(channelName, socket);
                    ownedSockets.add(socket);
                    return null;
                });
    }

    void registerRouteRouter(String channelName, ZLinkBackendRouterSocket socket) {
        registerReceiveFlow(socket);
        inStateLane(
                () -> {
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
        Set<ClientServerConnection> physical = Collections.newSetFromMap(new IdentityHashMap<>());
        physical.addAll(clientServerConnections.values());
        physical.removeIf(
                connection ->
                        !connection.ready()
                                || !connection.descriptor().channelName().equals(channelName));
        List<ClientServerConnection> admitted =
                physical.stream()
                        .filter(
                                connection ->
                                        connection.descriptor().weight() > 0
                                                && connection.descriptor().state()
                                                        == systems.zlink.framework.runtime.host
                                                                .ZLinkFrameworkRuntimeState.SERVING)
                        .sorted(Comparator.comparing(ClientServerConnection::connectionId))
                        .toList();
        Map<String, ClientServerConnection> distinct = new LinkedHashMap<>();
        for (ClientServerConnection connection : admitted) {
            distinct.putIfAbsent(clientServerLogicalIdentity(connection.descriptor()), connection);
        }
        List<ClientServerConnection> eligible =
                distinct.values().stream()
                        .sorted(
                                Comparator.comparing(
                                        connection -> connection.descriptor().serverRid().toHex()))
                        .toList();
        long total = 0;
        for (ClientServerConnection connection : eligible) {
            total = Math.addExact(total, connection.descriptor().weight());
        }
        if (total == 0) {
            if (!physical.isEmpty()) {
                throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.UNAVAILABLE,
                        "client/server channel has no eligible server: " + channelName);
            }
            return unmanagedBackendClientMode ? clients.get(channelName) : null;
        }
        Map<String, Long> currentByServer =
                clientServerSelectionCurrents.computeIfAbsent(
                        channelName, ignored -> new HashMap<>());
        Set<String> eligibleServerIds =
                eligible.stream()
                        .map(connection -> connection.descriptor().serverRid().toHex())
                        .collect(Collectors.toSet());
        currentByServer.keySet().removeIf(serverId -> !eligibleServerIds.contains(serverId));
        ClientServerConnection selectedConnection = null;
        long selectedCurrent = Long.MIN_VALUE;
        for (ClientServerConnection connection : eligible) {
            String serverId = connection.descriptor().serverRid().toHex();
            long current =
                    Math.addExact(
                            currentByServer.getOrDefault(serverId, 0L),
                            connection.descriptor().weight());
            currentByServer.put(serverId, current);
            if (selectedConnection == null
                    || current > selectedCurrent
                    || current == selectedCurrent
                            && serverId.compareTo(
                                            selectedConnection.descriptor().serverRid().toHex())
                                    < 0) {
                selectedConnection = connection;
                selectedCurrent = current;
            }
        }
        if (selectedConnection != null) {
            String selectedId = selectedConnection.descriptor().serverRid().toHex();
            currentByServer.put(
                    selectedId, Math.subtractExact(currentByServer.get(selectedId), total));
            return selectedConnection.dealer();
        }
        throw new IllegalStateException(
                "ClientServer weighted selection did not select a connection");
    }

    /** Starts one binding operation after Registry-owned logical admission. */
    <T> CompletionStage<T> submitToChannel(
            String channelName,
            Duration timeoutOverride,
            Duration defaultTimeout,
            BiFunction<ZLinkBackendDealerSocket, Duration, CompletionStage<T>> clientSubmit,
            BiFunction<ZLinkInternalSpotNode, Duration, CompletionStage<T>> meshSubmit,
            java.util.function.Consumer<Throwable> rejectedSubmission) {
        long started = requestNanoTime.getAsLong();
        return submitInStateLane(
                () -> {
                    ChannelRegistration registration = registrations.get(channelName);
                    Duration timeout =
                            requestTimeoutCore(channelName, timeoutOverride, defaultTimeout);
                    if (registration != null
                            && registration.kind() == ChannelKind.CLIENT_SERVER
                            && registration.clientEnabled()) {
                        ZLinkBackendDealerSocket target = clientForOutboundCore(channelName);
                        if (target != null)
                            return clientSubmit.apply(
                                    target,
                                    systems.zlink.framework.runtime.internal.calls.ZLinkRequestCalls
                                            .remainingTimeout(
                                                    timeout, started, requestNanoTime.getAsLong()));
                        return waitForClientServerAdmissionCore(
                                channelName, started, timeout, clientSubmit, rejectedSubmission);
                    }
                    ZLinkInternalSpotNode node = spotRouterNodes.get(channelName);
                    if (node != null)
                        return meshSubmit.apply(
                                node,
                                systems.zlink.framework.runtime.internal.calls.ZLinkRequestCalls
                                        .remainingTimeout(
                                                timeout, started, requestNanoTime.getAsLong()));
                    if (registration != null
                            && registration.kind() == ChannelKind.CLIENT_SERVER
                            && registration.clientServerServerEnabled()) {
                        throw new ZLinkConfigurationException(
                                ZLinkFrameworkErrorKind.NOT_CONFIGURED,
                                "ClientServer Client role is not registered for this channel: "
                                        + channelName);
                    }
                    throw new ZLinkConfigurationException(
                            ZLinkFrameworkErrorKind.NOT_FOUND,
                            "channel has no request route: " + channelName);
                },
                rejectedSubmission);
    }

    private <T> CompletionStage<T> waitForClientServerAdmissionCore(
            String channelName,
            long started,
            Duration requestTimeout,
            BiFunction<ZLinkBackendDealerSocket, Duration, CompletionStage<T>> submit,
            java.util.function.Consumer<Throwable> rejectedSubmission) {
        var admission = new java.util.concurrent.CompletableFuture<CompletionStage<T>>();
        var result = new java.util.concurrent.CompletableFuture<T>();
        ZLinkFlowContext.State flow = ZLinkFlowContext.current();
        Runnable signal =
                () ->
                        stateLane
                                .runNowOrQueue(
                                        () -> {
                                            if (admission.isDone()) return null;
                                            try (var ignored =
                                                    flow == null
                                                            ? ZLinkFlowContext.suppress()
                                                            : ZLinkFlowContext.enter(flow)) {
                                                ZLinkBackendDealerSocket target =
                                                        clientForOutboundCore(channelName);
                                                if (target == null || !admission.complete(result))
                                                    return null;
                                                // Claim readiness before transferring payload
                                                // ownership to binding submission.
                                                // Timeout/cancellation can reject only an unclaimed
                                                // admission.
                                                CompletionStage<T> operation;
                                                try {
                                                    operation =
                                                            submit.apply(
                                                                    target,
                                                                    systems.zlink.framework.runtime
                                                                            .internal.calls
                                                                            .ZLinkRequestCalls
                                                                            .remainingTimeout(
                                                                                    requestTimeout,
                                                                                    started,
                                                                                    requestNanoTime
                                                                                            .getAsLong()));
                                                } catch (RuntimeException | Error failure) {
                                                    rejectSubmission(
                                                            flow, rejectedSubmission, failure);
                                                    result.completeExceptionally(failure);
                                                    return null;
                                                }
                                                operation.whenComplete(
                                                        (value, failure) -> {
                                                            if (failure == null)
                                                                result.complete(value);
                                                            else
                                                                result.completeExceptionally(
                                                                        failure);
                                                        });
                                                ZLinkCompletionBridge.forwardCancellation(
                                                        result, operation);
                                            }
                                            return null;
                                        })
                                .whenComplete(
                                        (unused, failure) -> {
                                            if (failure != null)
                                                admission.completeExceptionally(failure);
                                        });
        // The readiness check and registration share the Registry turn.
        var listener =
                new TopologySignal(
                        signal,
                        () ->
                                admission.completeExceptionally(
                                        new ZLinkFrameworkException(
                                                ZLinkFrameworkErrorKind.SHUTTING_DOWN,
                                                "channel runtime is shutting down")));
        topologySignals.add(listener);
        admission.whenComplete(
                (operation, failure) -> {
                    topologySignals.remove(listener);
                    if (failure != null) {
                        Throwable terminal =
                                failure instanceof java.util.concurrent.TimeoutException
                                        ? new ZLinkFrameworkException(
                                                ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                                                "client/server channel admission timed out: "
                                                        + channelName,
                                                failure)
                                        : failure;
                        rejectSubmission(flow, rejectedSubmission, terminal);
                        result.completeExceptionally(terminal);
                    }
                });
        if (requestTimeout != null) {
            long remaining = requestTimeout.toNanos() - (requestNanoTime.getAsLong() - started);
            admission.orTimeout(Math.max(0, remaining), TimeUnit.NANOSECONDS);
        }
        ZLinkCompletionBridge.forwardCancellation(result, admission);
        return result;
    }

    /** Starts a direct node operation without exporting the selected transport. */
    <T> CompletionStage<T> submitToNode(
            String channelName,
            Duration timeoutOverride,
            Duration defaultTimeout,
            BiFunction<ZLinkBackendRouterSocket, Duration, CompletionStage<T>> routerSubmit,
            BiFunction<ZLinkInternalSpotNode, Duration, CompletionStage<T>> nodeSubmit,
            java.util.function.Consumer<Throwable> rejectedSubmission) {
        long started = defaultTimeout == null ? 0 : requestNanoTime.getAsLong();
        return submitInStateLane(
                () -> {
                    Duration timeout =
                            requestTimeoutCore(channelName, timeoutOverride, defaultTimeout);
                    ZLinkBackendRouterSocket router = routeRouters.get(channelName);
                    if (router != null) {
                        return routerSubmit.apply(
                                router,
                                systems.zlink.framework.runtime.internal.calls.ZLinkRequestCalls
                                        .remainingTimeout(
                                                timeout, started, requestNanoTime.getAsLong()));
                    }
                    ZLinkInternalSpotNode node = spotRouterNodes.get(channelName);
                    if (node != null) {
                        return nodeSubmit.apply(
                                node,
                                systems.zlink.framework.runtime.internal.calls.ZLinkRequestCalls
                                        .remainingTimeout(
                                                timeout, started, requestNanoTime.getAsLong()));
                    }
                    throw new ZLinkConfigurationException(
                            "route mesh channel is not configured: " + channelName);
                },
                rejectedSubmission);
    }

    /** Local owner, configured bridge, and registered node keep their existing precedence. */
    <T> CompletionStage<T> submitToSpot(
            String channelName,
            RoutingId targetNodeRid,
            Supplier<ZLinkInternalSpotNode> bridgeOwner,
            Duration timeoutOverride,
            Duration defaultTimeout,
            BiFunction<ZLinkBackendSpotRouteBridge, Duration, CompletionStage<T>> bridgeSubmit,
            BiFunction<ZLinkInternalSpotNode, Duration, CompletionStage<T>> nodeSubmit) {
        long started = defaultTimeout == null ? 0 : requestNanoTime.getAsLong();
        return submitInStateLane(
                () -> {
                    Duration timeout =
                            requestTimeoutCore(channelName, timeoutOverride, defaultTimeout);
                    if (bridgeOwner != null) {
                        ZLinkInternalSpotNode localNode = bridgeOwner.get();
                        if (localNode != null && localNode.routingId().equals(targetNodeRid)) {
                            return nodeSubmit.apply(
                                    localNode,
                                    systems.zlink.framework.runtime.internal.calls.ZLinkRequestCalls
                                            .remainingTimeout(
                                                    timeout, started, requestNanoTime.getAsLong()));
                        }
                    }
                    ChannelRegistration registration = registrations.get(channelName);
                    if (registration != null && registration.kind() == ChannelKind.ROUTE_MESH) {
                        return bridgeSubmit.apply(
                                requireSpotRouteBridgeCore(channelName, bridgeOwner),
                                systems.zlink.framework.runtime.internal.calls.ZLinkRequestCalls
                                        .remainingTimeout(
                                                timeout, started, requestNanoTime.getAsLong()));
                    }
                    ZLinkInternalSpotNode node = spotRouterNodes.get(channelName);
                    if (node != null) {
                        return nodeSubmit.apply(
                                node,
                                systems.zlink.framework.runtime.internal.calls.ZLinkRequestCalls
                                        .remainingTimeout(
                                                timeout, started, requestNanoTime.getAsLong()));
                    }
                    throw new ZLinkConfigurationException(
                            "route mesh channel is not configured: " + channelName);
                });
    }

    private Duration requestTimeoutCore(
            String channelName, Duration timeoutOverride, Duration defaultTimeout) {
        if (defaultTimeout == null && timeoutOverride == null) return null;
        ChannelRegistration registration = registrations.get(channelName);
        return timeoutOverride != null
                ? timeoutOverride
                : registration != null && registration.defaultRequestTimeout() != null
                        ? registration.defaultRequestTimeout()
                        : defaultTimeout;
    }

    private <T> CompletionStage<T> submitInStateLane(Supplier<CompletionStage<T>> submission) {
        return submitInStateLane(submission, failure -> {});
    }

    private <T> CompletionStage<T> submitInStateLane(
            Supplier<CompletionStage<T>> submission,
            java.util.function.Consumer<Throwable> rejectedSubmission) {
        ZLinkFlowContext.State flow = ZLinkFlowContext.current();
        Supplier<CompletionStage<T>> work =
                () -> {
                    try (var ignored =
                            flow == null
                                    ? ZLinkFlowContext.suppress()
                                    : ZLinkFlowContext.enter(flow)) {
                        if (closing.getAsBoolean())
                            throw new ZLinkFrameworkException(
                                    ZLinkFrameworkErrorKind.SHUTTING_DOWN,
                                    "channel runtime is shutting down");
                        return submission.get();
                    } catch (RuntimeException | Error failure) {
                        rejectSubmission(flow, rejectedSubmission, failure);
                        return java.util.concurrent.CompletableFuture.failedFuture(failure);
                    }
                };
        if (stateLane.isOnLane()) return work.get();
        try {
            // Callers hold no external gate that this turn reacquires; selection and first submit
            // run here. ZLinkStateLane publishes pending completion off-lane, so continuations
            // cannot reenter its current turn. JVM submit must finish the first attempt before
            // return.
            return stateLane.runNowOrQueue(work::get).toCompletableFuture().join();
        } catch (RuntimeException | Error failure) {
            Throwable cause =
                    failure instanceof CompletionException && failure.getCause() != null
                            ? failure.getCause()
                            : failure;
            rejectSubmission(flow, rejectedSubmission, cause);
            return java.util.concurrent.CompletableFuture.failedFuture(cause);
        }
    }

    private static void rejectSubmission(
            ZLinkFlowContext.State flow,
            java.util.function.Consumer<Throwable> rejectedSubmission,
            Throwable failure) {
        // run/call preserve unrelated ambient context for null, whereas an uncaptured call must
        // suppress it.
        // 06-observability/04-flow-correlation.ko.md:101: terminal callbacks restore their
        // operation context.
        try (var ignored =
                flow == null ? ZLinkFlowContext.suppress() : ZLinkFlowContext.enter(flow)) {
            rejectedSubmission.accept(failure);
        }
    }

    ZLinkBackendSpotRouteBridge requireSpotRouteBridge(
            String channelName, Supplier<ZLinkInternalSpotNode> bridgeOwner) {
        return stateLane.isOnLane()
                ? requireSpotRouteBridgeCore(channelName, bridgeOwner)
                : inStateLane(() -> requireSpotRouteBridgeCore(channelName, bridgeOwner));
    }

    private ZLinkBackendSpotRouteBridge requireSpotRouteBridgeCore(
            String channelName, Supplier<ZLinkInternalSpotNode> bridgeOwner) {
        ZLinkBackendSpotRouteBridge existing = spotRouteBridges.get(channelName);
        if (existing != null) {
            return existing;
        }
        if (bridgeOwner == null) {
            throw new ZLinkConfigurationException(
                    "routed SPOT egress requires a router-capable SPOT node");
        }
        ChannelRegistration registration = registrations.get(channelName);
        if (registration == null || registration.kind() != ChannelKind.ROUTE_MESH) {
            throw new ZLinkConfigurationException(
                    "SPOT route bridge requires a router channel: " + channelName);
        }
        ZLinkBackendRouterSocket router = routeRouters.get(channelName);
        if (router == null) {
            throw new ZLinkConfigurationException(
                    "route mesh channel is not configured: " + channelName);
        }
        ZLinkBackendSpotRouteBridge bridge = bridgeOwner.get().createRouteBridge();
        bridge.attachRouterChannel(channelName, router);
        spotRouteBridges.put(channelName, bridge);
        return bridge;
    }

    void addClientServerConnection(
            String connectionId,
            ZLinkClientServerServerDescriptor descriptor,
            ZLinkBackendDealerSocket dealer) {
        addClientServerConnection(connectionId, descriptor, dealer, fence -> {});
    }

    void addClientServerConnection(
            String connectionId,
            ZLinkClientServerServerDescriptor descriptor,
            ZLinkBackendDealerSocket dealer,
            Consumer<AdmissionFence> restartAdmission) {
        // This method used to hold the registry monitor while adding the
        // physical DEALER. The absolute flow-state application happens before
        // that monitor is acquired so a binding call cannot block routing.
        registerReceiveFlow(dealer);
        inTopologyTurn(
                () -> {
                    clientServerConnections.put(
                            connectionId,
                            new ClientServerConnection(
                                    connectionId, descriptor, dealer, false, restartAdmission));
                    ownedSockets.add(dealer);
                    return null;
                });
    }

    void registerClientServerMonitor(String connectionId, ZLinkBackendSocketMonitor monitor) {
        ClientServerConnection current;
        current =
                inStateLane(
                        () -> {
                            ClientServerConnection registered =
                                    clientServerConnections.get(connectionId);
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
        inStateLane(
                () -> {
                    unmanagedBackendClientMode = true;
                    return null;
                });
    }

    AdmissionFence clientServerTransportReady(String connectionId) {
        return inStateLane(
                () -> {
                    ClientServerConnection current = clientServerConnections.get(connectionId);
                    return clientServerTransportReadyCore(
                            connectionId, current == null ? null : current.dealer);
                });
    }

    AdmissionFence clientServerTransportReady(
            String connectionId, ZLinkBackendDealerSocket dealer) {
        return inStateLane(() -> clientServerTransportReadyCore(connectionId, dealer));
    }

    private AdmissionFence clientServerTransportReadyCore(
            String connectionId, ZLinkBackendDealerSocket dealer) {
        ClientServerConnection current = clientServerConnections.get(connectionId);
        if (current == null || current.dealer != dealer) {
            return null;
        }
        current.physicalGeneration++;
        current.admissionGeneration++;
        current.ready = false;
        current.outstandingProbeId = 0;
        return new AdmissionFence(
                current.physicalGeneration, current.admissionGeneration, current.dealer);
    }

    boolean retryTimedOutClientServerAdmission(
            String connectionId,
            AdmissionFence fence,
            Throwable failure,
            Executor executor,
            Consumer<AdmissionFence> retry) {
        if (!(ZLinkChannelCallRuntime.unwrap(failure) instanceof ZlinkRequestException request)
                || request.getResult() != RequestResult.TIMED_OUT) {
            return false;
        }
        // The request deadline paces hello retries. Only the attempt changes;
        // Core still owns the physical connection and its reconnect policy.
        executor.execute(
                () -> {
                    AdmissionFence next =
                            inStateLane(
                                    () -> {
                                        ClientServerConnection current =
                                                clientServerConnections.get(connectionId);
                                        if (!fence.matches(current) || current.ready) return null;
                                        current.admissionGeneration++;
                                        return new AdmissionFence(
                                                current.physicalGeneration,
                                                current.admissionGeneration,
                                                current.dealer);
                                    });
                    if (next != null) retry.accept(next);
                });
        return true;
    }

    void clientServerTransportTerminated(String connectionId) {
        inTopologyTurn(
                () -> {
                    ClientServerConnection current = clientServerConnections.get(connectionId);
                    clientServerTransportTerminatedCore(
                            connectionId, current == null ? null : current.dealer);
                    return null;
                });
    }

    void clientServerTransportTerminated(String connectionId, ZLinkBackendDealerSocket dealer) {
        inTopologyTurn(
                () -> {
                    clientServerTransportTerminatedCore(connectionId, dealer);
                    return null;
                });
    }

    private void clientServerTransportTerminatedCore(
            String connectionId, ZLinkBackendDealerSocket dealer) {
        ClientServerConnection current = clientServerConnections.get(connectionId);
        if (current == null || current.dealer != dealer) {
            return;
        }
        current.physicalGeneration++;
        current.admissionGeneration++;
        current.ready = false;
        current.outstandingProbeId = 0;
    }

    void restartClientServerAdmission(String connectionId, AdmissionFence expectedFence) {
        ClientServerConnection connection =
                inStateLane(() -> clientServerConnections.get(connectionId));
        if (connection == null) {
            return;
        }
        AdmissionFence next =
                inTopologyTurn(
                        () -> {
                            if (clientServerConnections.get(connectionId) != connection
                                    || expectedFence == null
                                    || !expectedFence.matches(connection)) {
                                return null;
                            }
                            return invalidateClientServerAdmissionCore(connection);
                        });
        dispatchClientServerAdmissionRestart(connection, next);
    }

    boolean admitClientServerConnection(
            String connectionId, ZLinkClientServerServerDescriptor descriptor) {
        AdmissionFence fence =
                inStateLane(
                        () -> {
                            ClientServerConnection current =
                                    clientServerConnections.get(connectionId);
                            return current == null
                                    ? null
                                    : new AdmissionFence(
                                            current.physicalGeneration,
                                            current.admissionGeneration,
                                            current.dealer);
                        });
        return fence != null && admitClientServerConnection(connectionId, descriptor, fence);
    }

    boolean admitClientServerConnection(
            String connectionId,
            ZLinkClientServerServerDescriptor descriptor,
            AdmissionFence fence) {
        AdmissionResult result =
                inTopologyTurn(
                        () -> {
                            ClientServerConnection current =
                                    clientServerConnections.get(connectionId);
                            if (fence == null || !fence.matches(current)) {
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
                                            .filter(
                                                    other ->
                                                            other != current
                                                                    && other.ready
                                                                    && clientServerLogicalIdentity(
                                                                                    other.descriptor)
                                                                            .equals(
                                                                                    clientServerLogicalIdentity(
                                                                                            descriptor)))
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

    CompletionStage<AdmissionFence> updateClientServerConnection(
            String connectionId,
            ZLinkClientServerServerDescriptor descriptor,
            ZLinkBackendDealerSocket dealer) {
        return stateLane
                .runAsync(
                        () -> {
                            ClientServerConnection current =
                                    clientServerConnections.get(connectionId);
                            if (current == null || current.dealer != dealer) return null;
                            current.descriptor = descriptor;
                            // Only this owner decides whether the live admission remains valid.
                            return current.ready
                                    ? null
                                    : clientServerTransportReadyCore(connectionId, dealer);
                        })
                .whenComplete((fence, failure) -> signalTopologyChanged());
    }

    ZLinkClientServerServerDescriptor clientServerConnectionDescriptor(String connectionId) {
        return inStateLane(
                () -> {
                    ClientServerConnection current = clientServerConnections.get(connectionId);
                    return current == null ? null : current.descriptor;
                });
    }

    void removeClientServerConnection(String connectionId) {
        removeClientServerConnection(connectionId, null);
    }

    void removeClientServerConnection(
            String connectionId, ZLinkBackendDealerSocket expectedDealer) {
        Removal removal =
                inTopologyTurn(
                        () -> removeClientServerConnectionCore(connectionId, expectedDealer));
        if (removal != null && removal.closePhysical())
            closeClientServerPhysical(removal.connection());
    }

    CompletionStage<Boolean> retireClientServerConnection(
            String connectionId, ZLinkBackendDealerSocket dealer, Set<String> replacements) {
        return stateLane
                .runAsync(
                        () -> {
                            for (String replacement : replacements) {
                                ClientServerConnection candidate =
                                        clientServerConnections.get(replacement);
                                if (candidate != null && !candidate.ready) return null;
                            }
                            return java.util.Optional.ofNullable(
                                    removeClientServerConnectionCore(connectionId, dealer));
                        })
                .thenApply(
                        removal -> {
                            if (removal == null) return false;
                            if (removal.isPresent() && removal.get().closePhysical()) {
                                closeClientServerPhysical(removal.get().connection());
                            }
                            signalTopologyChanged();
                            return true;
                        });
    }

    private Removal removeClientServerConnectionCore(
            String connectionId, ZLinkBackendDealerSocket expectedDealer) {
        ClientServerConnection registered = clientServerConnections.get(connectionId);
        if (registered == null || (expectedDealer != null && registered.dealer != expectedDealer))
            return null;
        ClientServerConnection removed = clientServerConnections.remove(connectionId);
        removed.aliases.remove(connectionId);
        return new Removal(removed, removed.aliases.isEmpty());
    }

    int clientServerPhysicalConnectionCount() {
        return inStateLane(
                () -> {
                    Set<ClientServerConnection> physical =
                            Collections.newSetFromMap(new IdentityHashMap<>());
                    physical.addAll(clientServerConnections.values());
                    return physical.size();
                });
    }

    boolean hasUnavailableClientServerConnection(String channelName) {
        return inStateLane(() -> hasUnavailableClientServerConnectionCore(channelName));
    }

    private boolean hasUnavailableClientServerConnectionCore(String channelName) {
        return clientServerConnections.values().stream()
                .anyMatch(
                        connection ->
                                connection.descriptor().channelName().equals(channelName)
                                        && !connection.ready());
    }

    List<ClientServerTargetSnapshot> clientServerTargetSnapshots(String channelName) {
        return inStateLane(() -> clientServerTargetSnapshotsCore(channelName));
    }

    private List<ClientServerTargetSnapshot> clientServerTargetSnapshotsCore(String channelName) {
        Map<String, ClientServerTargetSnapshot> targets = new LinkedHashMap<>();
        Set<ClientServerConnection> physical = Collections.newSetFromMap(new IdentityHashMap<>());
        physical.addAll(clientServerConnections.values());
        for (ClientServerConnection connection : physical) {
            ZLinkClientServerServerDescriptor descriptor = connection.descriptor();
            if (!descriptor.channelName().equals(channelName)) {
                continue;
            }
            targets.put(
                    clientServerLogicalIdentity(descriptor),
                    new ClientServerTargetSnapshot(
                            descriptor,
                            descriptor.weight(),
                            connection.ready(),
                            !connection.physicalClosed));
        }
        ZLinkClientServerServerDescriptor local = clientServerServerDescriptors.get(channelName);
        if (local != null) {
            // Local descriptors exist only after listener setup. The local
            // server remains a target independently of a Client self-connection.
            targets.put(
                    clientServerLogicalIdentity(local),
                    new ClientServerTargetSnapshot(local, local.weight(), true, false));
        }
        return List.copyOf(targets.values());
    }

    boolean hasClientRegistration(String channelName) {
        return inStateLane(
                () -> {
                    ChannelRegistration registration = registrations.get(channelName);
                    return registration != null
                            && registration.kind() == ChannelKind.CLIENT_SERVER
                            && registration.clientEnabled();
                });
    }

    boolean hasServerRegistration(String channelName) {
        return inStateLane(
                () -> {
                    ChannelRegistration registration = registrations.get(channelName);
                    return registration != null
                            && registration.kind() == ChannelKind.CLIENT_SERVER
                            && registration.clientServerServerEnabled();
                });
    }

    void setClientServerServerDescriptor(
            String channelName, ZLinkClientServerServerDescriptor descriptor) {
        inTopologyTurn(
                () -> {
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

    ZLinkClientServerServerDescriptor clientServerServerDescriptor(String channelName) {
        return inStateLane(() -> clientServerServerDescriptors.get(channelName));
    }

    int clientServerServerWeight(String channelName, int fallback) {
        return inStateLane(
                () -> {
                    ChannelRegistration registration = registrations.get(channelName);
                    return registration == null || !registration.clientServerServerEnabled()
                            ? fallback
                            : registration.serverSocketOptions().weight();
                });
    }

    void setClientServerServerWeight(String channelName, int weight) {
        ZLinkClientServerServerDescriptor changed =
                inTopologyTurn(
                        () -> {
                            ChannelRegistration registration = registrations.get(channelName);
                            if (registration == null || !registration.clientServerServerEnabled()) {
                                throw new ZLinkConfigurationException(
                                        "client/server channel has no server: " + channelName);
                            }
                            registration.serverSocketOptions().weight(weight);
                            ZLinkClientServerServerDescriptor current =
                                    clientServerServerDescriptors.get(channelName);
                            if (current == null || current.weight() == weight) {
                                return null;
                            }
                            ZLinkClientServerServerDescriptor updated =
                                    new ZLinkClientServerServerDescriptor(
                                            current.channelName(),
                                            current.serverRid(),
                                            current.lifecycleGeneration(),
                                            current.descriptorRevision() + 1,
                                            current.endpoint(),
                                            weight,
                                            current.state(),
                                            current.securityIdentity(),
                                            current.ownerId(),
                                            current.leaseGeneration(),
                                            Instant.now());
                            clientServerServerDescriptors.put(channelName, updated);
                            return updated;
                        });
        if (changed != null) {
            pushClientServerDescriptorUpdate(channelName, changed);
        }
    }

    void initializeClientServerServerDescriptors(String ownerId) {
        List<ServerDescriptorInput> inputs =
                inStateLane(
                        () ->
                                servers.entrySet().stream()
                                        .map(
                                                entry ->
                                                        new ServerDescriptorInput(
                                                                entry.getKey(),
                                                                listenerEndpoints.get(
                                                                        new ListenerKey(
                                                                                ZLinkListenerKind
                                                                                        .CLIENT_SERVER,
                                                                                entry.getKey())),
                                                                serverRoutingIds.get(
                                                                        entry.getKey()),
                                                                registrations.get(entry.getKey())))
                                        .toList());
        List<ServerDescriptorValue> descriptors = new ArrayList<>();
        for (ServerDescriptorInput input : inputs) {
            ChannelRegistration registration = input.registration();
            String endpoint = input.endpoint();
            if (registration == null || endpoint == null) {
                continue;
            }
            descriptors.add(
                    new ServerDescriptorValue(
                            input.channelName(),
                            new ZLinkClientServerServerDescriptor(
                                    input.channelName(),
                                    input.routingId(),
                                    ThreadLocalRandom.current().nextLong(1, Long.MAX_VALUE),
                                    1,
                                    endpoint,
                                    registration.serverSocketOptions().weight(),
                                    systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState
                                            .SERVING,
                                    "default",
                                    ownerId,
                                    1,
                                    Instant.EPOCH)));
        }
        inTopologyTurn(
                () -> {
                    for (ServerDescriptorValue descriptor : descriptors) {
                        clientServerServerDescriptors.put(
                                descriptor.channelName(), descriptor.descriptor());
                    }
                    return null;
                });
    }

    boolean tryHandleClientServerControl(
            String channelName, ZLinkBackendRouterSocket router, ZLinkBackendReceived received) {
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
                acceptClientServerServerAck(channelName, received.routingId().get(), ack.probeId());
                received.close();
                return true;
            }
            if (control instanceof ZLinkClientServerServiceWire.LivenessProbe probe
                    && received.routingId().isPresent()) {
                byte[] ack = ZLinkClientServerServiceWire.encodeLivenessAck(probe.probeId());
                if (received.isRequest()) {
                    reply = ack;
                } else {
                    try (Message message = Message.from(ack)) {
                        router.send(received.routingId().get(), List.of(message));
                    }
                    received.close();
                    return true;
                }
            } else {
                ZLinkClientServerServerDescriptor descriptor =
                        inStateLane(() -> clientServerServerDescriptors.get(channelName));
                if (!(control instanceof ZLinkClientServerServiceWire.Hello hello)
                        || descriptor == null
                        || !hello.channelName().equals(channelName)
                        || !hello.securityIdentity().equals(descriptor.securityIdentity())) {
                    reply = ZLinkClientServerServiceWire.encodeReject(2);
                } else {
                    reply =
                            ZLinkClientServerServiceWire.encodeAdmit(
                                    descriptor, normalizedMessageLimit(router.maxMessageSize()));
                    if (received.routingId().isPresent()) {
                        admitClientServerServerPeer(
                                channelName, received.routingId().get(), router);
                    }
                }
            }
        } catch (RuntimeException failure) {
            if (received.isRequest()) {
                reply = ZLinkClientServerServiceWire.encodeReject(1);
            }
        }
        if (reply != null && received.isRequest()) {
            ZLinkChannelDispatchReporter.replyAndClose(router, received, Message.from(reply));
        } else if (reply != null && received.routingId().isPresent()) {
            try {
                router.disconnectPeer(received.routingId().get());
            } catch (RuntimeException failure) {
                LOGGER.log(Level.WARNING, "rejected ClientServer peer disconnect failed", failure);
            }
        }
        received.close();
        return true;
    }

    void tickClientServerLiveness(long nowNanos) {
        LivenessSnapshot snapshot =
                inStateLane(
                        () -> {
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
        clientConnections.sort(Comparator.comparing(ClientServerConnection::connectionId));
        int connectionCount = clientConnections.size();
        int connectionStart =
                connectionCount == 0
                        ? 0
                        : (int) Math.floorMod(snapshot.controlCursor(), connectionCount);
        for (int offset = 0; offset < connectionCount; offset++) {
            ClientServerConnection connection =
                    clientConnections.get((connectionStart + offset) % connectionCount);
            int nextCursor = (connectionStart + offset + 1) % connectionCount;
            inStateLane(
                    () -> {
                        clientServerControlCursor = nextCursor;
                        return null;
                    });
            flushClientServerLivenessAck(connection);
            ClientLivenessAction action =
                    inStateLane(
                            () -> {
                                if (!connection.aliases.stream()
                                                .anyMatch(
                                                        alias ->
                                                                clientServerConnections.get(alias)
                                                                        == connection)
                                        || !connection.ready) {
                                    return ClientLivenessAction.NONE;
                                }
                                if (nowNanos >= connection.deadlineAtNanos) {
                                    return new ClientLivenessAction(
                                            invalidateClientServerAdmissionCore(connection), 0);
                                } else if (nowNanos >= connection.nextProbeAtNanos) {
                                    connection.nextProbeAtNanos =
                                            nowNanos + CLIENT_SERVER_PROBE_INTERVAL_NANOS;
                                    long probeId =
                                            connection.outstandingProbeId == 0
                                                    ? allocateProbeIdCore()
                                                    : connection.outstandingProbeId;
                                    connection.outstandingProbeId = probeId;
                                    return new ClientLivenessAction(null, probeId);
                                }
                                return ClientLivenessAction.NONE;
                            });
            if (action.restartFence() != null) {
                signalTopologyChanged();
                dispatchClientServerAdmissionRestart(connection, action.restartFence());
                continue;
            }
            if (action.probeId() == 0) {
                continue;
            }
            sendClientServerProbe(connection, action.probeId());
        }
        for (ClientServerServerPeer peer : serverPeers) {
            ServerPeerLivenessAction action =
                    inStateLane(
                            () -> {
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
                                    long probeId =
                                            current.outstandingProbeId == 0
                                                    ? allocateProbeIdCore()
                                                    : current.outstandingProbeId;
                                    current.outstandingProbeId = probeId;
                                    return new ServerPeerLivenessAction(false, probeId);
                                } else {
                                    return ServerPeerLivenessAction.NONE;
                                }
                            });
            if (action.disconnect()) {
                signalTopologyChanged();
                try {
                    peer.router.disconnectPeer(peer.routingId);
                } catch (RuntimeException failure) {
                    LOGGER.log(
                            Level.WARNING, "expired ClientServer peer disconnect failed", failure);
                }
                continue;
            }
            if (action.probeId() == 0) {
                continue;
            }
            try (Message message =
                    Message.from(
                            ZLinkClientServerServiceWire.encodeLivenessProbe(action.probeId()))) {
                try {
                    peer.router.send(peer.routingId, List.of(message));
                } catch (ZlinkSubmitException ignored) {
                    // The peer may lose admission between the liveness
                    // schedule check and the non-blocking send. Its existing
                    // deadline and connection lifecycle perform cleanup.
                }
            }
        }
    }

    void reportClientServerReceiveFailuresTo(BiConsumer<String, Throwable> sink) {
        clientServerReceiveFailures = java.util.Objects.requireNonNull(sink, "sink");
    }

    /**
     * Starts the one receive owner of a ClientServer DEALER. Its poller wait is also where the
     * binding completes the DEALER's requests (the public {@code POLLCOMPLETION} owner), so a reply
     * completes as soon as Core delivers it. The owner is the only poller waiter of the DEALER and
     * closes it when it ends.
     */
    void startClientServerControlReceive(String connectionId) {
        inStateLane(
                () -> {
                    ClientServerConnection connection = clientServerConnections.get(connectionId);
                    if (connection != null
                            && !connection.physicalClosed
                            && connection.receiveOwner == null) {
                        // Started inside the lane: a close that reads this owner finds it
                        // running or ended, never not yet started.
                        connection.receiveOwner =
                                Thread.ofPlatform()
                                        .daemon()
                                        .name("zlink-client-server-control")
                                        .start(() -> runClientServerControlReceive(connection));
                    }
                    return null;
                });
    }

    private void runClientServerControlReceive(ClientServerConnection connection) {
        try {
            while (!connection.receiveStopped) {
                try {
                    receiveClientServerControls(connection, CLIENT_SERVER_CONTROL_RECEIVE_WAIT);
                } catch (RuntimeException failure) {
                    clientServerReceiveFailures.accept(
                            connection.descriptor.channelName(), failure);
                }
            }
        } finally {
            try {
                connection.dealer.close();
            } catch (RuntimeException failure) {
                LOGGER.log(Level.WARNING, "ClientServer dealer cleanup failed", failure);
            }
        }
    }

    /** Receives the controls the connection's DEALER has ready now, without a poller wait. */
    void receiveClientServerControls(String connectionId) {
        ClientServerConnection connection =
                inStateLane(() -> clientServerConnections.get(connectionId));
        if (connection == null) {
            return;
        }
        try {
            receiveClientServerControls(connection, Duration.ZERO);
        } catch (RuntimeException failure) {
            clientServerReceiveFailures.accept(connection.descriptor.channelName(), failure);
        }
    }

    private void receiveClientServerControls(ClientServerConnection connection, Duration wait) {
        if (connection.dealer.waitForReadable(wait)) {
            drainClientServerControls(connection);
        }
    }

    private void drainClientServerControls(ClientServerConnection connection) {
        ZLinkReceiveBatchBudget batch = new ZLinkReceiveBatchBudget();
        while (batch.canReceiveNext()) {
            if (!connection.dealer.waitForReadable(Duration.ZERO)) {
                return;
            }
            ZLinkBackendReceived received = connection.dealer.recv(ZLinkBackendRecvMode.DONT_WAIT);
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
                        ZLinkClientServerServiceWire.decode(received.parts().get(0).toByteArray());
                if (control instanceof ZLinkClientServerServiceWire.LivenessProbe probe) {
                    inStateLane(
                            () -> {
                                connection.pendingLivenessAckId = probe.probeId();
                                return null;
                            });
                    try (Message ack =
                            Message.from(
                                    ZLinkClientServerServiceWire.encodeLivenessAck(
                                            probe.probeId()))) {
                        try {
                            connection
                                    .dealer
                                    .send(List.of(ack))
                                    .whenComplete(
                                            (ignored, failure) -> {
                                                if (failure == null) {
                                                    inStateLane(
                                                            () -> {
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
                } else if (control instanceof ZLinkClientServerServiceWire.Update update) {
                    applyClientServerUpdate(connection, update.admission());
                } else if (control instanceof ZLinkClientServerServiceWire.LivenessAck ack) {
                    acceptClientServerClientAck(connection, ack.probeId());
                } else {
                    terminateClientServerProtocol(connection);
                }
            } catch (RuntimeException failure) {
                terminateClientServerProtocol(connection);
            }
        }
    }

    private void flushClientServerLivenessAck(ClientServerConnection connection) {
        long probeId = inStateLane(() -> connection.pendingLivenessAckId);
        if (probeId == 0) {
            return;
        }
        try (Message ack = Message.from(ZLinkClientServerServiceWire.encodeLivenessAck(probeId))) {
            connection
                    .dealer
                    .send(List.of(ack))
                    .whenComplete(
                            (ignored, failure) -> {
                                if (failure == null) {
                                    inStateLane(
                                            () -> {
                                                connection.pendingLivenessAckId = 0;
                                                return null;
                                            });
                                }
                            });
        } catch (ZlinkSubmitException ignored) {
        }
    }

    private void sendClientServerProbe(ClientServerConnection connection, long probeId) {
        try (Message message =
                Message.from(ZLinkClientServerServiceWire.encodeLivenessProbe(probeId))) {
            connection.dealer.send(List.of(message));
        } catch (ZlinkSubmitException ignored) {
            // The same outstanding probe ID is retried on the next interval.
            // A transient send-admission race is not a connection result.
        }
    }

    private void acceptClientServerClientAck(ClientServerConnection connection, long probeId) {
        inStateLane(
                () -> {
                    if (!connection.aliases.stream()
                                    .anyMatch(
                                            alias ->
                                                    clientServerConnections.get(alias)
                                                            == connection)
                            || connection.outstandingProbeId != probeId) {
                        return null;
                    }
                    connection.outstandingProbeId = 0;
                    connection.deadlineAtNanos = System.nanoTime() + CLIENT_SERVER_DEADLINE_NANOS;
                    return null;
                });
    }

    private void applyClientServerUpdate(
            ClientServerConnection connection, ZLinkClientServerServiceWire.Admission update) {
        AdmissionFence restart =
                inStateLane(
                        () -> {
                            if (!connection.aliases.stream()
                                            .anyMatch(
                                                    alias ->
                                                            clientServerConnections.get(alias)
                                                                    == connection)
                                    || !connection.ready) {
                                return null;
                            }
                            ZLinkClientServerServerDescriptor before = connection.descriptor;
                            if (!update.channelName().equals(before.channelName())
                                    || !update.serverRid().equals(before.serverRid())
                                    || update.lifecycleGeneration() != before.lifecycleGeneration()
                                    || !update.securityIdentity().equals(before.securityIdentity())
                                    || !update.advertisedEndpoint().equals(before.endpoint())) {
                                return invalidateClientServerAdmissionCore(connection);
                            } else if (update.descriptorRevision() >= before.descriptorRevision()) {
                                ZLinkClientServerServerDescriptor candidate =
                                        descriptorFromAdmission(update, before);
                                if (update.descriptorRevision() == before.descriptorRevision()) {
                                    if (!sameClientServerDescriptor(candidate, before)) {
                                        return invalidateClientServerAdmissionCore(connection);
                                    }
                                } else {
                                    connection.descriptor = candidate;
                                }
                            }
                            return null;
                        });
        dispatchClientServerAdmissionRestart(connection, restart);
    }

    private void terminateClientServerProtocol(ClientServerConnection connection) {
        AdmissionFence restart =
                inStateLane(
                        () -> {
                            if (!connection.ready
                                    || !connection.aliases.stream()
                                            .anyMatch(
                                                    alias ->
                                                            clientServerConnections.get(alias)
                                                                    == connection)) {
                                return null;
                            }
                            return invalidateClientServerAdmissionCore(connection);
                        });
        dispatchClientServerAdmissionRestart(connection, restart);
    }

    // Transport liveness spec 55 section 6: terminal cleanup never leaves a
    // monitor subscription behind its connection, so the monitor closes before
    // the DEALER it observes.
    private void closeClientServerPhysical(ClientServerConnection connection) {
        ZLinkBackendSocketMonitor monitor;
        ZLinkApplicationJobReceiveFlowController.Registration receiveFlow;
        ClientServerClose close =
                inStateLane(
                        () -> {
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
                                    receiveFlowRegistrations.remove(connection.dealer),
                                    connection.receiveOwner);
                        });
        if (close == null) {
            return;
        }
        monitor = close.monitor();
        receiveFlow = close.receiveFlow();
        closeReceiveFlowRegistration(receiveFlow);
        if (monitor != null) {
            try {
                monitor.close();
            } catch (RuntimeException failure) {
                LOGGER.log(Level.WARNING, "ClientServer monitor cleanup failed", failure);
            }
        }
        connection.receiveStopped = true;
        Thread owner = close.receiveOwner();
        if (owner == null) {
            try {
                connection.dealer.close();
            } catch (RuntimeException failure) {
                LOGGER.log(Level.WARNING, "ClientServer dealer cleanup failed", failure);
            }
        } else if (owner != Thread.currentThread()) {
            // The receive owner closes the DEALER when it ends. A close that runs on the owner
            // itself (in a completion it dispatched) returns here, and the owner closes the
            // DEALER once that completion returns and it sees receiveStopped.
            awaitReceiveOwner(owner);
        }
    }

    private static void awaitReceiveOwner(Thread owner) {
        boolean interrupted = false;
        while (owner.isAlive()) {
            try {
                owner.join();
            } catch (InterruptedException interruption) {
                interrupted = true;
            }
        }
        if (interrupted) {
            Thread.currentThread().interrupt();
        }
    }

    private AdmissionFence invalidateClientServerAdmissionCore(ClientServerConnection connection) {
        if (!connection.aliases.stream()
                .anyMatch(alias -> clientServerConnections.get(alias) == connection)) {
            return null;
        }
        connection.admissionGeneration++;
        connection.ready = false;
        connection.outstandingProbeId = 0;
        connection.pendingLivenessAckId = 0;
        return new AdmissionFence(
                connection.physicalGeneration, connection.admissionGeneration, connection.dealer);
    }

    private void dispatchClientServerAdmissionRestart(
            ClientServerConnection connection, AdmissionFence fence) {
        if (fence != null) {
            connection.restartAdmission.accept(fence);
        }
    }

    private void admitClientServerServerPeer(
            String channelName, RoutingId routingId, ZLinkBackendRouterSocket router) {
        inStateLane(
                () -> {
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
            String channelName, RoutingId routingId, long probeId) {
        inStateLane(
                () -> {
                    ClientServerServerPeer peer =
                            clientServerServerPeers.get(serverPeerKey(channelName, routingId));
                    if (peer == null || peer.outstandingProbeId != probeId) {
                        return null;
                    }
                    peer.outstandingProbeId = 0;
                    peer.deadlineAtNanos = System.nanoTime() + CLIENT_SERVER_DEADLINE_NANOS;
                    return null;
                });
    }

    private void pushClientServerDescriptorUpdate(
            String channelName, ZLinkClientServerServerDescriptor descriptor) {
        List<ClientServerServerPeer> peers;
        peers = inStateLane(() -> List.copyOf(clientServerServerPeers.values()));
        for (ClientServerServerPeer peer : peers) {
            if (!peer.channelName.equals(channelName)) {
                continue;
            }
            try (Message message =
                    Message.from(
                            ZLinkClientServerServiceWire.encodeUpdate(
                                    descriptor,
                                    normalizedMessageLimit(peer.router.maxMessageSize())))) {
                try {
                    peer.router.send(peer.routingId, List.of(message));
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
        nextClientServerProbeId = result == Long.MAX_VALUE ? 1 : result + 1;
        return result;
    }

    ZLinkBackendRouterSocket server(String channelName) {
        return inStateLane(() -> servers.get(channelName));
    }

    ZLinkBackendPublisherSocket publisher(String channelName) {
        return inStateLane(() -> publishers.get(channelName));
    }

    void recordListener(ZLinkListenerKind kind, String name, String endpoint) {
        inStateLane(
                () -> {
                    listenerEndpoints.put(new ListenerKey(kind, name), endpoint);
                    return null;
                });
    }

    void clearListenerRecords() {
        inStateLane(
                () -> {
                    listenerEndpoints.clear();
                    return null;
                });
    }

    String listenerEndpoint(ZLinkListenerKind kind, String name) {
        String endpoint = inStateLane(() -> listenerEndpoints.get(new ListenerKey(kind, name)));
        if (endpoint == null) {
            throw new ZLinkConfigurationException(kind + " listener is not bound: " + name);
        }
        return endpoint;
    }

    ZLinkBackendSubscriberSocket subscriber(String channelName) {
        return inStateLane(() -> subscribers.get(channelName));
    }

    ZLinkBackendRouterSocket routeRouter(String channelName) {
        return inStateLane(() -> routeRouters.get(channelName));
    }

    Object routeSocketLock(String channelName, Object fallback) {
        return inStateLane(() -> routeSocketLocks.getOrDefault(channelName, fallback));
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
        inStateLane(
                () -> {
                    spotRouterNodes.put(channelName, node);
                    return null;
                });
    }

    ZLinkInternalSpotNode spotRouterNode(String channelName) {
        return inStateLane(() -> spotRouterNodes.get(channelName));
    }

    /** Returns the configured MeshName without adding a state-lane turn. */
    String requestMetricMeshName(String channelName) {
        ZLinkInternalSpotNode node = spotRouterNodes.get(channelName);
        if (node != null) {
            return node.name();
        }
        ChannelRegistration registration = registrations.get(channelName);
        return registration != null
                        && (registration.kind() == ChannelKind.CLIENT_SERVER
                                || registration.kind() == ChannelKind.ROUTE_MESH)
                ? registration.name()
                : null;
    }

    Map<String, ZLinkBackendSocket> monitoringSocketSources() {
        return inStateLane(
                () -> {
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
        AutoConnectSnapshot snapshot =
                inStateLane(
                        () ->
                                new AutoConnectSnapshot(
                                        List.copyOf(registrations.values()),
                                        Map.copyOf(clients),
                                        Map.copyOf(servers),
                                        Map.copyOf(serverRoutingIds),
                                        Map.copyOf(publishers),
                                        Map.copyOf(publisherRoutingIds),
                                        Map.copyOf(routeRouters),
                                        Map.copyOf(listenerEndpoints)));
        List<ZLinkChannelRuntime.AutoConnectSurface> surfaces = new ArrayList<>();
        for (ChannelRegistration channel : snapshot.registrations()) {
            addAutoConnectSurfaces(channel, surfaces, snapshot);
        }
        return List.copyOf(surfaces);
    }

    void closeSpotRouteBridges() {
        List<ZLinkBackendSpotRouteBridge> bridges = List.copyOf(spotRouteBridges.values());
        spotRouteBridges.clear();
        closeAll(bridges, Collections.newSetFromMap(new IdentityHashMap<>()));
    }

    void closeAll() {
        rejectPendingAdmission();
        List<ChannelRegistration> registrationsToDetach =
                inStateLane(() -> List.copyOf(registrations.values()));
        for (ChannelRegistration registration : registrationsToDetach) {
            registration.detachRuntimeConnections();
        }
        // ClientServer physical connections close first so that spec 55
        // section 6 holds here too: the admission monitor never outlives the
        // DEALER it observes. Draining ownedSockets in insertion order would
        // close the DEALER before its monitor.
        Set<ClientServerConnection> physical = Collections.newSetFromMap(new IdentityHashMap<>());
        physical.addAll(
                inStateLane(
                        () -> {
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
        List<ZLinkBackendObject> owned =
                inStateLane(
                        () -> {
                            // closeClientServerPhysical removes its monitor and DEALER from
                            // the owned set. Snapshot only after that ordered close so the
                            // generic drain cannot close either physical object a second time.
                            List<ZLinkBackendObject> snapshot = List.copyOf(ownedSockets);
                            ownedSockets.clear();
                            return snapshot;
                        });
        owned.forEach(this::deregisterReceiveFlow);
        closeAll(owned, Collections.newSetFromMap(new IdentityHashMap<>()));
    }

    private void registerReceiveFlow(ZLinkBackendDealerSocket socket) {
        if (applicationJobQueue == null) {
            return;
        }
        ZLinkApplicationJobReceiveFlowController.Registration registration =
                applicationJobQueue.registerReceiveFlowTarget(socket::setReceiveFlowState);
        ZLinkApplicationJobReceiveFlowController.Registration previous;
        previous = inStateLane(() -> receiveFlowRegistrations.put(socket, registration));
        closeReceiveFlowRegistration(previous);
    }

    private void registerReceiveFlow(ZLinkBackendRouterSocket socket) {
        if (applicationJobQueue == null) {
            return;
        }
        ZLinkApplicationJobReceiveFlowController.Registration registration =
                applicationJobQueue.registerReceiveFlowTarget(socket::setReceiveFlowState);
        ZLinkApplicationJobReceiveFlowController.Registration previous;
        previous = inStateLane(() -> receiveFlowRegistrations.put(socket, registration));
        closeReceiveFlowRegistration(previous);
    }

    private void deregisterReceiveFlow(ZLinkBackendObject socket) {
        ZLinkApplicationJobReceiveFlowController.Registration registration;
        registration = inStateLane(() -> receiveFlowRegistrations.remove(socket));
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
        String serverEndpoint =
                snapshot.listenerEndpoints()
                        .get(new ListenerKey(ZLinkListenerKind.CLIENT_SERVER, channel.name()));
        if (server != null && serverEndpoint != null) {
            surfaces.add(
                    new ZLinkChannelRuntime.AutoConnectSurface(
                            ZLinkAutoConnectType.CLIENT_SERVER,
                            channel.name(),
                            ZLinkLocationRole.ROUTER,
                            snapshot.serverRoutingIds().get(channel.name()),
                            serverEndpoint,
                            server.peerWeight(),
                            null,
                            List.of()));
        }
        if (channel.clientEnabled()) {
            surfaces.add(
                    new ZLinkChannelRuntime.AutoConnectSurface(
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
        String publisherEndpoint =
                snapshot.listenerEndpoints()
                        .get(new ListenerKey(ZLinkListenerKind.FANOUT, channel.name()));
        if (snapshot.publishers().containsKey(channel.name()) && publisherEndpoint != null) {
            surfaces.add(
                    new ZLinkChannelRuntime.AutoConnectSurface(
                            ZLinkAutoConnectType.FANOUT,
                            channel.name(),
                            ZLinkLocationRole.PUB,
                            snapshot.publisherRoutingIds().get(channel.name()),
                            publisherEndpoint,
                            100,
                            null,
                            List.of()));
        }
        if (channel.automaticSubscriberEnabled()) {
            surfaces.add(
                    new ZLinkChannelRuntime.AutoConnectSurface(
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
            surfaces.add(
                    new ZLinkChannelRuntime.AutoConnectSurface(
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
            surfaces.add(
                    new ZLinkChannelRuntime.AutoConnectSurface(
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
            String configuredEndpoint, ZLinkBackendRouterSocket router) {
        return advertisedEndpoint(configuredEndpoint, router, null);
    }

    static String advertisedEndpoint(
            String configuredEndpoint, ZLinkBackendRouterSocket router, String advertiseHost) {
        String endpoint = configuredEndpoint;
        if (!configuredEndpoint.endsWith(":0")) {
            endpoint = configuredEndpoint;
        } else {
            String boundEndpoint = router.lastEndpoint();
            endpoint =
                    boundEndpoint == null || boundEndpoint.isBlank()
                            ? configuredEndpoint
                            : boundEndpoint;
        }
        return ZLinkListenerIdentity.advertisedEndpoint(endpoint, advertiseHost);
    }

    static String advertisedEndpoint(
            String configuredEndpoint,
            ZLinkBackendPublisherSocket publisher,
            String advertiseHost) {
        String endpoint;
        if (!configuredEndpoint.endsWith(":0")) {
            endpoint = configuredEndpoint;
        } else {
            String boundEndpoint = publisher.lastEndpoint();
            endpoint =
                    boundEndpoint == null || boundEndpoint.isBlank()
                            ? configuredEndpoint
                            : boundEndpoint;
        }
        return ZLinkListenerIdentity.advertisedEndpoint(endpoint, advertiseHost);
    }

    private static void closeAll(
            Iterable<? extends ZLinkBackendObject> closeables, Set<ZLinkBackendObject> closed) {
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
            long physicalGeneration, long admissionGeneration, ZLinkBackendDealerSocket dealer) {
        private boolean matches(ClientServerConnection current) {
            return current != null
                    && current.dealer == dealer
                    && current.physicalGeneration == physicalGeneration
                    && current.admissionGeneration == admissionGeneration;
        }
    }

    record ClientServerTargetSnapshot(
            RoutingId nodeRid,
            int weight,
            ZLinkFrameworkRuntimeState hostState,
            boolean ready,
            boolean connecting) {
        ClientServerTargetSnapshot(
                ZLinkClientServerServerDescriptor descriptor,
                int weight,
                boolean connectionReady,
                boolean connecting) {
            this(
                    descriptor.serverRid(),
                    weight,
                    descriptor.state(),
                    connectionReady && descriptor.state() == ZLinkFrameworkRuntimeState.SERVING,
                    connecting);
        }
    }

    private record AdmissionResult(boolean admitted, ClientServerConnection closeAfter) {}

    private record Removal(ClientServerConnection connection, boolean closePhysical) {}

    private record LivenessSnapshot(
            List<ClientServerConnection> clientConnections,
            List<ClientServerServerPeer> serverPeers,
            long controlCursor) {}

    private record ClientLivenessAction(AdmissionFence restartFence, long probeId) {
        private static final ClientLivenessAction NONE = new ClientLivenessAction(null, 0);
    }

    private record ServerPeerLivenessAction(boolean disconnect, long probeId) {
        private static final ServerPeerLivenessAction NONE = new ServerPeerLivenessAction(false, 0);
        private static final ServerPeerLivenessAction DISCONNECT =
                new ServerPeerLivenessAction(true, 0);
    }

    private record ClientServerClose(
            ZLinkBackendSocketMonitor monitor,
            ZLinkApplicationJobReceiveFlowController.Registration receiveFlow,
            Thread receiveOwner) {}

    private record ListenerKey(ZLinkListenerKind kind, String name) {}

    private record ServerDescriptorInput(
            String channelName,
            String endpoint,
            RoutingId routingId,
            ChannelRegistration registration) {}

    private record ServerDescriptorValue(
            String channelName, ZLinkClientServerServerDescriptor descriptor) {}

    private record AutoConnectSnapshot(
            List<ChannelRegistration> registrations,
            Map<String, ZLinkBackendDealerSocket> clients,
            Map<String, ZLinkBackendRouterSocket> servers,
            Map<String, RoutingId> serverRoutingIds,
            Map<String, ZLinkBackendPublisherSocket> publishers,
            Map<String, RoutingId> publisherRoutingIds,
            Map<String, ZLinkBackendRouterSocket> routeRouters,
            Map<ListenerKey, String> listenerEndpoints) {}

    private static final class ClientServerConnection {
        private final String connectionId;
        private final Set<String> aliases = new HashSet<>();
        private ZLinkClientServerServerDescriptor descriptor;
        private final ZLinkBackendDealerSocket dealer;
        private final Consumer<AdmissionFence> restartAdmission;
        private ZLinkBackendSocketMonitor monitor;
        private boolean ready;
        private boolean physicalClosed;
        // Set by the close path after the monitor closed (spec 55 section 6: the monitor never
        // outlives the DEALER it observes). The receive owner stops on it and closes the DEALER.
        private volatile boolean receiveStopped;
        // Set once in the state lane; the only poller waiter of the DEALER, and its closer.
        private Thread receiveOwner;
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
                boolean ready,
                Consumer<AdmissionFence> restartAdmission) {
            this.connectionId = connectionId;
            this.descriptor = descriptor;
            this.dealer = dealer;
            this.restartAdmission = restartAdmission;
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

    private static String serverPeerKey(String channelName, RoutingId routingId) {
        return channelName + '\0' + routingId.toHex();
    }

    private static String clientServerLogicalIdentity(
            ZLinkClientServerServerDescriptor descriptor) {
        return descriptor.channelName()
                + '\0'
                + descriptor.serverRid().toHex()
                + '\0'
                + descriptor.lifecycleGeneration();
    }

    private static ZLinkClientServerServerDescriptor descriptorFromAdmission(
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
            ZLinkClientServerServerDescriptor left, ZLinkClientServerServerDescriptor right) {
        return left.channelName().equals(right.channelName())
                && left.serverRid().equals(right.serverRid())
                && left.lifecycleGeneration() == right.lifecycleGeneration()
                && left.descriptorRevision() == right.descriptorRevision()
                && left.endpoint().equals(right.endpoint())
                && left.weight() == right.weight()
                && left.state() == right.state()
                && left.securityIdentity().equals(right.securityIdentity());
    }
}
