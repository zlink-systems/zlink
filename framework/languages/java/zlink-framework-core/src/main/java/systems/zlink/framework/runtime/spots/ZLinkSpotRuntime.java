package systems.zlink.framework.runtime.spots;

import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;

import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;

import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerState;

import systems.zlink.framework.runtime.internal.backend.*;

import java.lang.reflect.Method;
import java.lang.reflect.InvocationTargetException;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.ArrayList;
import java.util.EnumSet;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CompletionException;
import java.util.concurrent.Executor;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.Supplier;
import java.util.logging.Logger;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkCloseException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.channels.ZLinkPublishCall;
import systems.zlink.framework.channels.ZLinkRequestCall;
import systems.zlink.framework.channels.ZLinkSendCall;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue;
import systems.zlink.framework.execution.ZLinkWorkerPool;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkRuntimeEventDispatcher;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.configuration.ZLinkDispatchErrorAction;
import systems.zlink.framework.configuration.ZLinkDispatchErrorReason;
import systems.zlink.framework.configuration.ZLinkDispatchErrorSurface;
import systems.zlink.framework.configuration.ZLinkDispatchMessageKind;
import systems.zlink.framework.configuration.ZLinkMessageFlowEvent;
import systems.zlink.framework.configuration.ZLinkMessageFlowOutcome;
import systems.zlink.framework.configuration.ZLinkDispatchFailure;
import systems.zlink.framework.runtime.actors.ZLinkActorSpotRoutePackets;
import systems.zlink.framework.runtime.actors.ZLinkActorReplyRoute;
import systems.zlink.framework.runtime.actors.ZLinkActorRuntime;
import systems.zlink.framework.runtime.configuration.ZLinkFrameworkRegistration;
import systems.zlink.framework.runtime.channels.ChannelRegistration;
import systems.zlink.framework.runtime.channels.ChannelKind;
import systems.zlink.framework.runtime.channels.ZLinkChannelRuntime;
import systems.zlink.framework.runtime.diagnostics.ZLinkDispatchErrorReporter;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddressResolver;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddress;
import systems.zlink.framework.runtime.internal.spots.ZLinkInstanceSpotCallRuntime;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerStages;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerScanner;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandler;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerKind;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerCatalog;
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationAdapter;
import systems.zlink.framework.runtime.locations.ZLinkLocationLifecycle;
import systems.zlink.framework.runtime.messaging.ZLinkPayloadEncoding;
import systems.zlink.framework.runtime.messaging.ZLinkMessagePayloads;
import systems.zlink.framework.runtime.messaging.ZLinkFrameworkErrorReply;
import systems.zlink.framework.runtime.messaging.ZLinkPacketNames;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteStatus;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkInboundDispatchBudget;
import systems.zlink.framework.runtime.messaging.ZLinkStringMessageSerializer;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;
import systems.zlink.framework.spots.ZLinkSpotCreateResult;
import systems.zlink.framework.spots.ZLinkSpotCreateResponse;
import systems.zlink.framework.spots.ZLinkWorkerCall;
import systems.zlink.framework.spots.ZLinkWorkerTask;
import systems.zlink.framework.spots.ZLinkSpotCreateState;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotInfo;
import systems.zlink.framework.spots.ZLinkSpotHandlerRegistry;
import systems.zlink.framework.spots.ZLinkSpotManager;
import systems.zlink.framework.spots.ZLinkSpotOutbound;
import systems.zlink.framework.spots.ZLinkSpotPublisherClient;
import systems.zlink.framework.spots.ZLinkSpotTimerHandler;
import systems.zlink.framework.spots.ZLinkTimer;
import systems.zlink.framework.spots.ZLinkTimerOptions;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

public final class ZLinkSpotRuntime
    extends ZLinkSpotContextHost
    implements ZLinkSpotManager, AutoCloseable {
    private static final Logger LOGGER = Logger.getLogger(ZLinkSpotRuntime.class.getName());
    private static final int ACTOR_RECV_INFO_NO_BIND = 1;

    private static final String REMOTE_BOUND_SESSION_BIND_PACKET_NAME =
        "zlink.framework.actor.bound_session.bind";

    private static final boolean STREAM_TRACE =
        "1".equals(System.getenv("ZLINK_JAVA_STREAM_TRACE"));
    private final ZLinkBackendContext context;
    private final boolean ownsContext;
    private final ZLinkFrameworkRegistration frameworkRegistration;
    private final List<ZLinkInternalSpotNode> nodes = new ArrayList<>();
    private final Map<String, ZLinkInternalSpotNode> nodesByName = new HashMap<>();
    private final ZLinkSpotLocationCoordinator spotLocations =
        new ZLinkSpotLocationCoordinator();
    private final ZLinkSpotLifecycle spotLifecycle;
    private final ZLinkActorSessionCoordinator actorSessions =
        new ZLinkActorSessionCoordinator();
    private final ZLinkActorSpotAdmission actorAdmissions =
        new ZLinkActorSpotAdmission();
    private final ZLinkInternalSpotNode primaryNode;
    private final String primaryNodeSourceName;
    private final ZLinkMessageSerializer serializer;
    private final ZLinkSpotRouteMessages routeMessages;
    private final ZLinkSpotDirectOutbound directOutbound;
    private final ZLinkSpotRoutedOutbound routedOutbound;
    private final ZLinkSpotPublisherRuntime publishers;
    private final ZLinkHandlerActivator handlerFactory;
    private final ZLinkDispatchErrorReporter dispatchErrors;
    private final ZLinkRuntimeEventDispatcher eventDispatcher;
    private final Executor handlerExecutor;
    private final List<ZLinkSuspendInvocationAdapter> suspendHandlerInvokers;
    private final ZLinkSpotHandlerInvoker spotHandlerInvoker;
    private final ZLinkSpotActorHandlerCatalog actorHandlers;
    private final ZLinkSpotHandlerLoader handlerLoader;
    private final ZLinkSpotActivationFactory activationFactory;
    private final Duration defaultRequestTimeout;
    private final ZLinkActorBoundSessionSender boundSessionSender;
    private final ZLinkChannelRuntime channels;
    private final List<ChannelRegistration> routeMeshChannels = new ArrayList<>();
    private final Set<RoutingId> manualRouterPeerNodeRids = ConcurrentHashMap.newKeySet();
    private final Set<RoutingId> autoConnectedRouterPeerNodeRids = ConcurrentHashMap.newKeySet();
    private final Map<String, ZLinkInstanceSpotActivation>
        instanceSpotActivations = new ConcurrentHashMap<>();
    private final Map<String, Duration> instanceSpotIdleTimeouts = new HashMap<>();
    private final List<ZLinkInternalMeshNode> routeMeshNodes;
    private final Map<String, ZLinkInternalMeshNode> routeMeshNodesByName;
    private volatile systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository
        userSpotAuthorityStore;
    private volatile systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository
        userSpotLocationStore;
    private volatile systems.zlink.framework.runtime.locations
        .ZLinkLocationRuntime userSpotLocationRuntime;
    private final systems.zlink.framework.runtime.locations
        .ZLinkServiceAuthorityPayloadCodec userSpotAuthorities =
            new systems.zlink.framework.runtime.locations
                .ZLinkServiceAuthorityPayloadCodec();
    private final systems.zlink.framework.runtime.internal.service
        .ZLinkServiceM6AWireCodec userSpotPayloads =
            new systems.zlink.framework.runtime.internal.service
                .ZLinkServiceM6AWireCodec();
    private final Set<String> suppressedActorLifecycleCallbacks = ConcurrentHashMap.newKeySet();
    private final ZLinkSpotRelocationReplyRoutes relocationReplyRoutes =
        new ZLinkSpotRelocationReplyRoutes();
    private final ZLinkSpotOutboundScope outboundScope = new ZLinkSpotOutboundScope();
    private volatile boolean closing;
    private volatile boolean draining;
    private volatile boolean relocating;
    private int drainInitialUserSpots;
    private final java.util.concurrent.atomic.AtomicBoolean drainRoomsMetricRecorded =
        new java.util.concurrent.atomic.AtomicBoolean();
    private final ZLinkWorkerPool workerPool;
    private final ScheduledExecutorService timerExecutor = Executors.newScheduledThreadPool(1, task -> {
        Thread thread = new Thread(task, "zlink-java-spot-timer");
        thread.setDaemon(true);
        return thread;
    });
    private final ExecutorService infrastructureExecutor =
        Executors.newVirtualThreadPerTaskExecutor();

    public ZLinkSpotRuntime(
        ZLinkBackendAdapterProvider backendFactory,
        ZLinkBackendAdapterOptions adapterOptions,
        ZLinkFrameworkRegistration registration) {
        this(backendFactory, adapterOptions, registration, null);
    }

    public ZLinkSpotRuntime(
        ZLinkBackendAdapterProvider backendFactory,
        ZLinkBackendAdapterOptions adapterOptions,
        ZLinkFrameworkRegistration registration,
        ZLinkChannelRuntime channels) {
        this(backendFactory, adapterOptions, registration, channels, ZLinkHandlerActivator.reflection());
    }

    public ZLinkSpotRuntime(
        ZLinkBackendAdapterProvider backendFactory,
        ZLinkBackendAdapterOptions adapterOptions,
        ZLinkFrameworkRegistration registration,
        ZLinkChannelRuntime channels,
        ZLinkHandlerActivator handlerFactory) {
        this(
            backendFactory,
            adapterOptions,
            registration,
            channels,
            new ZLinkStringMessageSerializer(),
            handlerFactory);
    }

    public ZLinkSpotRuntime(
        ZLinkBackendAdapterProvider backendFactory,
        ZLinkBackendAdapterOptions adapterOptions,
        ZLinkFrameworkRegistration registration,
        ZLinkChannelRuntime channels,
        ZLinkMessageSerializer serializer,
        ZLinkHandlerActivator handlerFactory) {
        this(
            backendFactory,
            adapterOptions,
            registration,
            channels,
            serializer,
            handlerFactory,
            null);
    }

    public ZLinkSpotRuntime(
        ZLinkBackendAdapterProvider backendFactory,
        ZLinkBackendAdapterOptions adapterOptions,
        ZLinkFrameworkRegistration registration,
        ZLinkChannelRuntime channels,
        ZLinkMessageSerializer serializer,
        ZLinkHandlerActivator handlerFactory,
        ZLinkRuntimeEventDispatcher eventDispatcher) {
        this(
            backendFactory,
            adapterOptions,
            registration,
            channels,
            null,
            true,
            serializer,
            handlerFactory,
            eventDispatcher,
            Map.of());
    }

    public ZLinkSpotRuntime(
        ZLinkBackendAdapterProvider backendFactory,
        ZLinkBackendAdapterOptions adapterOptions,
        ZLinkFrameworkRegistration registration,
        ZLinkChannelRuntime channels,
        ZLinkBackendContext context,
        ZLinkMessageSerializer serializer,
        ZLinkHandlerActivator handlerFactory,
        ZLinkRuntimeEventDispatcher eventDispatcher) {
        this(
            backendFactory,
            adapterOptions,
            registration,
            channels,
            context,
            false,
            serializer,
            handlerFactory,
            eventDispatcher,
            Map.of());
    }

    public ZLinkSpotRuntime(
        ZLinkBackendAdapterProvider backendFactory,
        ZLinkBackendAdapterOptions adapterOptions,
        ZLinkFrameworkRegistration registration,
        ZLinkChannelRuntime channels,
        ZLinkBackendContext context,
        ZLinkMessageSerializer serializer,
        ZLinkHandlerActivator handlerFactory,
        ZLinkRuntimeEventDispatcher eventDispatcher,
        Map<String, ZLinkInternalMeshNode> meshNodes) {
        this(
            backendFactory,
            adapterOptions,
            registration,
            channels,
            context,
            false,
            serializer,
            handlerFactory,
            eventDispatcher,
            meshNodes,
            (ignoredBackend, ignoredKey) -> (ignoredSubmission, ignoredCleanup) ->
                CompletableFuture.failedFuture(new IllegalStateException(
                    "one-way admission factory is required")));
    }

    public ZLinkSpotRuntime(
        ZLinkBackendAdapterProvider backendFactory,
        ZLinkBackendAdapterOptions adapterOptions,
        ZLinkFrameworkRegistration registration,
        ZLinkChannelRuntime channels,
        ZLinkBackendContext context,
        ZLinkMessageSerializer serializer,
        ZLinkHandlerActivator handlerFactory,
        ZLinkRuntimeEventDispatcher eventDispatcher,
        Map<String, ZLinkInternalMeshNode> meshNodes,
        java.util.function.BiFunction<
            ZLinkBackendObject,
            ZLinkBackendAdmissionKey,
            java.util.function.BiFunction<
                java.util.function.Supplier<Boolean>,
                Runnable,
                CompletionStage<Void>>> admission) {
        this(
            backendFactory,
            adapterOptions,
            registration,
            channels,
            context,
            false,
            serializer,
            handlerFactory,
            eventDispatcher,
            meshNodes,
            admission);
    }

    private ZLinkSpotRuntime(
        ZLinkBackendAdapterProvider backendFactory,
        ZLinkBackendAdapterOptions adapterOptions,
        ZLinkFrameworkRegistration registration,
        ZLinkChannelRuntime channels,
        ZLinkBackendContext context,
        boolean ownsContext,
        ZLinkMessageSerializer serializer,
        ZLinkHandlerActivator handlerFactory,
        ZLinkRuntimeEventDispatcher eventDispatcher,
        Map<String, ZLinkInternalMeshNode> meshNodes) {
        this(
            backendFactory,
            adapterOptions,
            registration,
            channels,
            context,
            ownsContext,
            serializer,
            handlerFactory,
            eventDispatcher,
            meshNodes,
            (ignoredBackend, ignoredKey) -> (ignoredSubmission, ignoredCleanup) ->
                CompletableFuture.failedFuture(new IllegalStateException(
                    "one-way admission factory is required")));
    }

    private ZLinkSpotRuntime(
        ZLinkBackendAdapterProvider backendFactory,
        ZLinkBackendAdapterOptions adapterOptions,
        ZLinkFrameworkRegistration registration,
        ZLinkChannelRuntime channels,
        ZLinkBackendContext context,
        boolean ownsContext,
        ZLinkMessageSerializer serializer,
        ZLinkHandlerActivator handlerFactory,
        ZLinkRuntimeEventDispatcher eventDispatcher,
        Map<String, ZLinkInternalMeshNode> meshNodes,
        java.util.function.BiFunction<
            ZLinkBackendObject,
            ZLinkBackendAdmissionKey,
            java.util.function.BiFunction<
                java.util.function.Supplier<Boolean>,
                Runnable,
                CompletionStage<Void>>> admission) {
        if (registration.spotNodes().isEmpty() && meshNodes.isEmpty()) {
            throw new ZLinkConfigurationException("at least one mesh node is required");
        }
        this.frameworkRegistration = registration;
        this.routeMeshNodes = List.copyOf(meshNodes.values());
        this.routeMeshNodesByName = Map.copyOf(meshNodes);
        this.channels = channels;
        this.serializer = java.util.Objects.requireNonNull(serializer, "serializer");
        this.routeMessages = new ZLinkSpotRouteMessages(this.serializer);
        this.handlerFactory = handlerFactory;
        this.eventDispatcher = eventDispatcher;
        this.handlerExecutor = systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext
            .propagating(java.util.Objects.requireNonNull(
                registration.handlerExecutor(), "handlerExecutor"));
        this.dispatchErrors = new ZLinkDispatchErrorReporter(
            registration.dispatchOptions(),
            handlerFactory,
            this.handlerExecutor,
            eventDispatcher);
        this.directOutbound = new ZLinkSpotDirectOutbound(
            routeMessages,
            this.handlerExecutor,
            dispatchErrors.flow(),
            new ZLinkOneWayCalls(admission));
        this.routedOutbound = new ZLinkSpotRoutedOutbound(
            channels,
            routeMessages,
            directOutbound,
            dispatchErrors.flow(),
            nodesByName::get);
        this.publishers = new ZLinkSpotPublisherRuntime(
            serializer,
            routeMessages,
            Math.max(2, Runtime.getRuntime().availableProcessors()),
            backendFactory.admissionTimeout(),
            registration.codecs()::contentTypeFor);
        this.suspendHandlerInvokers = registration.suspendHandlerInvokers();
        this.spotHandlerInvoker = new ZLinkSpotHandlerInvoker(
            serializer,
            suspendHandlerInvokers);
        this.workerPool = new ZLinkWorkerPool(
            registration.workers().minThreads(),
            registration.workers().maxThreads(),
            registration.workers().idleTimeout(),
            registration.workers().maxQueueLength());
        ZLinkScannedHandlerCatalog scannedHandlers =
            ZLinkHandlerScanner.scan(registration.handlerPackageMarkers());
        this.actorHandlers = new ZLinkSpotActorHandlerCatalog(scannedHandlers, serializer);
        this.handlerLoader = new ZLinkSpotHandlerLoader(scannedHandlers, actorHandlers);
        ZLinkChannelBackendAdapter channelAdapter =
            backendFactory.createChannelAdapter(adapterOptions);
        ZLinkSpotBackendAdapter spotAdapter = registration.spotNodes().isEmpty()
            ? null
            : backendFactory.createSpotAdapter(adapterOptions);
        this.context = context == null ? channelAdapter.createContext() : context;
        this.ownsContext = ownsContext;
        this.defaultRequestTimeout = registration.defaultRequestTimeout();
        this.boundSessionSender = new ZLinkActorBoundSessionSender(
            defaultRequestTimeout,
            this::isClosing,
            ZLinkSpotRuntime::traceActorSession);
        Map<String, ZLinkInternalSpotNode> routeBridgeNodesByName = new HashMap<>();
        Set<Class<? extends ZLinkSpot<?>>> initializedSpotTypes = new HashSet<>();
        Map<Class<? extends ZLinkSpot<?>>,
            systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode>
                initializedSpotExecutionModes = new HashMap<>();
        Map<Class<? extends ZLinkSpot<?>>,
            systems.zlink.framework.configuration
                .ZLinkSpotRelocationReadinessMode>
                initializedSpotRelocationReadiness = new HashMap<>();
        List<EntrySpotInitialization> entrySpotInitializations =
            new ArrayList<>();
        for (SpotNodeRegistration nodeRegistration : registration.spotNodes()) {
            ZLinkInternalSpotNode node =
                spotAdapter.createSpotNode(this.context, resolveSpotNodeMode(nodeRegistration));
            if (nodeRegistration.nodeRoutingId() != null) {
                node.setRoutingId(nodeRegistration.nodeRoutingId());
                if (nodeRegistration.pubSubEnabled()) {
                    node.setPublisherRoutingId(deriveRoutingId(
                        nodeRegistration.nodeRoutingId(),
                        "pub"));
                    node.setSubscriberRoutingId(deriveRoutingId(
                        nodeRegistration.nodeRoutingId(),
                        "sub"));
                }
            }
            if (!nodeRegistration.entrySpots().isEmpty()
                || !nodeRegistration.actorFactories().isEmpty()) {
                ZLinkBackendSpot entryBackendSpot = node.entrySpot();
                entryBackendSpot.setRoutingId(
                    nodeRegistration.entrySpotId());
                entrySpotInitializations.add(new EntrySpotInitialization(
                    node.routingId(),
                    entryBackendSpot,
                    nodeRegistration.entrySpots()));
            }
            if (nodeRegistration.routerBind() != null) {
                node.setRouterBind(nodeRegistration.routerBind());
            }
            if (nodeRegistration.pubBind() != null) {
                node.setPubBind(nodeRegistration.pubBind());
            }
            for (SpotNodeRegistration.RouterManualConnection connection
                    : nodeRegistration.routerManualConnections()) {
                if (connection.peerRoutingId() != null) {
                    node.connectPeer(connection.peerRoutingId(), connection.endpoint());
                    manualRouterPeerNodeRids.add(connection.peerRoutingId());
                } else {
                    node.connectPeer(connection.endpoint());
                }
            }
            for (String endpoint : nodeRegistration.pubSubManualConnections()) {
                node.connectPeer(endpoint);
            }
            nodes.add(node);
            nodesByName.put(nodeRegistration.nodeName(), node);
            initializedSpotTypes.addAll(nodeRegistration.spotFactories());
            nodeRegistration.spotFactories().forEach(type ->
                initializedSpotExecutionModes.putIfAbsent(
                    type,
                    systems.zlink.framework.configuration
                        .ZLinkUserSpotExecutionMode.SPOT_WIDE));
            nodeRegistration.spotFactories().forEach(type ->
                initializedSpotRelocationReadiness.putIfAbsent(
                    type,
                    systems.zlink.framework.configuration
                        .ZLinkSpotRelocationReadinessMode.ANY_TURN_BOUNDARY));
            if (nodeRegistration.pubSubEnabled()) {
                publishers.register(nodeRegistration.meshName(), node);
            }
            if (nodeRegistration.routerEnabled()) {
                routeBridgeNodesByName.put(nodeRegistration.nodeName(), node);
                if (channels != null) {
                    channels.registerSpotRouterNode(nodeRegistration.meshName(), node);
                    if (!nodeRegistration.meshName().equals(nodeRegistration.nodeName())) {
                        channels.registerSpotRouterNode(nodeRegistration.nodeName(), node);
                    }
                }
            }
            spotLocations.registerNode(
                nodeRegistration.meshName(),
                node.routingId(),
                node.entrySpot().spotId(),
                node.entrySpot().lifecycleGeneration(),
                nodeRegistration.routerBind(),
                nodeRegistration.pubSubEnabled());
        }
        for (var nodeRegistration : registration.meshNodes()) {
            if (nodeRegistration.spotFactories().isEmpty()
                && nodeRegistration.entrySpots().isEmpty()
                && nodeRegistration.actorFactories().isEmpty()
                && nodeRegistration.channelNames().isEmpty()
                && !nodeRegistration.objectRoleEnabled()) {
                continue;
            }
            ZLinkInternalMeshNode meshNode = meshNodes.get(nodeRegistration.meshName());
            if (meshNode == null) {
                throw new ZLinkConfigurationException(
                    "MeshNode runtime is missing: " + nodeRegistration.meshName());
            }
            ZLinkInternalSpotNode node = meshNode.spotNode();
            instanceSpotIdleTimeouts.put(
                nodeRegistration.meshName(),
                nodeRegistration.instanceSpotIdleTimeout());
            nodeRegistration.relocatableInstanceSpotFactories().values()
                .forEach(factory -> meshNode.registerInstanceSpotType(
                    factory.stableType(),
                    (stableType, route, backendSpot) ->
                        activateInstanceSpotTarget(
                            nodeRegistration.meshName(),
                            factory,
                            route,
                            backendSpot)));
            if (!nodeRegistration.entrySpots().isEmpty()
                || !nodeRegistration.actorFactories().isEmpty()) {
                ZLinkBackendSpot entryBackendSpot = node.entrySpot();
                entryBackendSpot.setRoutingId(
                    nodeRegistration.entrySpotId());
                entrySpotInitializations.add(new EntrySpotInitialization(
                    node.routingId(),
                    entryBackendSpot,
                    nodeRegistration.entrySpots()));
            }
            nodes.add(node);
            nodesByName.put(nodeRegistration.meshName(), node);
            initializedSpotTypes.addAll(nodeRegistration.spotFactories());
            nodeRegistration.spotFactories().forEach(type ->
                initializedSpotExecutionModes.putIfAbsent(
                    type,
                    systems.zlink.framework.configuration
                        .ZLinkUserSpotExecutionMode.SPOT_WIDE));
            nodeRegistration.spotFactories().forEach(type ->
                initializedSpotRelocationReadiness.putIfAbsent(
                    type,
                    systems.zlink.framework.configuration
                        .ZLinkSpotRelocationReadinessMode.ANY_TURN_BOUNDARY));
            nodeRegistration.relocatableSpotFactories().values().forEach(factory -> {
                var previous = initializedSpotExecutionModes.put(
                    factory.spotType(),
                    factory.options().executionMode());
                if (previous != null
                    && previous != factory.options().executionMode()) {
                    throw new ZLinkConfigurationException(
                        "User Spot class is registered with conflicting "
                            + "execution modes: "
                            + factory.spotType().getName());
                }
                initializedSpotRelocationReadiness.put(
                    factory.spotType(),
                    factory.options().relocationReadiness());
            });
            if (!nodeRegistration.channelNames().isEmpty()) {
                publishers.register(nodeRegistration.meshName(), node);
                if (channels != null) {
                    channels.registerSpotRouterNode(nodeRegistration.meshName(), node);
                    for (String channelName : nodeRegistration.channelNames()) {
                        publishers.register(channelName, node);
                        channels.registerSpotRouterNode(channelName, node);
                    }
                }
            }
            spotLocations.registerNode(
                nodeRegistration.meshName(),
                node.routingId(),
                node.entrySpot().spotId(),
                node.entrySpot().lifecycleGeneration(),
                nodeRegistration.bindEndpoint(),
                !nodeRegistration.channelNames().isEmpty());
        }
        for (var channel : registration.channels()) {
            if (channel.kind() == ChannelKind.ROUTE_MESH) {
                routeMeshChannels.add(channel);
            }
        }
        attachRouteMeshSpotBridges(routeBridgeNodesByName);
        if (!registration.spotNodes().isEmpty()) {
            this.primaryNode = nodesByName.get(registration.spotNodes().get(0).nodeName());
            this.primaryNodeSourceName = registration.spotNodes().get(0).nodeName();
        } else {
            var primaryRegistration = registration.meshNodes().get(0);
            this.primaryNode = nodesByName.get(primaryRegistration.meshName());
            this.primaryNodeSourceName = primaryRegistration.meshName();
        }
        this.activationFactory = new ZLinkSpotActivationFactory(
            this,
            workerPool,
            handlerLoader,
            spotHandlerInvoker,
            handlerFactory,
            initializedSpotExecutionModes,
            initializedSpotRelocationReadiness);
        this.spotLifecycle = new ZLinkSpotLifecycle(
            primaryNode,
            primaryNodeSourceName,
            handlerExecutor,
            spotLocations,
            initializedSpotTypes,
            activationFactory::activate,
            actorSessions::hasActorsInSpot);
        for (EntrySpotInitialization initialization : entrySpotInitializations) {
            for (Class<? extends ZLinkEntrySpot<?>> entrySpotType :
                initialization.entrySpots()) {
                spotLifecycle.addEntrySpot(activationFactory.activateEntry(
                    initialization.nodeRid(),
                    initialization.backendSpot(),
                    entrySpotType));
            }
        }
    }

    public void installUserSpotOperationHandlers(
        systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository authorityStore,
        systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository locationStore,
        systems.zlink.framework.runtime.locations.ZLinkLocationRuntime
            locationRuntime) {
        java.util.Objects.requireNonNull(authorityStore, "authorityStore");
        java.util.Objects.requireNonNull(locationStore, "locationStore");
        userSpotAuthorityStore = authorityStore;
        userSpotLocationStore = locationStore;
        userSpotLocationRuntime = java.util.Objects.requireNonNull(
            locationRuntime, "locationRuntime");
        for (var registration : frameworkRegistration.meshNodes()) {
            if (registration.relocatableSpotFactories().isEmpty()) {
                continue;
            }
            ZLinkInternalMeshNode meshNode =
                routeMeshNodesByName.get(registration.meshName());
            if (meshNode == null) {
                throw new ZLinkConfigurationException(
                    "MeshNode runtime is missing: " + registration.meshName());
            }
            ZLinkInternalMeshNode.UserSpotOperationHandler handler =
                new ZLinkUserSpotOperationHandler(
                    registration.meshName(),
                    meshNode,
                    authorityStore,
                    spotLifecycle,
                    serializer,
                    registration.relocatableSpotFactories());
            meshNode.setUserSpotOperationHandler(handler);
        }
    }

    private static ZLinkBackendSpotNodeMode resolveSpotNodeMode(
        SpotNodeRegistration registration) {
        if (registration.routerEnabled() && registration.pubSubEnabled()) {
            return ZLinkBackendSpotNodeMode.ALL;
        }
        if (registration.routerEnabled()) {
            return ZLinkBackendSpotNodeMode.ROUTED;
        }
        if (registration.pubSubEnabled()) {
            return ZLinkBackendSpotNodeMode.PUBSUB;
        }
        throw new ZLinkConfigurationException(
            "spot node must enable router or pub/sub capability: "
                + registration.nodeName());
    }

    private static RoutingId deriveRoutingId(RoutingId base, String suffix) {
        byte[] baseBytes = base.toBytes();
        byte[] suffixBytes = suffix.getBytes(java.nio.charset.StandardCharsets.UTF_8);
        if (baseBytes.length + 1 + suffixBytes.length > RoutingId.MAX_LENGTH) {
            throw new ZLinkConfigurationException(
                "derived routing id must be at most 255 bytes");
        }
        byte[] bytes = new byte[baseBytes.length + 1 + suffixBytes.length];
        System.arraycopy(baseBytes, 0, bytes, 0, baseBytes.length);
        System.arraycopy(suffixBytes, 0, bytes, baseBytes.length + 1, suffixBytes.length);
        return RoutingId.from(bytes);
    }

    @Override
    public systems.zlink.framework.spots.ZLinkSpotCreateCall create(
        String spotType) {
        rejectAfterRelocationReady("User Spot create");
        return new CreateCall(requireStableType(spotType));
    }

    @Override
    public systems.zlink.framework.spots.ZLinkSpotGetOrCreateCall getOrCreate(
        String spotId,
        String spotType) {
        rejectAfterRelocationReady("User Spot getOrCreate");
        return new GetOrCreateCall(
            systems.zlink.framework.runtime.internal.spots
                .ZLinkSpotIdValidator.requireCallerAssignable(spotId),
            requireStableType(spotType));
    }

    @Override
    public CompletionStage<Optional<systems.zlink.framework.spots.SpotRef>> find(
        String spotId) {
        rejectAfterRelocationReady("User Spot find");
        java.util.Objects.requireNonNull(spotId, "spotId");
        systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository store =
            requireUserSpotAuthorityStore();
        return store.read(
                systems.zlink.framework.runtime.locations
                    .ZLinkAuthorityKeyCodec.spot(spotId),
                () -> false)
            .thenApply(read -> read
                instanceof systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot snapshot
                ? readyRef(snapshot, spotId)
                : Optional.empty());
    }

    @Override
    public CompletionStage<Boolean> close(
        systems.zlink.framework.spots.SpotRef spot) {
        rejectAfterRelocationReady("User Spot close");
        java.util.Objects.requireNonNull(spot, "spot");
        systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository store =
            requireUserSpotAuthorityStore();
        String key = systems.zlink.framework.runtime.locations
            .ZLinkAuthorityKeyCodec.spot(spot.spotId());
        return store.read(key, () -> false).thenCompose(read -> {
            if (!(read instanceof systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot snapshot)) {
                return CompletableFuture.completedFuture(false);
            }
            var authority = userSpotAuthorities.decode(snapshot.payload())
                .orElseThrow(() -> new IllegalStateException(
                    "invalid User Spot authority"));
            if (authority.kind()
                    != systems.zlink.framework.runtime.locations
                        .ZLinkServiceAuthorityPayloadCodec.Kind.USER
                || snapshot.allocation().objectKind()
                    != systems.zlink.framework.locations.ZLinkPlacementObjectKind.USER_SPOT) {
                return CompletableFuture.failedFuture(
                    new systems.zlink.framework.runtime.internal.backend
                        .ZLinkUserSpotOperationException(
                            107, 33,
                            "User Spot authority kind is stale"));
            }
            if (authority.state()
                    != systems.zlink.framework.runtime.locations
                        .ZLinkServiceAuthorityPayloadCodec.State.READY
                || snapshot.allocation().state()
                    != systems.zlink.framework.runtime.internal.locations.ZLinkPlacementAllocationState.ACTIVE) {
                return CompletableFuture.failedFuture(
                    new systems.zlink.framework.runtime.internal.backend
                        .ZLinkUserSpotOperationException(
                            107, 34,
                            "User Spot is moving"));
            }
            if (snapshot.objectGeneration() != spot.objectGeneration()) {
                return CompletableFuture.failedFuture(
                    new systems.zlink.framework.runtime.internal.backend
                        .ZLinkUserSpotOperationException(
                            107, 33,
                            "User Spot generation is stale"));
            }
            if (!authority.meshName().equals(spot.meshName())
                || !authority.nodeRid().equals(spot.nodeRid())) {
                return CompletableFuture.failedFuture(
                    new systems.zlink.framework.runtime.internal.backend
                        .ZLinkUserSpotOperationException(
                            107, 34,
                            "User Spot is moving"));
            }
            ZLinkInternalMeshNode source = routeMeshNodesByName.get(
                authority.meshName());
            if (source == null) {
                return CompletableFuture.failedFuture(
                    new IllegalStateException(
                        "Object client Mesh is not configured: "
                            + authority.meshName()));
            }
            long deadline = System.currentTimeMillis()
                + defaultRequestTimeout.toMillis();
            var intent = new ZLinkInternalMeshNode.UserSpotCloseIntent(
                        new systems.zlink.framework.runtime.internal.service
                            .ZLinkServiceM6BWireCodec.UserSpotCloseFence(
                                spot.spotId(),
                                spot.objectGeneration(),
                                authority.nodeRid(),
                                authority.nodeGeneration(),
                                snapshot.authorityOwnerGeneration(),
                                snapshot.storeVersion()),
                        deadline);
            CompletionStage<ZLinkInternalMeshNode.UserSpotCloseResponse>
                targetClose = source.requestUserSpotClose(
                    authority.nodeRid(), intent, defaultRequestTimeout);
            return targetClose
                .thenApply(
                    ZLinkInternalMeshNode.UserSpotCloseResponse::closed);
        });
    }

    private static void rejectAfterRelocationReady(String operation) {
        systems.zlink.framework.runtime.internal.handlers
            .ZLinkSuspendInvocationContext.rejectAfterRelocationReady(
                operation);
    }

    private CompletionStage<ZLinkSpotCreateResult> submitUserSpot(
        String requestedId,
        String stableType,
        String meshName,
        ZLinkMessage request,
        Duration timeout,
        boolean getOrCreate) {
        requireAcceptingNewState();
        systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository locations =
            requireUserSpotLocationStore();
        String selectedMesh = resolveObjectMesh(meshName);
        String spotId = requestedId == null
            ? java.util.UUID.randomUUID().toString()
            : requestedId;
        byte[] applicationBytes =
            request.toEncodedPayload(serializer).bytes();
        byte[] envelope = userSpotPayloads.encodeApplicationPayload(
            new systems.zlink.framework.runtime.internal.service
                .ZLinkServiceM6AWireCodec.ApplicationPayload(
                    "zlink.user-spot-create",
                    "application/zlink-framework-json-v1",
                    applicationBytes));
        if (envelope.length > 1024 * 1024) {
            return CompletableFuture.failedFuture(
                new IllegalArgumentException(
                    "User Spot creation request exceeds 1 MiB"));
        }
        Duration effectiveTimeout = timeout == null
            ? defaultRequestTimeout : timeout;
        long deadline = System.currentTimeMillis()
            + effectiveTimeout.toMillis();
        return placeUserSpot(
            locations,
            selectedMesh,
            spotId,
            stableType,
            envelope,
            deadline,
            effectiveTimeout,
            getOrCreate,
            java.util.Set.of());
    }

    private CompletionStage<ZLinkSpotCreateResult> placeUserSpot(
        systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository locations,
        String meshName,
        String spotId,
        String stableType,
        byte[] envelope,
        long deadline,
        Duration timeout,
        boolean getOrCreate,
        java.util.Set<systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptorKey> excludedTargets) {
        return selectUserSpotTarget(
                locations,
                meshName,
                stableType,
                deadline,
                excludedTargets)
            .thenCompose(target -> reserveAndCreate(
                locations,
                meshName,
                spotId,
                stableType,
                envelope,
                target,
                deadline,
                timeout,
                getOrCreate,
                excludedTargets));
    }

    private CompletionStage<ZLinkSpotCreateResult> reserveAndCreate(
        systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository locations,
        String meshName,
        String spotId,
        String stableType,
        byte[] envelope,
        systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor target,
        long deadline,
        Duration timeout,
        boolean getOrCreate,
        java.util.Set<systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptorKey> excludedTargets) {
        String key = systems.zlink.framework.runtime.locations
            .ZLinkAuthorityKeyCodec.spot(spotId);
        var owner = new systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken(
                target.ownerId(), target.leaseGeneration());
        byte[] creating = userSpotAuthorities.encodeUser(
            systems.zlink.framework.runtime.locations
                .ZLinkServiceAuthorityPayloadCodec.State.CREATING,
            stableType,
            spotId,
            target.ownerId(),
            target.leaseGeneration(),
            meshName,
            target.rid(),
            target.lifecycleGeneration());
        var reserve = new systems.zlink.framework.runtime.internal.locations.ZLinkObjectReservationRequest(
                systems.zlink.framework.locations.ZLinkPlacementObjectKind.USER_SPOT,
                key,
                stableType,
                inlineCreationIntent(envelope),
                sha256(envelope),
                envelope.length,
                new systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptorKey(meshName, target.rid()),
                target.lifecycleGeneration(),
                owner,
                creating,
                systems.zlink.framework.runtime.internal.locations.ZLinkPlacementCapacityBundle.spot(
                        systems.zlink.framework.locations.ZLinkPlacementObjectKind.USER_SPOT,
                        stableType,
                        1));
        return locations.reserve(reserve, () -> false)
            .thenCompose(result -> {
                if (result instanceof systems.zlink.framework.runtime.internal.locations.ZLinkObjectAlreadyExists exists) {
                    if (!getOrCreate) {
                        return CompletableFuture.failedFuture(
                            new IllegalStateException(
                                "User Spot already exists"));
                    }
                    return existingResult(
                        exists.current(), spotId, stableType);
                }
                if (result instanceof systems.zlink.framework.runtime.internal.locations.ZLinkObjectTypeMismatch) {
                    return CompletableFuture.failedFuture(
                        new IllegalStateException(
                                "User Spot type does not match"));
                }
                if (result instanceof systems.zlink.framework.runtime.internal.locations.ZLinkPlacementCapacityExhausted
                    || result instanceof systems.zlink.framework.runtime.internal.locations.ZLinkObjectConflict conflict
                        && conflict.current() instanceof
                            systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityMissing) {
                    return placeUserSpot(
                        locations,
                        meshName,
                        spotId,
                        stableType,
                        envelope,
                        deadline,
                        timeout,
                        getOrCreate,
                        excluding(excludedTargets, target));
                }
                if (result instanceof systems.zlink.framework.runtime.internal.locations.ZLinkObjectConflict) {
                    return CompletableFuture.supplyAsync(
                            () -> null,
                            CompletableFuture.delayedExecutor(
                                10, TimeUnit.MILLISECONDS))
                        .thenCompose(ignored -> placeUserSpot(
                            locations,
                            meshName,
                            spotId,
                            stableType,
                            envelope,
                            deadline,
                            timeout,
                            getOrCreate,
                            excludedTargets));
                }
                if (!(result instanceof systems.zlink.framework.runtime.internal.locations.ZLinkObjectReserved reserved)) {
                    return CompletableFuture.failedFuture(
                        new IllegalStateException(
                            "User Spot reservation failed: "
                                + result.getClass().getSimpleName()));
                }
                var reservation = reserved.reservation();
                ZLinkInternalMeshNode source =
                    routeMeshNodesByName.get(meshName);
                if (source == null) {
                    return locations.abort(reservation, () -> false)
                        .thenCompose(ignored ->
                            CompletableFuture.failedFuture(
                                new IllegalStateException(
                                    "Object client Mesh is not configured: "
                                        + meshName)));
                }
                var fence = new systems.zlink.framework.runtime.internal.service
                    .ZLinkServiceM6BWireCodec.ReservationFence(
                        reservation.reservationVersion(),
                        reservation.storeVersion(),
                        reservation.objectGeneration(),
                        reservation.authorityOwnerGeneration(),
                        target.rid(),
                        target.lifecycleGeneration(),
                        target.ownerId(),
                        target.leaseGeneration(),
                        1);
                var intent =
                    new ZLinkInternalMeshNode.UserSpotCreateIntent(
                        spotId, stableType, fence, deadline);
                ensureManualObjectPeer(
                    meshName, source, target);
                CompletionStage<ZLinkInternalMeshNode.UserSpotCreateResponse>
                    targetCreate = source.requestUserSpotCreate(
                        target.rid(), intent, timeout);
                return targetCreate
                    .thenApply(response -> {
                        ZLinkMessage reply =
                            response.applicationReply().isEmpty()
                                ? null
                                : ZLinkMessage.fromEncoded(
                                    systems.zlink.framework.ZLinkEncodedPayload
                                        .from(response.applicationReply()
                                            .getLast().toByteArray()),
                                    serializer);
                        response.applicationReply().forEach(Message::close);
                        return new ZLinkSpotCreateResult(
                            new systems.zlink.framework.spots.SpotRef(
                                response.spotId(),
                                response.objectGeneration(),
                                meshName,
                                target.rid()),
                            switch (response.result()) {
                                case EXISTING ->
                                    ZLinkSpotCreateState.EXISTING;
                                case CREATED ->
                                    ZLinkSpotCreateState.CREATED;
                                case REJECTED ->
                                    ZLinkSpotCreateState.REJECTED;
                            },
                            reply);
                    });
            });
    }

    private void ensureManualObjectPeer(
        String meshName,
        ZLinkInternalMeshNode source,
        systems.zlink.framework.runtime.internal.locations
            .ZLinkMeshNodeDescriptor target) {
        if (source.status().routingId().equals(target.rid())
            || manualRouterPeerNodeRids.contains(target.rid())) {
            return;
        }
        boolean configured = frameworkRegistration.meshNodes().stream()
            .filter(node -> node.meshName().equals(meshName))
            .flatMap(node -> node.peers().stream())
            .anyMatch(peer -> peer.expectedRoutingId() == null
                && peer.endpoint().equals(target.endpoint()));
        if (configured
            && manualRouterPeerNodeRids.add(target.rid())) {
            source.connectPeer(target.endpoint(), target.rid());
        }
    }

    private CompletionStage<ZLinkSpotCreateResult> existingResult(
        systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot snapshot,
        String spotId,
        String stableType) {
        var authority = userSpotAuthorities.decode(snapshot.payload())
            .orElseThrow(() -> new IllegalStateException(
                "invalid User Spot authority"));
        if (!authority.stableType().equals(stableType)
            || authority.state()
                != systems.zlink.framework.runtime.locations
                    .ZLinkServiceAuthorityPayloadCodec.State.READY) {
            return CompletableFuture.failedFuture(
                new IllegalStateException(
                    "User Spot type or state does not match"));
        }
        return CompletableFuture.completedFuture(
            new ZLinkSpotCreateResult(
                new systems.zlink.framework.spots.SpotRef(
                    spotId,
                    snapshot.objectGeneration(),
                    authority.meshName(),
                    authority.nodeRid()),
                ZLinkSpotCreateState.EXISTING,
                null));
    }

    private CompletionStage<
        systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor>
        selectUserSpotTarget(
            systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository locations,
            String meshName,
            String stableType,
            long deadlineUnixMs,
            java.util.Set<systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptorKey> excludedTargets) {
        return locations.listMeshNodes(
                meshName,
                new systems.zlink.framework.locations.ZLinkPageRequest(
                    1000, null))
            .thenApply(page -> {
                List<systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor> candidates =
                        page.items().stream()
                            .filter(node ->
                                node.state()
                                    == systems.zlink.framework.runtime.host
                                        .ZLinkFrameworkRuntimeState.SERVING
                                    && node.objectRole()
                                        == systems.zlink.framework.locations.ZLinkMeshNodeObjectRole.SERVER
                                    && node.placementWeight() > 0
                                    && !excludedTargets.contains(
                                        descriptorKey(node))
                                    && node.objectCapabilities().stream()
                                        .anyMatch(capability ->
                                            capability.objectKind()
                                                == systems.zlink.framework
                                                    .locations
                                                    .ZLinkPlacementObjectKind
                                                    .USER_SPOT
                                            && capability.stableType()
                                                .equals(stableType)
                                            && hasCapacity(
                                                node,
                                                capability)))
                            .toList();
                if (candidates.isEmpty()) {
                    if (System.currentTimeMillis() >= deadlineUnixMs) {
                        throw new IllegalStateException(
                            "No Ready User Spot placement target");
                    }
                    return null;
                }
                long total = 0;
                for (var candidate : candidates) {
                    total = Math.addExact(
                        total,
                        candidate.placementWeight());
                }
                long selected = java.util.concurrent.ThreadLocalRandom
                    .current().nextLong(total);
                for (var candidate : candidates) {
                    selected -= candidate.placementWeight();
                    if (selected < 0) {
                        return candidate;
                    }
                }
                return candidates.getLast();
            })
            .thenCompose(found -> found != null
                ? CompletableFuture.completedFuture(found)
                : CompletableFuture.supplyAsync(
                        () -> null,
                        CompletableFuture.delayedExecutor(
                            10, TimeUnit.MILLISECONDS))
                    .thenCompose(ignored -> selectUserSpotTarget(
                        locations,
                        meshName,
                        stableType,
                        deadlineUnixMs,
                        java.util.Set.of())));
    }

    static boolean hasCapacity(
        systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor node,
        systems.zlink.framework.locations.ZLinkObjectCapability capability) {
        return hasRoom(node.capacity().spots())
            && node.capacity().spotTypes().stream()
                .filter(type ->
                    type.objectKind() == capability.objectKind()
                        && type.stableType().equals(
                            capability.stableType()))
                .findFirst()
                .map(type -> hasRoom(type.usage()))
                .orElse(false);
    }

    private static boolean hasRoom(
        systems.zlink.framework.locations.ZLinkCapacityUsage usage) {
        return usage.limit() == 0
            || (long) usage.active() + usage.reserved() < usage.limit();
    }

    private static systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptorKey descriptorKey(
            systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor node) {
        return new systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptorKey(node.meshName(), node.rid());
    }

    private static java.util.Set<systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptorKey> excluding(
            java.util.Set<systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptorKey> current,
            systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor node) {
        var result = new java.util.HashSet<>(current);
        result.add(descriptorKey(node));
        return java.util.Set.copyOf(result);
    }

    private Optional<systems.zlink.framework.spots.SpotRef> readyRef(
        systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot snapshot,
        String spotId) {
        return userSpotAuthorities.decode(snapshot.payload())
            .filter(authority ->
                authority.state()
                    == systems.zlink.framework.runtime.locations
                        .ZLinkServiceAuthorityPayloadCodec.State.READY)
            .map(authority -> new systems.zlink.framework.spots.SpotRef(
                spotId,
                snapshot.objectGeneration(),
                authority.meshName(),
                authority.nodeRid()));
    }

    private String resolveObjectMesh(String requested) {
        if (requested != null) {
            if (!routeMeshNodesByName.containsKey(requested)) {
                throw new IllegalStateException(
                    "Mesh is not configured: " + requested);
            }
            return requested;
        }
        List<String> meshes = frameworkRegistration.meshNodes().stream()
            .filter(
                systems.zlink.framework.runtime.mesh
                    .MeshNodeRegistration::objectRoleEnabled)
            .map(systems.zlink.framework.runtime.mesh
                .MeshNodeRegistration::meshName)
            .distinct()
            .toList();
        if (meshes.size() != 1) {
            throw new IllegalStateException(
                meshes.isEmpty()
                    ? "Object client is not configured"
                    : "Mesh selection is required");
        }
        return meshes.getFirst();
    }

    private systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository
        requireUserSpotAuthorityStore() {
        if (userSpotAuthorityStore == null) {
            throw new IllegalStateException(
                "User Spot Location authority is not configured");
        }
        return userSpotAuthorityStore;
    }

    private systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository
        requireUserSpotLocationStore() {
        if (userSpotLocationStore == null) {
            throw new IllegalStateException(
                "User Spot Location Store is not configured");
        }
        return userSpotLocationStore;
    }

    private static String requireStableType(String value) {
        if (value == null || value.isBlank()
            || value.getBytes(StandardCharsets.UTF_8).length > 255
            || value.indexOf('\0') >= 0) {
            throw new IllegalArgumentException(
                "spotType must be 1..255 UTF-8 bytes without NUL");
        }
        return value;
    }

    private static byte[] sha256(byte[] value) {
        try {
            return java.security.MessageDigest.getInstance("SHA-256")
                .digest(value);
        } catch (java.security.NoSuchAlgorithmException impossible) {
            throw new AssertionError(impossible);
        }
    }

    private static String inlineCreationIntent(byte[] value) {
        return "inline-v1:"
            + java.util.Base64.getUrlEncoder()
                .withoutPadding()
                .encodeToString(value);
    }

    private abstract class UserSpotCall {
        final String stableType;
        String meshName;
        ZLinkMessage request = ZLinkMessage.empty();
        boolean requestSet;
        Duration timeout;
        final java.util.concurrent.atomic.AtomicBoolean submitted =
            new java.util.concurrent.atomic.AtomicBoolean();

        UserSpotCall(String stableType) {
            this.stableType = stableType;
        }

        void setMesh(String value) {
            if (meshName != null) {
                throw new IllegalStateException("inMesh was already set");
            }
            meshName = requireStableType(value);
        }

        void setRequest(ZLinkMessage value) {
            if (requestSet) {
                throw new IllegalStateException("request was already set");
            }
            request = java.util.Objects.requireNonNull(value, "request");
            requestSet = true;
        }

        void setTimeout(Duration value) {
            if (timeout != null) {
                throw new IllegalStateException("timeout was already set");
            }
            if (value == null || value.isZero() || value.isNegative()) {
                throw new IllegalArgumentException(
                    "timeout must be positive");
            }
            timeout = value;
        }

        CompletionStage<ZLinkSpotCreateResult> submit(
            String spotId,
            boolean getOrCreate) {
            if (!submitted.compareAndSet(false, true)) {
                return CompletableFuture.failedFuture(
                    new IllegalStateException(
                        "User Spot call was already submitted"));
            }
            return submitUserSpot(
                spotId,
                stableType,
                meshName,
                request,
                timeout,
                getOrCreate);
        }
    }

    private final class CreateCall extends UserSpotCall
        implements systems.zlink.framework.spots.ZLinkSpotCreateCall {
        CreateCall(String stableType) {
            super(stableType);
        }

        @Override public CreateCall inMesh(String value) {
            setMesh(value); return this;
        }
        @Override public CreateCall request(Object value) {
            return request(ZLinkMessage.of(value));
        }
        @Override public CreateCall request(ZLinkMessage value) {
            setRequest(value); return this;
        }
        @Override public CreateCall timeout(Duration value) {
            setTimeout(value); return this;
        }
        @Override public CompletionStage<ZLinkSpotCreateResult> submit() {
            return submit(null, false);
        }
        @Override public CompletionStage<ZLinkSpotCreateResult> yield() {
            systems.zlink.framework.runtime.internal.handlers
                .ZLinkSuspendInvocationContext.requireYieldAllowed(
                    "Spot creation");
            return systems.zlink.framework.execution.ZLinkAsyncSerialQueue
                .yieldCurrent(submit());
        }
    }

    private final class GetOrCreateCall extends UserSpotCall
        implements systems.zlink.framework.spots.ZLinkSpotGetOrCreateCall {
        private final String spotId;

        GetOrCreateCall(String spotId, String stableType) {
            super(stableType);
            this.spotId = spotId;
        }

        @Override public GetOrCreateCall inMesh(String value) {
            setMesh(value); return this;
        }
        @Override public GetOrCreateCall request(Object value) {
            return request(ZLinkMessage.of(value));
        }
        @Override public GetOrCreateCall request(ZLinkMessage value) {
            setRequest(value); return this;
        }
        @Override public GetOrCreateCall timeout(Duration value) {
            setTimeout(value); return this;
        }
        @Override public CompletionStage<ZLinkSpotCreateResult> submit() {
            return submit(spotId, true);
        }
        @Override public CompletionStage<ZLinkSpotCreateResult> yield() {
            systems.zlink.framework.runtime.internal.handlers
                .ZLinkSuspendInvocationContext.requireYieldAllowed(
                    "Spot creation");
            return systems.zlink.framework.execution.ZLinkAsyncSerialQueue
                .yieldCurrent(submit());
        }
    }

    @Override
    public void close() {
        closeAsync();
    }

    public CompletionStage<Void> closeAsync() {
        beginClose();
        return spotLifecycle.closeAllAsync().handle((ignored, failure) -> {
            RuntimeException firstFailure = failure == null ? null
                : failure instanceof RuntimeException runtime ? runtime : new RuntimeException(failure);
            for (ZLinkInstanceSpotActivation activation :
                    instanceSpotActivations.values()) {
                firstFailure = closeRuntimeComponent(
                    () -> activation.close(
                        systems.zlink.framework.spots
                            .ZLinkSpotCloseReason.HOST_SHUTDOWN,
                        java.time.Instant.now()),
                    firstFailure);
            }
            instanceSpotActivations.clear();
            firstFailure = closeRuntimeComponent(publishers::close, firstFailure);
            for (ZLinkInternalSpotNode node : nodes) {
                firstFailure = closeRuntimeComponent(node::close, firstFailure);
            }
            timerExecutor.shutdownNow();
            infrastructureExecutor.shutdown();
            try {
                if (!infrastructureExecutor.awaitTermination(1, TimeUnit.SECONDS)) {
                    infrastructureExecutor.shutdownNow();
                    infrastructureExecutor.awaitTermination(4, TimeUnit.SECONDS);
                }
            } catch (InterruptedException interrupted) {
                infrastructureExecutor.shutdownNow();
                Thread.currentThread().interrupt();
                if (firstFailure == null) {
                    firstFailure = new ZLinkConfigurationException(
                        "failed to close Spot infrastructure executor", interrupted);
                } else {
                    firstFailure.addSuppressed(interrupted);
                }
            }
            workerPool.close();
            if (ownsContext) {
                firstFailure = closeRuntimeComponent(context::close, firstFailure);
            }
            if (firstFailure != null) {
                throw firstFailure;
            }
            return null;
        });
    }

    public void beginClose() {
        closing = true;
    }

    public CompletionStage<Void> beginDrain() {
        draining = true;
        drainInitialUserSpots = spotLifecycle.userSpotCount();
        spotLifecycle.sealApplicationAdmission();
        return CompletableFuture.completedFuture(null);
    }

    public void beginRelocation() {
        relocating = true;
    }

    public void cancelRelocation() {
        relocating = false;
    }

    public CompletionStage<Void> awaitDrainBarrier() {
        return spotLifecycle.awaitApplicationTurns();
    }

    public CompletionStage<Void> continueDrain() {
        return continueDrain(
            systems.zlink.framework.spots.ZLinkSpotCloseReason.HOST_SHUTDOWN,
            java.time.Instant.now());
    }

    public CompletionStage<Void> continueDrain(
        systems.zlink.framework.spots.ZLinkSpotCloseReason reason,
        java.time.Instant deadline) {
        return spotLifecycle.releaseRecreatableSpots(reason, deadline)
            .thenRun(this::recordDrainedRoomsIfComplete);
    }

    public int activeUserSpotCount() {
        return spotLifecycle.userSpotCount();
    }

    public List<String> activeUserSpotIds() {
        return spotLifecycle.userSpotIds();
    }

    ZLinkSpotRelocationReplyRoutes.Registration registerRelocationReply(
        byte[] acceptedRecord,
        ZLinkBackendReceived received,
        String spotId,
        long objectGeneration) {
        return relocationReplyRoutes.register(
            acceptedRecord, received, spotId, objectGeneration);
    }

    void bindCommittedRelocationReplies(
        Map<String, List<ZLinkAsyncSerialQueue.QueuedRecord>> journal,
        RoutingId targetNodeRid,
        long targetNodeGeneration,
        long targetAuthorityOwnerGeneration) {
        bindCommittedRelocationReplies(
            journal,
            targetNodeRid,
            targetNodeGeneration,
            Map.of("spot", targetAuthorityOwnerGeneration));
    }

    void bindCommittedRelocationReplies(
        Map<String, List<ZLinkAsyncSerialQueue.QueuedRecord>> journal,
        RoutingId targetNodeRid,
        long targetNodeGeneration,
        Map<String, Long> targetAuthorityOwnerGenerations) {
        List<byte[]> records = journal.getOrDefault("spot", List.of())
            .stream()
            .map(ZLinkAsyncSerialQueue.QueuedRecord::payload)
            .toList();
        relocationReplyRoutes.bindCommitted(
            records,
            targetNodeRid,
            targetNodeGeneration,
            targetAuthorityOwnerGenerations.getOrDefault("spot", 0L));
        journal.forEach((lane, queued) -> {
            if (!lane.startsWith("actor:")) {
                return;
            }
            String actorId = lane.substring("actor:".length());
            long generation = targetAuthorityOwnerGenerations.getOrDefault(
                actorId, 0L);
            relocationReplyRoutes.bindActorCommitted(
                queued.stream()
                    .map(ZLinkAsyncSerialQueue.QueuedRecord::payload)
                    .toList(),
                targetNodeRid,
                targetNodeGeneration,
                generation);
        });
    }

    void bindCanonicalRelocationReplies(
        Map<String, List<ZLinkAsyncSerialQueue.QueuedRecord>> journal,
        RoutingId targetNodeRid,
        long targetNodeGeneration,
        Map<String, ZLinkSpotRelocationReplyRoutes.CommittedFence> fences) {
        if (journal.containsKey("spot")) {
            relocationReplyRoutes.bindCommitted(
                journal.get("spot").stream()
                    .map(ZLinkAsyncSerialQueue.QueuedRecord::payload)
                    .toList(),
                targetNodeRid,
                targetNodeGeneration,
                java.util.Objects.requireNonNull(fences.get("spot"),
                    "Spot canonical reply fence"));
        }
        journal.forEach((lane, queued) -> {
            if (!lane.startsWith("actor:")) {
                return;
            }
            String actorId = lane.substring("actor:".length());
            relocationReplyRoutes.bindActorCommitted(
                queued.stream()
                    .map(ZLinkAsyncSerialQueue.QueuedRecord::payload)
                    .toList(),
                targetNodeRid,
                targetNodeGeneration,
                java.util.Objects.requireNonNull(fences.get(actorId),
                    "Actor canonical reply fence"));
        });
    }

    CompletionStage<ZLinkSpotRelocationReplyRoutes.Ack> relayRelocationReply(
        ZLinkSpotRelocationReplyRoutes.Relay relay,
        RoutingId transportSource) {
        return relocationReplyRoutes.relay(relay, transportSource);
    }

    ZLinkSpotRelocationReplyRoutes.CanonicalRoute
        canonicalRelocationReplyRoute(
            systems.zlink.framework.runtime.internal.service
                .ZLinkServiceRelocationWireCodec.ReplyRelay relay,
            RoutingId transportSource) {
        return relocationReplyRoutes.lookupCanonical(relay, transportSource);
    }

    CompletionStage<ZLinkSpotRelocationReplyRoutes.Ack>
        deliverCanonicalRelocationReply(
            ZLinkSpotRelocationReplyRoutes.CanonicalRoute route,
            List<byte[]> parts) {
        return relocationReplyRoutes.deliverCanonical(route, parts);
    }

    public String userSpotMeshName(String spotId) {
        return meshNameForSpot(spotId);
    }

    public boolean drainComplete() {
        boolean complete = spotLifecycle.userSpotsDrained()
            && nodes.stream().noneMatch(
                ZLinkInternalSpotNode::hasPendingActorRequests);
        if (complete) {
            recordDrainedRoomsIfComplete();
        }
        return complete;
    }

    private void recordDrainedRoomsIfComplete() {
        if (!spotLifecycle.userSpotsDrained()
            || !drainRoomsMetricRecorded.compareAndSet(false, true)) {
            return;
        }
        for (int index = 0; index < drainInitialUserSpots; index++) {
            systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics.increment(
                "zlink.drain.rooms.drained", java.util.Map.of());
        }
    }

    private void requireAcceptingNewState() {
        if (closing || draining || relocating) {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.REJECTED,
                "SPOT creation is rejected while the node is draining");
        }
    }

    private void requireAcceptingNewState(String spotId) {
        if ((closing || draining || relocating)
            && !spotLifecycle.hasUserSpot(spotId)) {
            requireAcceptingNewState();
        }
    }

    boolean isClosing() {
        return closing;
    }

    boolean isRelocating() {
        return relocating;
    }

    ScheduledFuture<?> scheduleInstanceSpotIdleCheck(
        Runnable task,
        long delayNanos) {
        return timerExecutor.schedule(
            task,
            Math.max(1L, delayNanos),
            TimeUnit.NANOSECONDS);
    }

    boolean isDraining() {
        return draining;
    }

    ZLinkActorSessionCoordinator actorSessions() {
        return actorSessions;
    }

    ZLinkSpotLifecycle spotLifecycle() {
        return spotLifecycle;
    }

    ZLinkActorSpotAdmission actorAdmissions() {
        return actorAdmissions;
    }

    ZLinkMessageSerializer serializerForSpot() {
        return serializer;
    }

    ZLinkInboundDispatchBudget inboundDispatchBudget() {
        return frameworkRegistration.inboundDispatchBudget();
    }

    private static RuntimeException closeRuntimeComponent(
        Runnable close,
        RuntimeException firstFailure) {
        try {
            close.run();
        } catch (ZlinkCloseException ignored) {
        } catch (RuntimeException error) {
            if (firstFailure == null) {
                return error;
            }
            firstFailure.addSuppressed(error);
        }
        return firstFailure;
    }

    public ZLinkInternalSpotNode primaryNode() {
        return primaryNode;
    }

    public ZLinkInternalSpotNode node(String nodeName) {
        ZLinkInternalSpotNode node = nodesByName.get(nodeName);
        if (node == null) {
            throw new ZLinkConfigurationException("unknown SpotNode: " + nodeName);
        }
        return node;
    }

    private ZLinkInternalSpotNode nodeByRid(RoutingId nodeRid) {
        for (ZLinkInternalSpotNode node : nodes) {
            if (node.routingId().equals(nodeRid)) {
                return node;
            }
        }
        throw new ZLinkConfigurationException("unknown SpotNode routing id: " + nodeRid);
    }

    public void attachActorRuntime(ZLinkActorRuntime actorRuntime) {
        actorRuntime.setDeferredJoinAcceptedRecovery(
            frameworkRegistration.locations().storeInstance() == null
                ? null
                : new systems.zlink.framework.runtime.internal.locations
                    .ZLinkProviderLocationRepository(
                        frameworkRegistration.locations().storeInstance()),
            frameworkRegistration.relocationStore());
        actorSessions.attach(
            actorRuntime,
            this::notifyEntrySpotActorCreated,
            systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext
                ::currentEntrySpotDispatch,
            this::notifySpotActorDisconnected,
            new ZLinkActorRuntime.SourceActorLeaver() {
                @Override
                public CompletionStage<Void> leave(ZLinkActor actor) {
                    return notifySourceActorLeftForRemoteMove(actor);
                }

                @Override
                public CompletionStage<Void> leave(
                    ZLinkActor actor,
                    ZLinkActorRuntime.LocalMoveSource source) {
                    return notifySourceActorLeftForLocalMove(actor, source);
                }
            },
            this::spotFor,
            this::meshNameForSpot);
        actorRuntime.setTransferBacklogRestorer((actor, packet) ->
            dispatchTransferBacklog(
                actorRuntime.currentRef(actor),
                packet.header(),
                packet.payload(),
                packet.acceptedJournalRecord()));
        ZLinkInternalMeshNode relocationMesh =
            routeMeshNodesByName.get(primaryNodeSourceName);
        actorAdmissions.attach(
            actorRuntime,
            this::isDraining,
            relocationMesh == null
                ? null
                : new systems.zlink.framework.runtime.actors
                    .ZLinkSessionRelocationPeerClient(relocationMesh));
        actorRuntime.setLocalJoinCompleter(new ZLinkActorRuntime.LocalJoinCompleter() {
            @Override
            public CompletionStage<Void> complete(ZLinkActor actor) {
                return actorAdmissions.completeLocalJoinFromCaller(actor);
            }

            @Override
            public void cancel(ZLinkActor actor) {
                actorAdmissions.cancelLocalJoin(actor);
            }
        });
    }

    public Map<String, ZLinkInternalSpotNode> nodesByName() {
        return Map.copyOf(nodesByName);
    }

    public boolean isSessionRelayRouteReady(RoutingId nodeRid) {
        if (!hasRoutingId(nodeRid)) {
            return true;
        }
        for (ZLinkInternalSpotNode node : nodes) {
            if (nodeRid.equals(node.routingId())) {
                return true;
            }
        }
        for (ZLinkInternalMeshNode node : routeMeshNodes) {
            try {
                boolean admitted = node.peers().stream().anyMatch(peer ->
                    nodeRid.equals(peer.routingId())
                        && peer.state() == MeshPeerState.ADMITTED);
                if (admitted) {
                    return true;
                }
            } catch (RuntimeException ignored) {
                // A transient status read cannot prove relay readiness.
            }
        }
        return manualRouterPeerNodeRids.contains(nodeRid)
            || autoConnectedRouterPeerNodeRids.contains(nodeRid);
    }

    public void markAutoConnectedRouterPeer(RoutingId nodeRid) {
        if (hasRoutingId(nodeRid)) {
            autoConnectedRouterPeerNodeRids.add(nodeRid);
        }
    }

    public void unmarkAutoConnectedRouterPeer(RoutingId nodeRid) {
        if (hasRoutingId(nodeRid)) {
            autoConnectedRouterPeerNodeRids.remove(nodeRid);
        }
    }

    private static boolean hasRoutingId(RoutingId routingId) {
        return routingId != null && routingId.size() > 0;
    }

    public ZLinkSpotOutbound outbound() {
        return outboundScope.ambient();
    }

    public ZLinkInstanceSpotCallRuntime instanceSpotCalls() {
        return new ZLinkInstanceSpotCallRuntime() {
            @Override
            public CompletionStage<Boolean> isStaleRoute(
                String spotId,
                SpotTransportAddress address) {
                var store = requireUserSpotLocationStore();
                String key = systems.zlink.framework.runtime.locations
                    .ZLinkAuthorityKeyCodec.spot(spotId);
                return store.read(key, () -> false).thenApply(read -> {
                    if (!(read instanceof systems.zlink.framework.runtime.internal.locations
                            .ZLinkAuthoritySnapshot snapshot)) {
                        return true;
                    }
                    var authority = userSpotAuthorities.decode(snapshot.payload())
                        .orElse(null);
                    boolean stale = authority == null
                        || authority.kind()
                            != systems.zlink.framework.runtime.locations
                                .ZLinkServiceAuthorityPayloadCodec.Kind.INSTANCE
                        || authority.state()
                            != systems.zlink.framework.runtime.locations
                                .ZLinkServiceAuthorityPayloadCodec.State.READY
                        || snapshot.allocation().state()
                            != systems.zlink.framework.runtime.internal.locations
                                .ZLinkPlacementAllocationState.ACTIVE
                        || snapshot.objectGeneration() != address.spotGeneration()
                        || snapshot.authorityOwnerGeneration()
                            != address.authorityOwnerGeneration()
                        || !authority.spotId().equals(address.spotId())
                        || !authority.nodeRid().equals(address.targetNodeRid())
                        || address.spotKind()
                            != systems.zlink.framework.spots.ZLinkSpotKind.INSTANCE;
                    return stale;
                });
            }

            @Override
            public CompletionStage<Void> send(
                String spotId,
                String stableType,
                String meshName,
                Message payload,
                Optional<String> packetName,
                String contentType,
                Map<String, String> metadata) {
                Duration timeout = defaultRequestTimeout;
                return activateInstanceSpot(
                        spotId, stableType, meshName, payload, packetName,
                        metadata, timeout)
                    .thenCompose(activation -> {
                        List<Message> parts = routeMessages.encode(
                            packetName, payload, contentType);
                        return activation.source()
                            .submitInstanceSpotSend(
                                activation.route(),
                                activation.stableType(),
                                primaryNode.entrySpot().spotId(),
                                systems.zlink.framework.runtime.messaging
                                    .ZLinkApplicationMetadata.copyOf(metadata).encode(),
                                parts,
                                timeout)
                            .thenAccept(ignored -> { })
                            .whenComplete((ignored, failure) -> {
                                parts.forEach(Message::close);
                            });
                    });
            }

            @Override
            public CompletionStage<List<Message>> request(
                String spotId,
                String stableType,
                String meshName,
                Message payload,
                Optional<String> packetName,
                String contentType,
                Map<String, String> metadata,
                Duration timeout) {
                Duration effective = timeout == null
                    ? defaultRequestTimeout : timeout;
                return activateInstanceSpot(
                        spotId, stableType, meshName, payload, packetName,
                        metadata, effective)
                    .thenCompose(activation -> {
                        List<Message> parts = routeMessages.encode(
                            packetName, payload, contentType);
                        return activation.source()
                            .requestInstanceSpot(
                                activation.route(),
                                activation.stableType(),
                                primaryNode.entrySpot().spotId(),
                                systems.zlink.framework.runtime.messaging
                                    .ZLinkApplicationMetadata.copyOf(metadata).encode(),
                                parts,
                                effective)
                            .whenComplete((ignored, failure) ->
                                parts.forEach(Message::close));
                    });
            }
        };
    }

    private CompletionStage<InstanceActivation> activateInstanceSpot(
        String spotId,
        String requestedType,
        String requestedMesh,
        Message payload,
        Optional<String> packetName,
        Map<String, String> metadata,
        Duration timeout) {
        requireAcceptingNewState();
        systems.zlink.framework.runtime.internal.spots
            .ZLinkSpotIdValidator.requireCallerAssignable(spotId);
        String meshName = resolveObjectMesh(requestedMesh);
        long deadline = System.currentTimeMillis() + timeout.toMillis();
        var store = requireUserSpotLocationStore();
        return resolveInstanceType(
                store, meshName, requestedType, deadline)
            .thenCompose(stableType -> selectInstanceSpotTarget(
                store, meshName, stableType, deadline))
            .thenCompose(target -> reserveInstanceSpot(
                store, meshName, spotId, target.stableType(),
                target.descriptor(), payload, packetName, metadata,
                deadline));
    }

    private CompletionStage<String> resolveInstanceType(
        systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository store,
        String meshName,
        String requestedType,
        long deadline) {
        if (requestedType != null) {
            return CompletableFuture.completedFuture(
                requireStableType(requestedType));
        }
        return store.listMeshNodes(
                meshName,
                new systems.zlink.framework.locations.ZLinkPageRequest(1000, null))
            .thenApply(page -> page.items().stream()
                .flatMap(node -> node.objectCapabilities().stream())
                .filter(capability -> capability.objectKind()
                    == systems.zlink.framework.locations
                        .ZLinkPlacementObjectKind.INSTANCE_SPOT)
                .map(systems.zlink.framework.locations
                    .ZLinkObjectCapability::stableType)
                .distinct()
                .toList())
            .thenApply(types -> {
                if (types.size() != 1) {
                    throw new IllegalStateException(types.isEmpty()
                        ? "No Instance Spot type is registered in Mesh: " + meshName
                        : "Instance Spot type is required when multiple types are registered");
                }
                return types.getFirst();
            });
    }

    private CompletionStage<InstanceTarget> selectInstanceSpotTarget(
        systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository store,
        String meshName,
        String stableType,
        long deadline) {
        return store.listMeshNodes(
                meshName,
                new systems.zlink.framework.locations.ZLinkPageRequest(1000, null))
            .thenCompose(page -> {
                List<systems.zlink.framework.runtime.internal.locations
                    .ZLinkMeshNodeDescriptor> candidates = page.items().stream()
                    .filter(node -> node.state()
                            == systems.zlink.framework.runtime.host
                                .ZLinkFrameworkRuntimeState.SERVING
                        && node.objectRole()
                            == systems.zlink.framework.locations
                                .ZLinkMeshNodeObjectRole.SERVER
                        && node.placementWeight() > 0
                        && node.objectCapabilities().stream().anyMatch(capability ->
                            capability.objectKind()
                                == systems.zlink.framework.locations
                                    .ZLinkPlacementObjectKind.INSTANCE_SPOT
                            && capability.stableType().equals(stableType)
                            && hasCapacity(node, capability)))
                    .toList();
                if (candidates.isEmpty()) {
                    if (System.currentTimeMillis() >= deadline) {
                        return CompletableFuture.failedFuture(
                            new IllegalStateException(
                                "No Ready Instance Spot placement target"));
                    }
                    return CompletableFuture.supplyAsync(
                            () -> null,
                            CompletableFuture.delayedExecutor(
                                10, TimeUnit.MILLISECONDS))
                        .thenCompose(ignored -> selectInstanceSpotTarget(
                            store, meshName, stableType, deadline));
                }
                Set<RoutingId> connectedTargets = connectedInstanceTargets(
                    meshName);
                List<systems.zlink.framework.runtime.internal.locations
                    .ZLinkMeshNodeDescriptor> readyCandidates =
                    preferConnectedInstanceTargets(candidates, connectedTargets);
                if (readyCandidates.size() != candidates.size()) {
                    traceInstanceLifecycle(
                        "target-selection mesh=" + meshName
                            + " stableType=" + stableType
                            + " candidates=" + candidates.size()
                            + " connected=" + readyCandidates.size());
                    candidates = readyCandidates;
                }
                long total = candidates.stream()
                    .mapToLong(node -> node.placementWeight()).sum();
                long choice = java.util.concurrent.ThreadLocalRandom.current()
                    .nextLong(total);
                for (var candidate : candidates) {
                    choice -= candidate.placementWeight();
                    if (choice < 0) {
                        return CompletableFuture.completedFuture(
                            new InstanceTarget(stableType, candidate));
                    }
                }
                return CompletableFuture.completedFuture(
                    new InstanceTarget(stableType, candidates.getLast()));
            });
    }

    static List<systems.zlink.framework.runtime.internal.locations
        .ZLinkMeshNodeDescriptor> preferConnectedInstanceTargets(
        List<systems.zlink.framework.runtime.internal.locations
            .ZLinkMeshNodeDescriptor> candidates,
        Set<RoutingId> connectedTargets) {
        List<systems.zlink.framework.runtime.internal.locations
            .ZLinkMeshNodeDescriptor> ready = candidates.stream()
            .filter(candidate -> connectedTargets.contains(candidate.rid()))
            .toList();
        return ready.isEmpty() ? candidates : ready;
    }

    private Set<RoutingId> connectedInstanceTargets(String meshName) {
        ZLinkInternalMeshNode source = routeMeshNodesByName.get(meshName);
        if (source == null) {
            return Set.of();
        }
        return source.peers().stream()
            .filter(peer -> peer.state() == MeshPeerState.ADMITTED)
            .map(peer -> peer.routingId())
            .collect(java.util.stream.Collectors.toUnmodifiableSet());
    }

    private CompletionStage<InstanceActivation> reserveInstanceSpot(
        systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository store,
        String meshName,
        String spotId,
        String stableType,
        systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor target,
        Message payload,
        Optional<String> packetName,
        Map<String, String> metadata,
        long deadline) {
        byte[] requestBytes = payload.toByteArray();
        var owner = new systems.zlink.framework.runtime.internal.locations
            .ZLinkLocationOwnerToken(target.ownerId(), target.leaseGeneration());
        byte[] creating = userSpotAuthorities.encodeInstance(
            systems.zlink.framework.runtime.locations
                .ZLinkServiceAuthorityPayloadCodec.State.CREATING,
            stableType, spotId, target.ownerId(), target.leaseGeneration(),
            meshName, target.rid(), target.lifecycleGeneration());
        var request = new systems.zlink.framework.runtime.internal.locations
            .ZLinkObjectReservationRequest(
                systems.zlink.framework.locations
                    .ZLinkPlacementObjectKind.INSTANCE_SPOT,
                systems.zlink.framework.runtime.locations
                    .ZLinkAuthorityKeyCodec.spot(spotId),
                stableType,
                inlineCreationIntent(requestBytes),
                sha256(requestBytes),
                requestBytes.length,
                descriptorKey(target),
                target.lifecycleGeneration(),
                owner,
                creating,
                systems.zlink.framework.runtime.internal.locations
                    .ZLinkPlacementCapacityBundle.spot(
                        systems.zlink.framework.locations
                            .ZLinkPlacementObjectKind.INSTANCE_SPOT,
                        stableType,
                        1));
        return store.reserve(request, () -> false)
            .thenCompose(result -> {
                systems.zlink.framework.runtime.internal.locations
                    .ZLinkObjectReservation reservation;
                if (result instanceof systems.zlink.framework.runtime.internal.locations
                        .ZLinkObjectReserved reserved) {
                    reservation = reserved.reservation();
                } else if (result instanceof systems.zlink.framework.runtime.internal.locations
                        .ZLinkObjectAlreadyExists exists) {
                    return activationFromExisting(
                        exists.current(), stableType, meshName);
                } else if (result instanceof systems.zlink.framework.runtime.internal.locations
                        .ZLinkObjectConflict conflict
                    && conflict.current() instanceof systems.zlink.framework.runtime.internal.locations
                        .ZLinkAuthoritySnapshot current) {
                    return awaitInstanceReady(
                        store,
                        systems.zlink.framework.runtime.locations
                            .ZLinkAuthorityKeyCodec.spot(spotId),
                        current,
                        stableType,
                        meshName,
                        deadline);
                } else if (result instanceof systems.zlink.framework.runtime.internal.locations
                        .ZLinkObjectTypeMismatch) {
                    return CompletableFuture.failedFuture(
                        new IllegalStateException(
                            "Instance Spot type does not match"));
                } else {
                    return CompletableFuture.failedFuture(
                        new IllegalStateException(
                            "Instance Spot reservation failed: "
                                + result.getClass().getSimpleName()));
                }
                var route = new systems.zlink.framework.runtime.internal.service
                    .ZLinkServiceM6BWireCodec.InstanceRouteFence(
                        target.rid(), target.lifecycleGeneration(), spotId,
                        reservation.objectGeneration(), target.ownerId(),
                        reservation.authorityOwnerGeneration(),
                        target.leaseGeneration(), reservation.storeVersion());
                ZLinkInternalMeshNode source = routeMeshNodesByName.get(meshName);
                if (source == null) {
                    return CompletableFuture.failedFuture(
                        new IllegalStateException(
                            "Object client Mesh is not configured: " + meshName));
                }
                if (source.status().routingId().equals(target.rid())) {
                    source.registerInstanceIntent(stableType, route);
                }
                return CompletableFuture.completedFuture(
                    new InstanceActivation(source, route, stableType));
            });
    }

    private CompletionStage<InstanceActivation> awaitInstanceReady(
        systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository store,
        String key,
        systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot snapshot,
        String stableType,
        String meshName,
        long deadline) {
        var authority = userSpotAuthorities.decode(snapshot.payload())
            .orElseThrow(() -> new IllegalStateException(
                "invalid Instance Spot authority"));
        if (authority.kind()
                != systems.zlink.framework.runtime.locations
                    .ZLinkServiceAuthorityPayloadCodec.Kind.INSTANCE
            || !authority.stableType().equals(stableType)
            || !authority.meshName().equals(meshName)) {
            return CompletableFuture.failedFuture(
                new IllegalStateException(
                    "Instance Spot type or Mesh does not match"));
        }
        if (authority.state()
                == systems.zlink.framework.runtime.locations
                    .ZLinkServiceAuthorityPayloadCodec.State.READY
            && snapshot.allocation().state()
                == systems.zlink.framework.runtime.internal.locations
                    .ZLinkPlacementAllocationState.ACTIVE) {
            return activationFromExisting(snapshot, stableType, meshName);
        }
        if (authority.state()
                == systems.zlink.framework.runtime.locations
                    .ZLinkServiceAuthorityPayloadCodec.State.CLOSING) {
            return instanceSpotNotFound(
                "Instance Spot is closing: " + authority.spotId());
        }
        if (System.currentTimeMillis() >= deadline) {
            return CompletableFuture.failedFuture(
                new java.util.concurrent.TimeoutException(
                    "Instance Spot did not finish activation before the request deadline"));
        }
        return CompletableFuture.supplyAsync(
                () -> null,
                CompletableFuture.delayedExecutor(
                    10, TimeUnit.MILLISECONDS))
            .thenCompose(ignored -> store.read(key, () -> false))
            .thenCompose(read -> read instanceof systems.zlink.framework.runtime.internal.locations
                    .ZLinkAuthoritySnapshot current
                ? awaitInstanceReady(
                    store, key, current, stableType, meshName, deadline)
                : CompletableFuture.failedFuture(
                    new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.NOT_FOUND,
                        "Instance Spot authority is no longer available")));
    }

    private CompletionStage<InstanceActivation> activationFromExisting(
        systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot snapshot,
        String stableType,
        String meshName) {
        var authority = userSpotAuthorities.decode(snapshot.payload())
            .orElseThrow(() -> new IllegalStateException(
                "invalid Instance Spot authority"));
        if (authority.kind()
                != systems.zlink.framework.runtime.locations
                    .ZLinkServiceAuthorityPayloadCodec.Kind.INSTANCE
            || !authority.stableType().equals(stableType)
            || !authority.meshName().equals(meshName)
            || authority.state()
                != systems.zlink.framework.runtime.locations
                    .ZLinkServiceAuthorityPayloadCodec.State.READY
            || snapshot.allocation().state()
                != systems.zlink.framework.runtime.internal.locations
                    .ZLinkPlacementAllocationState.ACTIVE) {
            return CompletableFuture.failedFuture(
                new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.NOT_FOUND,
                    "Instance Spot is not Ready"));
        }
        ZLinkInternalMeshNode source = routeMeshNodesByName.get(meshName);
        var route = new systems.zlink.framework.runtime.internal.service
            .ZLinkServiceM6BWireCodec.InstanceRouteFence(
                authority.nodeRid(), authority.nodeGeneration(),
                authority.spotId(), snapshot.objectGeneration(),
                snapshot.ownerId(), snapshot.authorityOwnerGeneration(),
                snapshot.ownerLeaseGeneration(), snapshot.storeVersion());
        return CompletableFuture.completedFuture(
            new InstanceActivation(source, route, stableType));
    }

    private static <T> CompletionStage<T> instanceSpotNotFound(
        String message) {
        return CompletableFuture.failedFuture(
            new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.NOT_FOUND,
                message));
    }

    private CompletionStage<Void> activateInstanceSpotTarget(
        String meshName,
        systems.zlink.framework.runtime.internal.configuration
            .ZLinkObjectFactoryRegistration.RelocatableInstanceSpotFactory<?>
                factory,
        systems.zlink.framework.runtime.internal.service
            .ZLinkServiceM6BWireCodec.InstanceRouteFence route,
        ZLinkBackendSpot backendSpot) {
        var store = requireUserSpotLocationStore();
        String key = systems.zlink.framework.runtime.locations
            .ZLinkAuthorityKeyCodec.spot(route.targetSpotId());
        return store.read(key, () -> false).thenCompose(read -> {
            if (!(read instanceof systems.zlink.framework.runtime.internal.locations
                    .ZLinkAuthoritySnapshot snapshot)) {
                return CompletableFuture.failedFuture(
                    new IllegalStateException("Instance Spot reservation is missing"));
            }
            var authority = userSpotAuthorities.decode(snapshot.payload())
                .orElseThrow(() -> new IllegalStateException(
                    "invalid Instance Spot authority"));
            var allocation = snapshot.allocation();
            var pending = snapshot.pendingCreation().orElseThrow(
                () -> new IllegalStateException(
                    "Instance Spot creation projection is missing"));
            if (authority.kind()
                    != systems.zlink.framework.runtime.locations
                        .ZLinkServiceAuthorityPayloadCodec.Kind.INSTANCE
                || authority.state()
                    != systems.zlink.framework.runtime.locations
                        .ZLinkServiceAuthorityPayloadCodec.State.CREATING
                || !factory.stableType().equals(authority.stableType())
                || !meshName.equals(authority.meshName())
                || !route.targetSpotId().equals(authority.spotId())
                || !route.targetNodeRid().equals(authority.nodeRid())
                || route.targetNodeGeneration() != authority.nodeGeneration()
                || route.objectGeneration() != snapshot.objectGeneration()
                || route.authorityOwnerGeneration()
                    != snapshot.authorityOwnerGeneration()
                || route.leaseGeneration() != snapshot.ownerLeaseGeneration()
                || !route.ownerId().equals(snapshot.ownerId())
                || !route.storeVersion().equals(snapshot.storeVersion())
                || allocation.objectKind()
                    != systems.zlink.framework.locations
                        .ZLinkPlacementObjectKind.INSTANCE_SPOT
                || !allocation.stableType().equals(factory.stableType())) {
                return CompletableFuture.failedFuture(
                    new IllegalStateException(
                        "Instance Spot reservation fence is stale"));
            }
            var reservation = new systems.zlink.framework.runtime.internal.locations
                .ZLinkObjectReservation(
                    key,
                    snapshot.storeVersion(),
                    snapshot.objectGeneration(),
                    snapshot.authorityOwnerGeneration(),
                    pending.reservationId(),
                    allocation.descriptor(),
                    allocation.descriptorLifecycleGeneration(),
                    new systems.zlink.framework.runtime.internal.locations
                        .ZLinkLocationOwnerToken(
                            snapshot.ownerId(),
                            snapshot.ownerLeaseGeneration()));
            return activationFactory.activateInstance(
                    meshName, factory.spotType(), backendSpot)
                .thenCompose(activation -> {
                    byte[] ready = userSpotAuthorities.encodeInstance(
                        systems.zlink.framework.runtime.locations
                            .ZLinkServiceAuthorityPayloadCodec.State.READY,
                        factory.stableType(),
                        route.targetSpotId(),
                        snapshot.ownerId(),
                        snapshot.ownerLeaseGeneration(),
                        meshName,
                        route.targetNodeRid(),
                        route.targetNodeGeneration());
                    return store.commit(reservation, ready, () -> false)
                        .thenCompose(result -> {
                            if (result
                                    != systems.zlink.framework.runtime.internal
                                        .locations.ZLinkObjectCommitResult.COMMITTED
                                && result
                                    != systems.zlink.framework.runtime.internal
                                        .locations.ZLinkObjectCommitResult
                                            .ALREADY_COMMITTED) {
                                activation.close(
                                    systems.zlink.framework.spots
                                        .ZLinkSpotCloseReason.EXPLICIT_CLOSE,
                                    java.time.Instant.now());
                                return CompletableFuture.failedFuture(
                                    new IllegalStateException(
                                        "Instance Spot Ready commit lost its reservation"));
                            }
                            activation.setAuthorityFence(
                                route.ownerId(),
                                route.leaseGeneration(),
                                route.authorityOwnerGeneration(),
                                route.targetNodeGeneration());
                            instanceSpotActivations.put(
                                route.targetSpotId(), activation);
                            activation.startIdleEviction(
                                instanceSpotIdleTimeouts.getOrDefault(
                                    meshName, Duration.ZERO));
                            return CompletableFuture.completedFuture(null);
                        });
                });
        });
    }

    private record InstanceTarget(
        String stableType,
        systems.zlink.framework.runtime.internal.locations
            .ZLinkMeshNodeDescriptor descriptor) {
    }

    private record InstanceActivation(
        ZLinkInternalMeshNode source,
        systems.zlink.framework.runtime.internal.service
            .ZLinkServiceM6BWireCodec.InstanceRouteFence route,
        String stableType) {
    }

    public ZLinkSpotPublisherClient publisherClient() {
        return publishers.client();
    }

    public void setLocationLifecycle(ZLinkLocationLifecycle lifecycle) {
        spotLocations.setLifecycle(lifecycle);
    }

    public CompletionStage<Void> claimEntrySpotLocations() {
        return spotLocations.claimEntrySpotsAsync();
    }

    public CompletionStage<Optional<Message>> dispatchLocalSessionActor(
        ZLinkBackendActorRef actorRef,
        ZLinkStreamHeader header,
        Message payload) {
        return actorSessions.dispatchLocalSession(
            actorRef,
            header,
            payload,
            spotId -> spotSurfaceFor(spotId) != null,
            local -> dispatchLocalSessionActor(
                actorRef,
                header,
                payload,
                local));
    }

    CompletionStage<Optional<Message>> dispatchTransferBacklog(
        ZLinkBackendActorRef actorRef,
        ZLinkStreamHeader header,
        Message payload,
        byte[] acceptedJournalRecord) {
        return actorSessions.dispatchTransferBacklog(
            actorRef,
            header,
            payload,
            acceptedJournalRecord,
            spotId -> spotSurfaceFor(spotId) != null,
            local -> dispatchLocalSessionActor(
                actorRef,
                header,
                payload,
                local));
    }

    CompletionStage<Optional<byte[]>> replayPreparedActor(
        ZLinkSpotLifecycle.PreparedUserSpot preparedSpot,
        ZLinkActorRuntime.PreparedTransferredActor preparedActor,
        ZLinkActorAcceptedJournal.Record record) {
        ZLinkActor actor = preparedActor.actor();
        Object spotSurface = spotLifecycle.preparedSpot(preparedSpot);
        return replayPreparedActorOnSurface(
            spotSurface, preparedActor, record);
    }

    CompletionStage<Optional<byte[]>> replayPreparedActorAtSpot(
        String spotId,
        ZLinkActorRuntime.PreparedTransferredActor preparedActor,
        ZLinkActorAcceptedJournal.Record record) {
        Object spotSurface = spotSurfaceFor(spotId);
        if (spotSurface == null) {
            return CompletableFuture.failedFuture(
                new ZLinkConfigurationException(
                    "target execution Spot is unavailable: " + spotId));
        }
        return replayPreparedActorOnSurface(
            spotSurface, preparedActor, record);
    }

    private CompletionStage<Optional<byte[]>> replayPreparedActorOnSurface(
        Object spotSurface,
        ZLinkActorRuntime.PreparedTransferredActor preparedActor,
        ZLinkActorAcceptedJournal.Record record) {
        ZLinkActor actor = preparedActor.actor();
        boolean request = record.header().requestSequence().isPresent();
        SpotActorPacketHandlerRegistration handler = resolveActorPacketHandler(
            record.header().packetName(),
            spotSurface,
            request
                ? ZLinkScannedHandlerKind.ACTOR_REQUEST
                : ZLinkScannedHandlerKind.ACTOR_SEND);
        if (handler == null
            || request != (handler.kind()
                == ZLinkScannedHandlerKind.ACTOR_REQUEST)
            || !handler.actorType().isInstance(actor)) {
            return CompletableFuture.failedFuture(
                new ZLinkConfigurationException(
                    "staged Actor handler is unavailable or mismatched: "
                        + record.header().packetName()));
        }
        Message payload = Message.from(record.payload());
        CompletionStage<Optional<Message>> dispatched;
        try {
            dispatched = dispatchLocalSessionActorPacket(
                handler,
                spotSurface,
                actor,
                payload,
                contentTypeFor(record.header().codec()),
                record.header().metadata());
        } catch (RuntimeException failure) {
            payload.close();
            return CompletableFuture.failedFuture(failure);
        }
        return dispatched.thenApply(reply -> reply.map(value -> {
                try (value) {
                    return value.toByteArray();
                }
            }))
            .whenComplete((ignored, failure) -> payload.close());
    }

    Optional<Message> replyTransferredRequestDirect(
        ZLinkBackendActorRef targetActorRef,
        ZLinkStreamHeader requestHeader,
        ZLinkActorReplyRoute replyRoute,
        Optional<Message> reply) {
        if (replyRoute == null || reply.isEmpty()) {
            return reply;
        }
        traceMessageFlow(
            ZLinkMessageFlowOutcome.REPLIED,
            ZLinkDispatchErrorSurface.SPOT_ACTOR,
            ZLinkDispatchMessageKind.ACTOR_REQUEST,
            "handoff_direct_reply",
            null,
            null,
            Long.toUnsignedString(replyRoute.requestId()),
            replyRoute.sourceNodeRid().toString(),
            null,
            replyRoute.actorRef().actorId());
        try (Message payload = reply.get()) {
            ZLinkStreamHeader responseHeader = ZLinkStreamHeader.createResponse(
                requestHeader,
                requestHeader.codec(),
                java.util.EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
                requestHeader.packetName(),
                Map.of());
            try (Message frame = Message.from(
                    systems.zlink.framework.runtime.streams.ZLinkStreamFrameCodec.encode(
                        responseHeader,
                        payload.toByteArray()))) {
                try {
                    primaryNode.sendActorBoundSession(
                        targetActorRef,
                        List.of(frame),
                        systems.zlink.contracts.sockets.SendFlags.NONE);
                } catch (RuntimeException error) {
                    throw new ZLinkConfigurationException(
                        "handoff direct reply failed sourceNode="
                            + replyRoute.sourceNodeRid()
                            + " sourceSession=" + replyRoute.sourceSessionRid()
                            + " requestId=" + Long.toUnsignedString(replyRoute.requestId()),
                        error);
                }
            }
        }
        return Optional.empty();
    }

    private CompletionStage<Optional<Message>> dispatchLocalSessionActor(
        ZLinkBackendActorRef actorRef,
        ZLinkStreamHeader header,
        Message payload,
        ZLinkActorSessionCoordinator.LocalDispatch local) {
        ZLinkActor actor = local.actor();
        Object spotSurface = localActorSpotSurface(actor);
        boolean isRequest = header.requestSequence().isPresent()
            || header.kind() == ZLinkStreamMessageKind.REQUEST;
        SpotActorPacketHandlerRegistration handler = resolveActorPacketHandler(
            header.packetName(),
            spotSurface,
            isRequest
                ? ZLinkScannedHandlerKind.ACTOR_REQUEST
                : ZLinkScannedHandlerKind.ACTOR_SEND);
        if (handler == null
            || isRequest != (handler.kind() == ZLinkScannedHandlerKind.ACTOR_REQUEST)) {
            ZLinkConfigurationException error = new ZLinkConfigurationException(
                handler == null
                    ? "actor packet handler is not registered: " + header.packetName()
                    : "actor packet kind does not match handler kind: " + header.packetName());
            reportDispatchError(DispatchFailureReport.of(
                    ZLinkDispatchErrorSurface.SPOT_ACTOR,
                    isRequest
                        ? ZLinkDispatchMessageKind.ACTOR_REQUEST
                        : ZLinkDispatchMessageKind.ACTOR_SEND,
                    ZLinkDispatchErrorReason.HANDLER_MISSING,
                    isRequest
                        ? ZLinkDispatchErrorAction.REPLY_ERROR
                        : ZLinkDispatchErrorAction.DROP)
                .packetName(header.packetName())
                .spotId(local.joinedSpotId().orElse(null))
                .actorId(actor.context().actorId())
                .correlationId(header.requestSequence().map(Object::toString).orElse(null))
                .error(error));
            return CompletableFuture.failedFuture(error);
        }
        if (!handler.actorType().isInstance(actor)) {
            return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                "actor packet handler target type does not match actor: " + actorRef.actorId()));
        }
        return dispatchLocalSessionActorPacket(
            handler,
            spotSurface,
            actor,
            payload,
            frameworkRegistration.codecs()
                .streamContentType(header.codec())
                .orElse(null),
            header.metadata());
    }

    public void drainRoutedDispatchQueues() {
        if (closing) {
            return;
        }
        spotLifecycle.drainRoutedDispatchQueues();
    }

    private <T> CompletionStage<T> withCurrentOutbound(
        DefaultSpotOutbound outbound,
        Supplier<CompletionStage<T>> action) {
        CompletableFuture<T> result = new CompletableFuture<>();
        var flow = systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext.current();
        Object entryDispatchContext = systems.zlink.framework.runtime.internal.handlers
            .ZLinkSuspendInvocationContext.currentEntrySpotDispatch();
        try {
            systems.zlink.framework.execution.ZLinkAsyncSerialQueue
                .propagateCurrent(handlerExecutor).execute(() -> {
                outboundScope.run(outbound, () -> {
                    try (systems.zlink.framework.runtime.internal.handlers
                             .ZLinkSuspendInvocationContext.Scope ignored =
                             systems.zlink.framework.runtime.internal.handlers
                                 .ZLinkSuspendInvocationContext
                                 .enterEntrySpotDispatch(entryDispatchContext)) {
                        action.get().whenComplete((value, error) -> {
                            systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext.run(flow, () -> {
                                if (error != null) {
                                    result.completeExceptionally(error);
                                } else {
                                    result.complete(value);
                                }
                            });
                        });
                    } catch (RuntimeException ex) {
                        result.completeExceptionally(ex);
                    }
                    return null;
                });
            });
        } catch (RuntimeException ex) {
            result.completeExceptionally(ex);
        }
        return result;
    }

    private CompletionStage<Void> dispatchActorPacketToHandler(
        DefaultSpotOutbound outbound,
        SpotActorPacketHandlerRegistration handler,
        Object spotSurface,
        ZLinkActor actor,
        ActorPacketFrames.Header packetHeader,
        ZLinkBackendActorReceived headerPart,
        Message payload,
        String replyFailureMessage,
        ZLinkInboundDispatchBudget.Lease lease) {
        boolean request = handler.kind() == ZLinkScannedHandlerKind.ACTOR_REQUEST;
        CompletionStage<ZLinkInboundDispatchBudget.CompletionPermit> permitStage =
            request
                ? inboundDispatchBudget().acquireCompletionPermit()
                : CompletableFuture.completedFuture(null);
        return permitStage.thenCompose(permit -> {
            try {
                lease.handlerStarted();
                return dispatchActorPacketToHandlerBody(
                        outbound,
                        handler,
                        spotSurface,
                        actor,
                        packetHeader,
                        headerPart,
                        payload,
                        replyFailureMessage)
                    .whenComplete((ignored, error) -> {
                        if (permit != null) {
                            permit.close();
                        }
                    });
            } catch (RuntimeException failure) {
                if (permit != null) {
                    permit.close();
                }
                return CompletableFuture.failedFuture(failure);
            }
        });
    }

    private CompletionStage<Void> dispatchActorPacketToHandlerBody(
        DefaultSpotOutbound outbound,
        SpotActorPacketHandlerRegistration handler,
        Object spotSurface,
        ZLinkActor actor,
        ActorPacketFrames.Header packetHeader,
        ZLinkBackendActorReceived headerPart,
        Message payload,
        String replyFailureMessage) {
        var actorFlow = systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext.current();
        boolean noBindRequest = isNoBindActorRequest(packetHeader, headerPart);
        byte[] acceptedRecord = headerPart.acceptedJournalRecord();
        ZLinkSpotRelocationReplyRoutes.Registration relocationReply =
            acceptedRecord.length == 0
                ? null
                : relocationReplyRoutes.registerActor(
                    acceptedRecord,
                    actor.context().actorId(),
                    headerPart.actor().generation(),
                    parts -> deliverRelocatedActorReply(
                        actor.context().actorId(),
                        packetHeader,
                        headerPart.actor(),
                        headerPart.sourceNodeRid(),
                        headerPart.sourceSessionRid(),
                        headerPart.requestId(),
                        headerPart.flags(),
                        noBindRequest,
                        parts),
                    () -> { });
        traceActorSession("dispatch-actor-packet"
            + " actor=" + actor.context().actorId()
            + " packet=" + packetHeader.packetName()
            + " requestSeq=" + packetHeader.requestSeq().map(Object::toString).orElse(null)
            + " sourceNode=" + headerPart.sourceNodeRid()
            + " sourceSession=" + headerPart.sourceSessionRid()
            + " noBind=" + noBindRequest
            + " hasBound=" + actorSessions.hasBoundSession(actor));
        boolean actorIsRequest = handler.kind() == ZLinkScannedHandlerKind.ACTOR_REQUEST;
        String actorPacketName = packetHeader.packetName();
        String actorId = actor.context().actorId();
        ZLinkDispatchMessageKind actorKind = actorIsRequest
            ? ZLinkDispatchMessageKind.ACTOR_REQUEST
            : ZLinkDispatchMessageKind.ACTOR_SEND;
        traceMessageFlow(
            ZLinkMessageFlowOutcome.RECEIVED,
            ZLinkDispatchErrorSurface.SPOT_ACTOR,
            actorKind,
            actorPacketName,
            null,
            null,
            packetHeader.requestSeq().map(String::valueOf).orElse(null),
            null,
            null,
            actorId);
        CompletionStage<Optional<Message>> stage = withCurrentOutbound(
            outbound,
            () -> actorSessions.runPacketTurn(
                actor,
                packetHeader.requestSeq().isPresent(),
                noBindRequest,
                headerPart,
                primaryNode,
                () -> actorIsRequest
                    ? invokeActorRequestHandler(
                        handler,
                        spotSurface,
                        actor,
                        payload,
                        headerPart.contentType(),
                        packetHeader.metadata())
                    : invokeActorSendHandler(
                        handler,
                        spotSurface,
                        actor,
                        payload,
                        headerPart.contentType(),
                        packetHeader.metadata())
                        .thenApply(ignored -> Optional.empty()),
                relocationReply == null
                    ? () -> { }
                    : relocationReply::releaseForRelocation));
        return stage.handle((reply, error) -> {
                if (error != null) {
                    reportDispatchError(DispatchFailureReport.of(
                            ZLinkDispatchErrorSurface.SPOT_ACTOR,
                            actorKind,
                            ZLinkDispatchErrorReason.HANDLER_EXCEPTION,
                            actorIsRequest
                                ? ZLinkDispatchErrorAction.REPLY_ERROR
                                : ZLinkDispatchErrorAction.DROP)
                        .packetName(actorPacketName)
                        .actorId(actorId)
                        .sourceRid(headerPart.sourceNodeRid())
                        .correlationId(packetHeader.requestSeq().map(String::valueOf).orElse(null))
                        .error(error));
                    return Optional.of(new ActorDispatchReply(
                        ActorPacketFrames.encodeError(packetHeader, error),
                        true));
                }
                return reply.map(message -> new ActorDispatchReply(message, false));
            })
            .thenCompose(reply -> {
                if (reply.isEmpty()) {
                    return java.util.concurrent.CompletableFuture.completedFuture(null);
                }
                byte[] frameBytes;
                try (Message frame = reply.get().streamFrame()
                    ? reply.get().message()
                    : ActorPacketFrames.encodeReply(
                        packetHeader,
                        reply.get().message(),
                        ZLinkPacketNames.resolve(handler.replyType()),
                        streamCodecFor(handler.replyType()))) {
                    frameBytes = frame.toByteArray();
                }
                if (noBindRequest) {
                    try (Message frame = Message.from(frameBytes)) {
                        primaryNode.replyActorNoBind(
                            new ZLinkBackendActorRef(
                                headerPart.actor().nodeRid(),
                                actor.context().actorId(),
                                headerPart.actor().generation()),
                            headerPart.sourceNodeRid(),
                            headerPart.sourceSessionRid(),
                            headerPart.requestId(),
                            headerPart.flags(),
                            List.of(frame));
                    }
                    return java.util.concurrent.CompletableFuture.completedFuture(null);
                }
                return sendActorBoundSessionWithRetry(
                    primaryNode,
                    new ZLinkBackendActorRef(
                        headerPart.actor().nodeRid(),
                        actor.context().actorId(),
                        headerPart.actor().generation()),
                    actor.context().actorId(),
                    frameBytes,
                    replyFailureMessage);
            })
            .whenComplete((ignored, error) -> {
                if (relocationReply != null) {
                    relocationReply.completeLocal();
                }
                systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext.run(actorFlow, () -> {
                    if (error == null) {
                        ZLinkMessageFlowOutcome phase = actorIsRequest
                            ? ZLinkMessageFlowOutcome.REPLIED
                            : ZLinkMessageFlowOutcome.DISPATCHED;
                        traceMessageFlow(
                            phase,
                            ZLinkDispatchErrorSurface.SPOT_ACTOR,
                            actorKind,
                            actorPacketName,
                            null,
                            null,
                            packetHeader.requestSeq().map(String::valueOf).orElse(null),
                            null,
                            null,
                            actorId);
                    }
                });
            });
    }

    private CompletionStage<Void> deliverRelocatedActorReply(
        String actorId,
        ActorPacketFrames.Header packetHeader,
        ZLinkBackendActorRef actorRef,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        long requestId,
        int flags,
        boolean noBindRequest,
        List<byte[]> parts) {
        if (parts.size() != 1) {
            return CompletableFuture.failedFuture(
                new IllegalArgumentException(
                    "relocated Actor reply requires one payload part"));
        }
        byte[] frameBytes;
        try (Message payload = Message.from(parts.getFirst());
             Message frame = ActorPacketFrames.encodeReply(
                 packetHeader, payload)) {
            frameBytes = frame.toByteArray();
        }
        if (noBindRequest) {
            try (Message frame = Message.from(frameBytes)) {
                primaryNode.replyActorNoBind(
                    actorRef,
                    sourceNodeRid,
                    sourceSessionRid,
                    requestId,
                    flags,
                    List.of(frame));
            }
            return CompletableFuture.completedFuture(null);
        }
        return sendActorBoundSessionWithRetry(
            primaryNode,
            actorRef,
            actorId,
            frameBytes,
            "relocated Actor bound Session reply failed");
    }

    private ZLinkStreamCodec streamCodecFor(Class<?> payloadType) {
        String contentType = frameworkRegistration.codecs().contentTypeFor(payloadType);
        return frameworkRegistration.codecs().streamCodec(contentType)
            .orElse(ZLinkStreamCodec.JSON);
    }

    private String contentTypeFor(ZLinkStreamCodec codec) {
        return frameworkRegistration.codecs().streamContentType(codec).orElse(null);
    }

    private CompletionStage<Void> invokeActorSendHandler(
        SpotActorPacketHandlerRegistration registration,
        Object spotSurface,
        ZLinkActor actor,
        Message payload,
        String contentType,
        Map<String, String> metadata) {
        return spotHandlerInvoker.invokeActorSend(
            registration,
            spotSurface,
            actor,
            payload,
            contentType,
            metadata,
            handlerType -> systems.zlink.framework.runtime.internal.handlers
                .ZLinkActorHandlerInstances.instance(actor, handlerType),
            "failed to invoke Spot actor send handler");
    }

    private CompletionStage<Optional<Message>> invokeActorRequestHandler(
        SpotActorPacketHandlerRegistration registration,
        Object spotSurface,
        ZLinkActor actor,
        Message payload,
        String contentType,
        Map<String, String> metadata) {
        return spotHandlerInvoker.invokeActorRequest(
            registration,
            spotSurface,
            actor,
            payload,
            contentType,
            metadata,
            handlerType -> systems.zlink.framework.runtime.internal.handlers
                .ZLinkActorHandlerInstances.instance(actor, handlerType),
            "failed to invoke Spot actor request handler");
    }

    @SuppressWarnings({"rawtypes", "unchecked"})
    private CompletionStage<Void> notifySpotActorLifecycle(
        Object spotSurface,
        ZLinkActor actor,
        boolean joined) {
        if (spotSurface instanceof ZLinkSpot spot) {
            DefaultSpotContext context = spotLifecycle.contextFor(spot);
            if (context == null) {
                return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                    "Spot activation context is unavailable: "
                        + spot.getClass().getName()));
            }
            return context.runLifecycleExecution(() ->
                withCurrentOutbound(
                    context.dispatchOutbound(),
                    () -> joined
                        ? ZLinkHandlerStages.fromStageSupplier(
                            () -> spot.onJoinedActor(actor))
                        : ZLinkHandlerStages.fromStageSupplier(
                            () -> spot.onLeaveActor(actor))));
        }
        if (spotSurface instanceof ZLinkEntrySpot entrySpot) {
            return joined
                ? ZLinkHandlerStages.fromStageSupplier(() -> entrySpot.onJoinedActor(actor))
                : ZLinkHandlerStages.fromStageSupplier(() -> entrySpot.onLeaveActor(actor));
        }
        return java.util.concurrent.CompletableFuture.completedFuture(null);
    }

    private boolean isAlreadyJoinedTo(
        ZLinkActor actor,
        ZLinkBackendActorRef actorRef,
        String spotId) {
        return actorSessions.isJoinedTo(actor, actorRef, spotId);
    }

    private boolean isJoinedToDifferentSpot(ZLinkActor actor, String spotId) {
        return actorSessions.isJoinedToDifferentSpot(actor, spotId);
    }

    ZLinkBackendActorRef actorLifecycleRef(ZLinkBackendActorLifecycleEvent event) {
        return event.kind() == ZLinkBackendActorLifecycleEventKind.LEFT
            ? event.info().previousActor()
            : event.info().currentActor();
    }

    private String actorLifecycleSpotId(
        ZLinkBackendActorLifecycleEvent event,
        String fallbackSpotId) {
        return event.kind() == ZLinkBackendActorLifecycleEventKind.LEFT
            ? event.info().previousSpotId().orElse(fallbackSpotId)
            : event.info().currentSpotId().orElse(fallbackSpotId);
    }

    private boolean shouldIgnoreJoinedOrLeftLifecycle(
        ZLinkBackendActorLifecycleEvent event,
        ZLinkBackendActorRef actorRef,
        ZLinkActor actor,
        String spotId) {
        if (isJoinedToDifferentSpot(actor, spotId)) {
            return true;
        }
        if (consumeSuppressedActorLifecycleCallback(event.kind(), actor, spotId)) {
            return true;
        }
        return event.kind() != ZLinkBackendActorLifecycleEventKind.LEFT
            && isAlreadyJoinedTo(actor, actorRef, spotId);
    }

    CompletionStage<Void> notifySpotActorLifecycleAndSuppressBackendEvent(
        Object spotSurface,
        ZLinkActor actor,
        String spotId,
        boolean joined) {
        suppressActorLifecycleCallback(
            joined ? ZLinkBackendActorLifecycleEventKind.JOINED : ZLinkBackendActorLifecycleEventKind.LEFT,
            actor,
            spotId);
        if (joined && spotSurface instanceof ZLinkEntrySpot<?>) {
            return actorSessions.runtime().invokeActorLifecycle(
                actor,
                () -> notifySpotActorLifecycle(spotSurface, actor, true));
        }
        return notifySpotActorLifecycle(spotSurface, actor, joined);
    }

    private void suppressActorLifecycleCallback(
        ZLinkBackendActorLifecycleEventKind kind,
        ZLinkActor actor,
        String spotId) {
        if (kind != null && actor != null && spotId != null) {
            suppressedActorLifecycleCallbacks.add(actorLifecycleCallbackKey(kind, actor.context().actorId(), spotId));
        }
    }

    private boolean consumeSuppressedActorLifecycleCallback(
        ZLinkBackendActorLifecycleEventKind kind,
        ZLinkActor actor,
        String spotId) {
        return kind != null
            && actor != null
            && spotId != null
            && suppressedActorLifecycleCallbacks.remove(actorLifecycleCallbackKey(kind, actor.context().actorId(), spotId));
    }

    private static String actorLifecycleCallbackKey(
        ZLinkBackendActorLifecycleEventKind kind,
        String actorId,
        String spotId) {
        return kind.name() + "|" + actorId + "|" + spotId;
    }

    @SuppressWarnings({"rawtypes", "unchecked"})
    private CompletionStage<Void> notifySpotActorDisconnected(ZLinkActor actor) {
        Object spotSurface = localActorSpotSurface(actor);
        if (spotSurface instanceof ZLinkSpot spot) {
            if (spot.context() instanceof DefaultSpotContext context) {
                return withCurrentOutbound(
                    context.dispatchOutbound(),
                    () -> ZLinkHandlerStages.fromStageSupplier(() ->
                        spot.onDisconnectActor(actor)));
            }
            return ZLinkHandlerStages.fromStageSupplier(() -> spot.onDisconnectActor(actor));
        }
        if (spotSurface instanceof ZLinkEntrySpot entrySpot) {
            return ZLinkHandlerStages.fromStageSupplier(() ->
                entrySpot.onDisconnectActor(actor));
        }
        return java.util.concurrent.CompletableFuture.completedFuture(null);
    }

    private CompletionStage<Void> notifySourceActorLeftForRemoteMove(ZLinkActor actor) {
        Object spotSurface = localActorSpotSurface(actor);
        if (spotSurface == null) {
            return java.util.concurrent.CompletableFuture.completedFuture(null);
        }
        String spotId = spotSurface instanceof ZLinkSpot<?> spot
            ? spot.context().spotId()
            : ((ZLinkEntrySpot<?>) spotSurface).context().spotId();
        return notifySpotActorLifecycleAndSuppressBackendEvent(
            spotSurface,
            actor,
            spotId,
            false);
    }

    private CompletionStage<Void> notifySourceActorLeftForLocalMove(
        ZLinkActor actor,
        ZLinkActorRuntime.LocalMoveSource source) {
        Object spotSurface = source.spotSurface();
        if (spotSurface == null) {
            return notifySourceActorLeftForRemoteMove(actor);
        }
        String spotId = source.spotId();
        if (spotId == null || spotId.isBlank()) {
            spotId = spotSurface instanceof ZLinkSpot<?> spot
                ? spot.context().spotId()
                : ((ZLinkEntrySpot<?>) spotSurface).context().spotId();
        }
        return notifySpotActorLifecycleAndSuppressBackendEvent(
            spotSurface,
            actor,
            spotId,
            false);
    }

    private CompletionStage<systems.zlink.framework.spots.ZLinkActorCreateResponse>
        notifyEntrySpotActorCreated(
        RoutingId nodeRid,
        ZLinkActor actor,
        ZLinkMessage createRequest,
        Object createContext) {
        return spotLifecycle.notifyEntrySpotActorCreated(
            nodeRid,
            actor,
            createRequest,
            createContext);
    }

    ZLinkSpot<?> spotFor(String spotId) {
        return spotLifecycle.spotFor(spotId);
    }

    private String meshNameForSpot(String spotId) {
        return spotLocations.meshNameForSpot(
            spotId,
            primaryNode.routingId(),
            spotLifecycle.hasUserSpot(spotId));
    }

    Object spotSurfaceFor(String spotId) {
        return spotLifecycle.spotSurfaceFor(spotId);
    }

    private EntrySpotActivation entrySpotActivationFor(String spotId) {
        return spotLifecycle.entrySpotActivationFor(spotId);
    }

    public CompletionStage<Message> handleEntryActorTransferRoute(
        RoutingId sourceRoutingId,
        Message envelope) {
        EntrySpotActivation activation = entrySpotActivationFor(
            primaryNode.entrySpot().spotId());
        if (activation == null) {
            return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                "Entry Spot activation is not available for actor transfer"));
        }
        return activation.handleInternalActorTransfer(sourceRoutingId, envelope);
    }

    Object localActorSpotSurface(ZLinkActor actor) {
        return actorSessions.spotSurface(
            actor,
            this::spotSurfaceFor,
            spotLifecycle::firstEntrySpot);
    }

    byte[] freezeActorTimerRelocationEnvelope(String actorId) {
        ZLinkActor actor = actorSessions.localActor(actorId)
            .orElseThrow(() -> new IllegalStateException(
                "Actor timer owner is unavailable: " + actorId));
        Object surface = localActorSpotSurface(actor);
        if (surface instanceof ZLinkSpot<?> spot
            && spot.context() instanceof DefaultSpotContext context) {
            return context.freezeActorTimerRelocationEnvelope(actorId);
        }
        if (surface instanceof ZLinkEntrySpot<?> entry
            && entry.context() instanceof DefaultEntrySpotContext context) {
            return context.freezeActorTimerRelocationEnvelope(actorId);
        }
        return ZLinkSpotTimerRelocationEnvelope.encodeCanonical(List.of());
    }

    void resumeActorTimersAfterRelocationAbort(String actorId) {
        actorTimerContext(actorId, false).ifPresent(context -> {
            if (context instanceof DefaultSpotContext spot) {
                spot.resumeActorTimersAfterRelocationAbort(actorId);
            } else {
                ((DefaultEntrySpotContext) context)
                    .resumeActorTimersAfterRelocationAbort(actorId);
            }
        });
    }

    void closeActorTimersAfterRelocation(String actorId) {
        actorTimerContext(actorId, false).ifPresent(context -> {
            if (context instanceof DefaultSpotContext spot) {
                spot.closeActorTimers(actorId);
            } else {
                ((DefaultEntrySpotContext) context).closeActorTimers(actorId);
            }
        });
    }

    void stageEntryActorTimerRelocationEnvelope(
        String targetSpotId,
        String actorId,
        byte[] envelope) {
        EntrySpotActivation activation = entrySpotActivationFor(targetSpotId);
        if (activation == null) {
            throw new IllegalStateException(
                "target Entry Spot timer owner is unavailable: "
                    + targetSpotId);
        }
        activation.context.stageActorTimerRelocationEnvelope(
            actorId, envelope);
    }

    void publishEntryActorTimerRelocation(
        String targetSpotId,
        String actorId) {
        EntrySpotActivation activation = entrySpotActivationFor(targetSpotId);
        if (activation == null) {
            throw new IllegalStateException(
                "target Entry Spot timer owner is unavailable: "
                    + targetSpotId);
        }
        activation.context.publishStagedActorTimerRelocation(actorId);
    }

    void discardEntryActorTimerRelocation(String actorId) {
        EntrySpotActivation activation = entrySpotActivationFor(
            primaryNode.entrySpot().spotId());
        if (activation != null) {
            activation.context.closeActorTimers(actorId);
        }
    }

    private Optional<Object> actorTimerContext(
        String actorId,
        boolean required) {
        Optional<ZLinkActor> actor = actorSessions.localActor(actorId);
        if (actor.isEmpty()) {
            if (required) {
                throw new IllegalStateException(
                    "Actor timer owner is unavailable: " + actorId);
            }
            return Optional.empty();
        }
        Object surface = localActorSpotSurface(actor.orElseThrow());
        if (surface instanceof ZLinkSpot<?> spot
            && spot.context() instanceof DefaultSpotContext context) {
            return Optional.of(context);
        }
        if (surface instanceof ZLinkEntrySpot<?> entry
            && entry.context() instanceof DefaultEntrySpotContext context) {
            return Optional.of(context);
        }
        if (required) {
            throw new IllegalStateException(
                "Actor timer context is unavailable: " + actorId);
        }
        return Optional.empty();
    }

    private SpotActorPacketHandlerRegistration resolveActorPacketHandler(
        String packetName,
        Object spotSurface,
        ZLinkScannedHandlerKind kind) {
        List<SpotActorPacketHandlerRegistration> handlers =
            actorHandlers.handlers(packetName);
        if (handlers == null || handlers.isEmpty()) {
            return null;
        }
        Class<?> spotType = spotSurface == null ? null : spotSurface.getClass();
        SpotActorPacketHandlerRegistration fallback = null;
        for (SpotActorPacketHandlerRegistration handler : handlers) {
            if (handler.kind() != kind) {
                continue;
            }
            if (handler.spotType() == null) {
                if (fallback == null) {
                    fallback = handler;
                }
                continue;
            }
            if (spotType != null) {
                if (handler.spotType().isAssignableFrom(spotType)) {
                    return handler;
                }
                continue;
            }
            if (fallback == null) {
                fallback = handler;
            }
        }
        return fallback;
    }

    private String handlerCandidates(String packetName) {
        List<SpotActorPacketHandlerRegistration> handlers =
            actorHandlers.handlers(packetName);
        if (handlers == null || handlers.isEmpty()) {
            return "none";
        }
        return handlers.stream()
            .map(ZLinkSpotRuntime::handlerSummary)
            .toList()
            .toString();
    }

    private static String handlerSummary(
        SpotActorPacketHandlerRegistration handler) {
        return handler == null
            ? "none"
            : handler.handlerType().getName()
                + ":" + handler.kind()
                + ":spot=" + (handler.spotType() == null
                    ? "null" : handler.spotType().getName())
                + ":actor=" + handler.actorType().getName();
    }

    private CompletionStage<Optional<Message>> dispatchLocalSessionActorPacket(
        SpotActorPacketHandlerRegistration registration,
        Object spotSurface,
        ZLinkActor actor,
        Message payload,
        String contentType,
        Map<String, String> metadata) {
        return registration.kind() == ZLinkScannedHandlerKind.ACTOR_REQUEST
            ? invokeLocalActorRequestHandler(
                registration, spotSurface, actor, payload, contentType, metadata)
            : invokeLocalActorSendHandler(
                registration, spotSurface, actor, payload, contentType, metadata)
                .thenApply(ignored -> Optional.empty());
    }

    private CompletionStage<Void> invokeLocalActorSendHandler(
        SpotActorPacketHandlerRegistration registration,
        Object spotSurface,
        ZLinkActor actor,
        Message payload,
        String contentType,
        Map<String, String> metadata) {
        return spotHandlerInvoker.invokeActorSend(
            registration,
            spotSurface,
            actor,
            payload,
            contentType,
            metadata,
            handlerType -> systems.zlink.framework.runtime.internal.handlers
                .ZLinkActorHandlerInstances.instance(actor, handlerType),
            "failed to invoke local session actor send handler");
    }

    private CompletionStage<Optional<Message>> invokeLocalActorRequestHandler(
        SpotActorPacketHandlerRegistration registration,
        Object spotSurface,
        ZLinkActor actor,
        Message payload,
        String contentType,
        Map<String, String> metadata) {
        return spotHandlerInvoker.invokeActorRequest(
            registration,
            spotSurface,
            actor,
            payload,
            contentType,
            metadata,
            handlerType -> systems.zlink.framework.runtime.internal.handlers
                .ZLinkActorHandlerInstances.instance(actor, handlerType),
            "failed to invoke local session actor request handler");
    }

    private void attachRouteMeshSpotBridges(Map<String, ZLinkInternalSpotNode> routeBridgeNodesByName) {
        if (channels == null || routeMeshChannels.isEmpty() || routeBridgeNodesByName.isEmpty()) {
            return;
        }
        for (ChannelRegistration routeMeshChannel : routeMeshChannels) {
            ZLinkInternalSpotNode routeBridgeNode =
                routeBridgeNodesByName.get(routeMeshChannel.name());
            if (routeBridgeNode == null && routeMeshChannel.routeRoutingId() != null) {
                for (ZLinkInternalSpotNode candidate : routeBridgeNodesByName.values()) {
                    if (routeMeshChannel.routeRoutingId().equals(candidate.routingId())) {
                        routeBridgeNode = candidate;
                        break;
                    }
                }
            }
            if (routeBridgeNode == null && routeBridgeNodesByName.size() == 1) {
                routeBridgeNode = routeBridgeNodesByName.values().iterator().next();
            }
            if (routeBridgeNode != null) {
                channels.attachSpotRouteBridgeToServer(routeMeshChannel.name(), routeBridgeNode);
            }
        }
    }

    boolean dispatchSpotRouteBridgePacket(ZLinkBackendReceived received) {
        return channels != null && channels.dispatchSpotRouteBridgePacket(received);
    }


    void replyActorDispatchError(
        SpotDispatchLine dispatchLine,
        ActorPacketFrames.Header packetHeader,
        ZLinkBackendActorReceived headerPart,
        String actorId,
        Throwable error,
        String failureMessage) {
        byte[] frameBytes;
        try (Message frame = ActorPacketFrames.encodeError(packetHeader, error)) {
            frameBytes = frame.toByteArray();
        } catch (RuntimeException ex) {
            headerPart.close();
            return;
        }
        ZLinkBackendActorRef actorRef = new ZLinkBackendActorRef(
            headerPart.actor().nodeRid(),
            actorId,
            headerPart.actor().generation());
        if (isNoBindActorRequest(packetHeader, headerPart)) {
            dispatchLine.enqueueDispatch(() -> {
                try (Message frame = Message.from(frameBytes)) {
                    primaryNode.replyActorNoBind(
                        actorRef,
                        headerPart.sourceNodeRid(),
                        headerPart.sourceSessionRid(),
                        headerPart.requestId(),
                        headerPart.flags(),
                        List.of(frame));
                    return java.util.concurrent.CompletableFuture.completedFuture(null);
                } finally {
                    headerPart.close();
                }
            });
            return;
        }
        dispatchLine.enqueueDispatch(() -> sendActorBoundSessionWithRetry(
                primaryNode,
                actorRef,
                actorId,
                frameBytes,
                failureMessage)
            .whenComplete((ignored, sendError) -> headerPart.close()));
    }

    @Override
    Executor serialExecutor() {
        return frameworkRegistration.serialExecutor();
    }

    @Override
    Executor infrastructureExecutor() {
        return infrastructureExecutor;
    }

    @Override
    DefaultSpotOutbound createContextOutbound(
        ZLinkBackendSpot backendSpot,
        RoutingId nodeRid) {
        return createSpotOutbound(
            backendSpot,
            spotLocations.meshName(nodeRid),
            spotLocations.publisherChannelName(nodeRid));
    }

    @Override
    ZLinkSpotTimerRegistry createTimerRegistry(
        String spotId,
        systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerInstanceOwner handlers,
        ZLinkSpotTimerRegistry.Dispatch dispatch) {
        return new ZLinkSpotTimerRegistry(
            spotId,
            timerExecutor,
            handlers,
            suspendHandlerInvokers,
            eventDispatcher,
            primaryNodeSourceName,
            (timerName, operation) -> dispatch.enqueue(timerName, () -> {
                if (!dispatchErrors.flow().enabled(
                    systems.zlink.framework.configuration.ZLinkMessageFlowOutcome.SENT)) {
                    return operation.get();
                }
                systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext.State timerFlow =
                    systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext.create(
                        systems.zlink.framework.monitoring.ZLinkFlowOrigin.TIMER);
                try (systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext.Scope ignored =
                    systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext.enter(timerFlow)) {
                    return operation.get();
                }
            }));
    }

    @Override
    systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerInstanceOwner
        createHandlerInstances() {
        return new systems.zlink.framework.runtime.internal.handlers
            .ZLinkHandlerInstanceOwner(handlerFactory);
    }

    @Override
    CompletionStage<Void> destroyActorFromEntry(
        RoutingId nodeRid,
        ZLinkActor actor) {
        return actorAdmissions.destroyFromEntry(nodeRid, actor);
    }

    @Override
    CompletionStage<Void> leaveActor(
        RoutingId nodeRid,
        ZLinkSpot<?> spot,
        ZLinkActor actor,
        String fallbackSpotId) {
        if (actor == null) {
            return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                "actor is required"));
        }
        try {
            if (actorAdmissionsRuntime().isRoutedTransferActor(actor)) {
                EntrySpotActivation entry = entrySpotActivationFor(
                    primaryNode.entrySpot().spotId());
                if (entry == null) {
                    return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                        "Entry Spot activation is not available for actor leave"));
                }
                @SuppressWarnings({"rawtypes", "unchecked"})
                ZLinkEntrySpot rawEntrySpot = entry.entrySpot();
                return actorAdmissions.leaveRoutedActorToLocalEntry(
                    actor,
                    primaryNode.routingId(),
                    actorId -> java.util.concurrent.CompletableFuture.completedFuture(
                        systems.zlink.framework.spots.ZLinkSpotActorJoinResult.accept()),
                    joinedActor -> notifySpotActorLifecycleAndSuppressBackendEvent(
                        rawEntrySpot,
                        joinedActor,
                        primaryNode.entrySpot().spotId(),
                        true));
            }
            EntrySpotActivation entry = entrySpotActivationFor(
                primaryNode.entrySpot().spotId());
            return actorAdmissions.leaveSpot(
                nodeByRid(nodeRid),
                actor,
                fallbackSpotId,
                entry == null ? null : primaryNode.routingId(),
                defaultRequestTimeout);
        } catch (RuntimeException ex) {
            return CompletableFuture.failedFuture(ex);
        }
    }

    private ZLinkActorRuntime actorAdmissionsRuntime() {
        return actorAdmissions.runtime();
    }

    @Override
    CompletionStage<Boolean> closeSpot(String spotId) {
        return spotLifecycle.close(spotId);
    }

    @Override
    CompletionStage<Boolean> closeInstanceSpot(
        String spotId,
        long objectGeneration) {
        ZLinkInstanceSpotActivation activation =
            instanceSpotActivations.get(spotId);
        traceInstanceLifecycle(
            "close-request spot=" + spotId
                + " generation=" + objectGeneration
                + " activation=" + (activation != null));
        if (activation == null
            || activation.context.objectGeneration() != objectGeneration) {
            return CompletableFuture.completedFuture(false);
        }
        return activation.closeExplicit();
    }

    CompletionStage<Boolean> sealInstanceSpotAuthority(
        ZLinkInstanceSpotActivation activation) {
        var store = requireUserSpotLocationStore();
        String key = systems.zlink.framework.runtime.locations
            .ZLinkAuthorityKeyCodec.spot(activation.context.spotId());
        return store.read(key, () -> false).thenCompose(read -> {
            if (read instanceof systems.zlink.framework.runtime.internal.locations
                    .ZLinkAuthorityMissing) {
                return CompletableFuture.completedFuture(true);
            }
            if (!(read instanceof systems.zlink.framework.runtime.internal.locations
                    .ZLinkAuthoritySnapshot snapshot)) {
                return CompletableFuture.completedFuture(false);
            }
            var authority = userSpotAuthorities.decode(snapshot.payload())
                .orElseThrow(() -> new IllegalStateException(
                    "invalid Instance Spot authority"));
            if (authority.kind()
                    != systems.zlink.framework.runtime.locations
                        .ZLinkServiceAuthorityPayloadCodec.Kind.INSTANCE
                || authority.state()
                    != systems.zlink.framework.runtime.locations
                        .ZLinkServiceAuthorityPayloadCodec.State.READY
                || !authority.spotId().equals(activation.context.spotId())
                || !authority.meshName().equals(activation.context.meshName())
                || !authority.nodeRid().equals(activation.context.nodeRid())
                || (activation.expectedNodeGeneration() >= 0
                    && authority.nodeGeneration()
                        != activation.expectedNodeGeneration())
                || snapshot.objectGeneration()
                    != activation.context.objectGeneration()
                || !activation.authorityFenceMatches(
                    snapshot.ownerId(),
                    snapshot.ownerLeaseGeneration(),
                    snapshot.authorityOwnerGeneration())
                || snapshot.allocation().state()
                    != systems.zlink.framework.runtime.internal.locations
                        .ZLinkPlacementAllocationState.ACTIVE
                || snapshot.allocation().objectKind()
                    != systems.zlink.framework.locations
                        .ZLinkPlacementObjectKind.INSTANCE_SPOT) {
                return CompletableFuture.completedFuture(false);
            }
            byte[] closing = userSpotAuthorities.encodeInstance(
                systems.zlink.framework.runtime.locations
                    .ZLinkServiceAuthorityPayloadCodec.State.CLOSING,
                authority.stableType(),
                authority.spotId(),
                snapshot.ownerId(),
                snapshot.ownerLeaseGeneration(),
                authority.meshName(),
                authority.nodeRid(),
                authority.nodeGeneration());
            return store.compareExchange(
                    key,
                    new systems.zlink.framework.runtime.internal.locations
                        .ZLinkAuthorityExpectFound(snapshot.storeVersion()),
                    new systems.zlink.framework.runtime.internal.locations
                        .ZLinkAuthorityPut(
                            closing,
                            systems.zlink.framework.runtime.internal.locations
                                .ZLinkAuthorityGenerationTransition.PRESERVE,
                            Optional.empty(),
                            Optional.empty()),
                    () -> false)
                .thenApply(result -> {
                    if (result instanceof systems.zlink.framework.runtime.internal.locations
                            .ZLinkAuthorityStored stored) {
                        activation.markSealedStoreVersion(stored.storeVersion());
                        ZLinkInternalMeshNode routeNode = routeMeshNodesByName.get(
                            activation.context.meshName());
                        if (routeNode != null) {
                            routeNode.forgetInstanceIntent(
                                activation.authorityRouteFence());
                        }
                        return true;
                    }
                    return false;
                });
        });
    }

    @Override
    CompletionStage<Boolean> completeInstanceSpotClose(
        ZLinkInstanceSpotActivation activation) {
        String spotId = activation.context.spotId();
        traceInstanceLifecycle("close-authority-start spot=" + spotId);
        return releaseInstanceSpotAuthority(activation)
            .thenApply(closed -> {
                traceInstanceLifecycle(
                    "close-authority-result spot=" + spotId
                        + " closed=" + closed);
                instanceSpotActivations.remove(spotId, activation);
                activation.closeResources();
                ZLinkInternalMeshNode routeNode = routeMeshNodesByName.get(
                    activation.context.meshName());
                if (routeNode != null) {
                    routeNode.forgetInstanceIntent(
                        activation.authorityRouteFence());
                }
                return closed;
            });
    }

    void discardInstanceSpotActivation(
        ZLinkInstanceSpotActivation activation) {
        instanceSpotActivations.remove(
            activation.context.spotId(), activation);
        activation.closeResources();
        ZLinkInternalMeshNode routeNode = routeMeshNodesByName.get(
            activation.context.meshName());
        if (routeNode != null) {
            routeNode.forgetInstanceIntent(
                activation.authorityRouteFence());
        }
    }

    private CompletionStage<Boolean> releaseInstanceSpotAuthority(
        ZLinkInstanceSpotActivation activation) {
        var store = requireUserSpotLocationStore();
        String key = systems.zlink.framework.runtime.locations
            .ZLinkAuthorityKeyCodec.spot(activation.context.spotId());
        return store.read(key, () -> false).thenCompose(read -> {
            traceInstanceLifecycle(
                "close-authority-read spot=" + activation.context.spotId()
                    + " result=" + read.getClass().getSimpleName());
            if (read instanceof systems.zlink.framework.runtime.internal.locations
                    .ZLinkAuthorityMissing) {
                return CompletableFuture.completedFuture(true);
            }
            if (!(read instanceof systems.zlink.framework.runtime.internal.locations
                    .ZLinkAuthoritySnapshot snapshot)) {
                return CompletableFuture.completedFuture(false);
            }
            var authority = userSpotAuthorities.decode(snapshot.payload())
                .orElseThrow(() -> new IllegalStateException(
                    "invalid Instance Spot authority"));
            if (authority.kind()
                    != systems.zlink.framework.runtime.locations
                        .ZLinkServiceAuthorityPayloadCodec.Kind.INSTANCE
                || authority.state()
                    != systems.zlink.framework.runtime.locations
                        .ZLinkServiceAuthorityPayloadCodec.State.CLOSING
                || !authority.spotId().equals(activation.context.spotId())
                || !authority.meshName().equals(activation.context.meshName())
                || !authority.nodeRid().equals(activation.context.nodeRid())
                || (activation.expectedNodeGeneration() >= 0
                    && authority.nodeGeneration()
                        != activation.expectedNodeGeneration())
                || snapshot.objectGeneration()
                    != activation.context.objectGeneration()
                || (activation.sealedStoreVersion() != null
                    && !activation.sealedStoreVersion().equals(
                        snapshot.storeVersion()))
                || !activation.authorityFenceMatches(
                    snapshot.ownerId(),
                    snapshot.ownerLeaseGeneration(),
                    snapshot.authorityOwnerGeneration())
                || snapshot.allocation().state()
                    != systems.zlink.framework.runtime.internal.locations
                        .ZLinkPlacementAllocationState.ACTIVE
                || snapshot.allocation().objectKind()
                    != systems.zlink.framework.locations
                        .ZLinkPlacementObjectKind.INSTANCE_SPOT) {
                return CompletableFuture.completedFuture(false);
            }
            return store.compareExchange(
                    key,
                    new systems.zlink.framework.runtime.internal.locations
                        .ZLinkAuthorityExpectFound(snapshot.storeVersion()),
                    new systems.zlink.framework.runtime.internal.locations
                        .ZLinkAuthorityDelete(),
                    () -> false)
                .thenApply(result -> {
                    traceInstanceLifecycle(
                        "close-authority-cas spot="
                            + activation.context.spotId()
                            + " result=" + result.getClass().getSimpleName());
                    return result instanceof
                        systems.zlink.framework.runtime.internal.locations
                            .ZLinkAuthorityDeleted;
                });
        });
    }

    private static void traceInstanceLifecycle(String message) {
        if (STREAM_TRACE) {
            LOGGER.warning(
                "[zlink-java-stream-trace] instance-lifecycle " + message);
        }
    }

    Optional<ZLinkUserSpotRelocationBarrier.Seal>
        trySealUserSpotRelocation(String spotId) {
        return spotLifecycle.relocationBarrier(
            spotId, actorSessions).trySeal();
    }

    <T> CompletionStage<T> runUserSpotCapture(
        String spotId,
        ZLinkUserSpotRelocationBarrier.Seal seal,
        Supplier<CompletionStage<T>> capture) {
        return spotLifecycle.relocationBarrier(
            spotId, actorSessions).runCapture(seal, capture);
    }

    boolean abortUserSpotRelocation(
        String spotId,
        ZLinkUserSpotRelocationBarrier.Seal seal) {
        return spotLifecycle.relocationBarrier(
            spotId, actorSessions).abort(seal);
    }

    Optional<ZLinkUserSpotRelocationBarrier.Committed>
        commitUserSpotRelocation(
            String spotId,
            ZLinkUserSpotRelocationBarrier.Seal seal) {
        return spotLifecycle.relocationBarrier(
            spotId, actorSessions).commit(seal);
    }

    @Override
    boolean isActorMember(String spotId, String actorId) {
        return actorSessions.isActorMember(spotId, actorId);
    }

    @Override
    boolean isActorAtSpot(String actorId, String spotId) {
        return actorAdmissions.isActorAtSpot(actorId, spotId);
    }

    @Override
    Object deferredActorJoinRuntimeScope() {
        return actorAdmissions.deferredJoinRuntimeScope();
    }

    @Override
    <T> CompletionStage<T> runWithOutbound(
        DefaultSpotOutbound outbound,
        Supplier<CompletionStage<T>> operation) {
        return withCurrentOutbound(outbound, operation);
    }

    @Override
    CompletionStage<Void> runEntryDispatch(
        Object entryContext,
        Supplier<CompletionStage<Void>> operation) {
        try (systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext.Scope ignored =
                 systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext
                     .enterEntrySpotDispatch(entryContext)) {
            return operation.get();
        }
    }

    @Override
    CompletionStage<Void> runActorTimerDispatch(
        String actorId,
        Supplier<CompletionStage<Void>> operation) {
        return actorSessions.runtime().runActorDispatchTurn(
            actorId, operation);
    }

    @Override
    CompletionStage<Void> enqueueActorDispatch(
        String actorId,
        long payloadBytes,
        Supplier<CompletionStage<Void>> operation) {
        return actorSessions.runtime().submitActorDispatch(
            actorId, payloadBytes, operation);
    }

    private DefaultSpotOutbound createSpotOutbound(
        ZLinkBackendSpot backendSpot,
        String meshName,
        String publisherChannelName) {
        return new DefaultSpotOutbound(
            backendSpot,
            meshName,
            publisherChannelName,
            serializer,
            frameworkRegistration.codecs()::contentTypeFor,
            routedOutbound,
            directOutbound,
            publishers,
            channels,
            channels != null && !routeMeshChannels.isEmpty(),
            defaultRequestTimeout,
            () -> (SpotTransportAddressResolver) handlerFactory.create(
                SpotTransportAddressResolver.class),
            instanceSpotCalls());
    }


    CompletionStage<Void> sendActorBoundSessionWithRetry(
        ZLinkInternalSpotNode node,
        ZLinkBackendActorRef actor,
        String actorId,
        byte[] frameBytes,
        String failureMessage) {
        return boundSessionSender.send(
            node,
            actor,
            actorId,
            frameBytes,
            failureMessage);
    }

    private static void traceActorSession(String message) {
        if (!STREAM_TRACE) {
            return;
        }
        LOGGER.warning("[zlink-java-stream-trace] actor-session " + message);
    }

    static ActorMessageRead readActorMessage(
        List<ZLinkBackendActorReceived> actorMessages,
        int index,
        ZLinkBackendActorReceived pendingActorHeader) {
        boolean fromPendingHeader = pendingActorHeader != null;
        ZLinkBackendActorReceived headerPart = fromPendingHeader
            ? pendingActorHeader
            : actorMessages.get(index++);
        ZLinkBackendActorReceived bodyPart =
            headerPart.hasMore() && index < actorMessages.size()
                ? actorMessages.get(index++)
                : null;
        if (headerPart.hasMore() && bodyPart == null) {
            return new ActorMessageRead(
                false,
                fromPendingHeader,
                headerPart,
                null,
                fromPendingHeader ? pendingActorHeader : copyActorReceived(headerPart),
                index);
        }
        return new ActorMessageRead(
            true,
            fromPendingHeader,
            headerPart,
            bodyPart,
            null,
            index);
    }

    boolean dispatchActorControlPacket(
        ActorPacketFrames.Header packetHeader,
        ZLinkBackendActorReceived headerPart,
        ZLinkActor actor,
        boolean pendingHeader) {
        if (REMOTE_BOUND_SESSION_BIND_PACKET_NAME.equals(packetHeader.packetName())) {
            actorSessions.bindNativeSession(
                actor,
                primaryNode,
                headerPart.actor(),
                headerPart.sourceNodeRid(),
                headerPart.sourceSessionRid());
            if (packetHeader.requestSeq().isPresent()) {
                // The native STREAM bind is terminal only after this target
                // runtime has installed the session context.
                try (Message acknowledgement = Message.from(new byte[0])) {
                    primaryNode.replyActorNoBind(
                        headerPart.actor(),
                        headerPart.sourceNodeRid(),
                        headerPart.sourceSessionRid(),
                        headerPart.requestId(),
                        headerPart.flags(),
                        List.of(acknowledgement));
                }
            }
            closePendingActorHeader(headerPart, pendingHeader);
            return true;
        }
        if (ZLinkActorSpotRoutePackets.SESSION_DISCONNECTED_PACKET_NAME.equals(packetHeader.packetName())) {
            closePendingActorHeader(headerPart, pendingHeader);
            notifySpotActorDisconnected(actor).exceptionally(error -> null);
            return true;
        }
        return false;
    }

    boolean isActorInfrastructureControl(
        List<ZLinkBackendActorReceived> actorMessages) {
        if (actorMessages == null || actorMessages.isEmpty()) {
            return false;
        }
        try {
            String packetName = ActorPacketFrames.decode(
                actorMessages.get(0)).packetName();
            return REMOTE_BOUND_SESSION_BIND_PACKET_NAME.equals(packetName)
                || ZLinkActorSpotRoutePackets.SESSION_DISCONNECTED_PACKET_NAME
                    .equals(packetName);
        } catch (RuntimeException malformed) {
            return false;
        }
    }

    Supplier<CompletionStage<Void>> actorLifecycleTransition(
        Object spotSurface,
        ZLinkBackendActorLifecycleEvent event,
        ZLinkBackendActorRef actorRef,
        ZLinkActor actor,
        String defaultSpotId) {
        if (event.kind() == ZLinkBackendActorLifecycleEventKind.DISCONNECTED) {
            return () -> notifySpotActorDisconnected(actor);
        }
        String spotId = actorLifecycleSpotId(event, defaultSpotId);
        if (shouldIgnoreJoinedOrLeftLifecycle(event, actorRef, actor, spotId)) {
            return null;
        }
        if (event.kind() == ZLinkBackendActorLifecycleEventKind.LEFT) {
            return () -> actorAdmissions.markLeft(actor)
                .thenCompose(ignored -> notifySpotActorLifecycle(spotSurface, actor, false))
                .whenComplete((ignored, error) ->
                    actorAdmissions.completeLeave(actor.context().actorId(), error));
        }
        return () -> actorAdmissions.markJoined(
                actor,
                actorRef,
                spotId,
                spotSurfaceFor(spotId) instanceof ZLinkSpot<?> spot ? spot : null)
            .thenCompose(ignored -> notifySpotActorLifecycle(spotSurface, actor, true))
            .whenComplete((ignored, error) -> {
                if (spotSurface instanceof ZLinkEntrySpot<?>) {
                    actorAdmissions.completeEntryJoin(actor.context().actorId(), error);
                }
            });
    }

    boolean shouldRunActorLifecycleInSpotDispatch(
        ZLinkBackendActorLifecycleEvent event,
        ZLinkActor actor) {
        return event.kind() == ZLinkBackendActorLifecycleEventKind.LEFT
            && actorAdmissions.isLeavePending(actor.context().actorId());
    }

    CompletionStage<Void> dispatchLocalActorPacket(
        SpotDispatchLine dispatchLine,
        Object spotSurface,
        ZLinkActor actor,
        ActorPacketFrames.Header packetHeader,
        ZLinkBackendActorReceived headerPart,
        ZLinkBackendActorReceived bodyPart,
        boolean pendingHeader) {
        traceActorSession("resolve-actor-packet"
            + " actor=" + actor.context().actorId()
            + " spot=" + dispatchLine.spotId()
            + " packet=" + packetHeader.packetName()
            + " request=" + packetHeader.requestSeq().isPresent()
            + " moving=" + actorSessions.isMoving(actor));
        SpotActorPacketHandlerRegistration handler =
            resolveActorPacketHandler(
                packetHeader.packetName(),
                spotSurface,
                packetHeader.requestSeq().isPresent()
                    ? ZLinkScannedHandlerKind.ACTOR_REQUEST
                    : ZLinkScannedHandlerKind.ACTOR_SEND);
        traceActorSession("resolve-result"
            + " actor=" + actor.context().actorId()
            + " packet=" + packetHeader.packetName()
            + " spotSurface=" + (spotSurface == null
                ? "null" : spotSurface.getClass().getName())
            + " selected=" + handlerSummary(handler)
            + " candidates=" + handlerCandidates(packetHeader.packetName()));
        if (handler == null) {
            boolean request = packetHeader.requestSeq().isPresent();
            reportSpotActorHandlerMissing(
                packetHeader,
                dispatchLine.spotId(),
                actor.context().actorId(),
                headerPart.sourceNodeRid());
            if (request) {
                ZLinkBackendActorReceived headerCopy = pendingHeader
                    ? headerPart
                    : copyActorReceived(headerPart);
                replyActorDispatchError(
                    dispatchLine,
                    packetHeader,
                    headerCopy,
                    actor.context().actorId(),
                    new ZLinkConfigurationException(
                        "No SPOT actor request handler is registered for '"
                            + packetHeader.packetName() + "'."),
                    "actor dispatch error reply failed");
            } else {
                closePendingActorHeader(headerPart, pendingHeader);
            }
            return CompletableFuture.completedFuture(null);
        }
        if (packetHeader.requestSeq().isPresent()
            != (handler.kind() == ZLinkScannedHandlerKind.ACTOR_REQUEST)) {
            closePendingActorHeader(headerPart, pendingHeader);
            return CompletableFuture.completedFuture(null);
        }
        if (!handler.actorType().isInstance(actor)) {
            closePendingActorHeader(headerPart, pendingHeader);
            return CompletableFuture.completedFuture(null);
        }
        ZLinkBackendActorReceived headerCopy = pendingHeader
            ? headerPart
            : copyActorReceived(headerPart);
        Message payloadCopy = bodyPart == null
            ? Message.from(new byte[0])
            : Message.from(bodyPart.message());
        ZLinkInboundDispatchBudget.Lease lease =
            bodyPart != null && bodyPart.inboundDispatchLease() != null
                ? bodyPart.inboundDispatchLease()
                : headerPart.inboundDispatchLease() != null
                    ? headerPart.inboundDispatchLease()
                    : inboundDispatchBudget().track(payloadCopy.size());
        AtomicBoolean released = new AtomicBoolean();
        Runnable release = () -> {
            if (!released.compareAndSet(false, true)) {
                return;
            }
            payloadCopy.close();
            headerCopy.close();
            lease.close();
        };
        CompletionStage<Optional<Message>> captured = null;
        if (actorSessions.isMoving(actor)) {
            ZLinkActorReplyRoute replyRoute =
                packetHeader.requestSeq().isPresent()
                    && headerCopy.sourceNodeRid() != null
                    && headerCopy.sourceSessionRid() != null
                ? new ZLinkActorReplyRoute(
                    headerCopy.actor(),
                    headerCopy.sourceNodeRid(),
                    headerCopy.sourceSessionRid(),
                    headerCopy.requestId() == 0
                        ? packetHeader.requestSeq().orElseThrow()
                        : headerCopy.requestId(),
                    headerCopy.flags())
                    : null;
            try {
                captured = actorSessions.captureMoving(
                    actor, packetHeader.toStreamHeader(), payloadCopy, replyRoute);
            } catch (RuntimeException failure) {
                release.run();
                throw failure;
            }
        }
        if (captured != null) {
            return captured.thenCompose(reply -> replyCapturedActorPacket(
                    actor, packetHeader, headerCopy, reply))
                .whenComplete((ignored, error) -> release.run());
        }
        CompletionStage<Void> queued = actorSessions.isMoving(actor)
            ? actorSessions.awaitMoveCompletion(actor)
                .thenCompose(ignored -> enqueueLocalActorPacket(
                    dispatchLine,
                    spotSurface,
                    actor,
                    packetHeader,
                    handler,
                    headerCopy,
                    payloadCopy,
                    lease))
            : enqueueLocalActorPacket(
                dispatchLine,
                spotSurface,
                actor,
                packetHeader,
                handler,
                headerCopy,
                payloadCopy,
                lease);
        queued.whenComplete((ignored, error) -> release.run());
        return queued;
    }

    private CompletionStage<Void> enqueueLocalActorPacket(
        SpotDispatchLine dispatchLine,
        Object spotSurface,
        ZLinkActor actor,
        ActorPacketFrames.Header packetHeader,
        SpotActorPacketHandlerRegistration handler,
        ZLinkBackendActorReceived headerCopy,
        Message payloadCopy,
        ZLinkInboundDispatchBudget.Lease lease) {
        CompletionStage<Void> queued;
        try {
            queued = dispatchLine.enqueueActorDispatch(
                actor.context().actorId(),
                payloadCopy.size(),
                () -> {
                    if (packetHeader.flowId().isEmpty()) {
                        return dispatchActorPacketToHandler(
                            dispatchLine.dispatchOutbound(), handler, spotSurface, actor,
                            packetHeader, headerCopy, payloadCopy,
                            "actor bound session reply failed", lease);
                    }
                    var state = new systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext.State(
                        packetHeader.flowId().orElseThrow(), packetHeader.flowOrigin().orElseThrow());
                    try (var ignored = systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext.enter(state)) {
                        return dispatchActorPacketToHandler(
                            dispatchLine.dispatchOutbound(), handler, spotSurface, actor,
                            packetHeader, headerCopy, payloadCopy,
                            "actor bound session reply failed", lease);
                    }
                });
        } catch (RuntimeException failure) {
            return CompletableFuture.failedFuture(failure);
        }
        return queued;
    }

    private CompletionStage<Void> replyCapturedActorPacket(
        ZLinkActor actor,
        ActorPacketFrames.Header packetHeader,
        ZLinkBackendActorReceived headerPart,
        Optional<Message> reply) {
        if ("1".equals(System.getenv("ZLINK_JAVA_STREAM_TRACE"))) {
            java.util.logging.Logger.getLogger(
                    ZLinkSpotRuntime.class.getName())
                .warning("[zlink-java-stream-trace] captured reply"
                    + " present=" + reply.isPresent()
                    + " actor=" + actor.context().actorId());
        }
        if (reply.isEmpty()) {
            return java.util.concurrent.CompletableFuture.completedFuture(null);
        }
        byte[] frameBytes;
        try (Message payload = reply.get();
             Message frame = ActorPacketFrames.encodeReply(packetHeader, payload)) {
            frameBytes = frame.toByteArray();
        }
        if (isNoBindActorRequest(packetHeader, headerPart)) {
            try (Message frame = Message.from(frameBytes)) {
                primaryNode.replyActorNoBind(
                    headerPart.actor(),
                    headerPart.sourceNodeRid(),
                    headerPart.sourceSessionRid(),
                    headerPart.requestId(),
                    headerPart.flags(),
                    List.of(frame));
            }
            return java.util.concurrent.CompletableFuture.completedFuture(null);
        }
        return sendActorBoundSessionWithRetry(
            primaryNode,
            actorSessions.messageFollowTargetActorRef(actor)
                .orElse(headerPart.actor()),
            actor.context().actorId(),
            frameBytes,
            "actor handoff reply failed");
    }

    static void closePendingActorHeader(
        ZLinkBackendActorReceived headerPart,
        boolean pendingHeader) {
        if (pendingHeader) {
            headerPart.close();
        }
    }

    static ZLinkBackendActorReceived copyActorReceived(
        ZLinkBackendActorReceived received) {
        return new ZLinkBackendActorReceived(
            received.actor(),
            received.sourceNodeRid(),
            received.sourceSessionRid(),
            received.requestSeq(),
            received.requestId(),
            received.flags(),
            Message.from(received.message()),
            received.hasMore(),
            received.acceptedJournalRecord(),
            received.contentType(),
            received.inboundDispatchLease());
    }

    static boolean isNoBindActorRequest(
        ActorPacketFrames.Header packetHeader,
        ZLinkBackendActorReceived headerPart) {
        return packetHeader.requestSeq().isPresent()
            && headerPart.requestId() != 0
            && isNoBindActorPacket(headerPart);
    }

    static boolean isNoBindActorPacket(ZLinkBackendActorReceived headerPart) {
        return (headerPart.flags() & ACTOR_RECV_INFO_NO_BIND) != 0;
    }

    void traceMessageFlow(
        ZLinkMessageFlowOutcome outcome,
        ZLinkDispatchErrorSurface surface,
        ZLinkDispatchMessageKind messageKind,
        String packetName,
        String channelName,
        String topic,
        String correlationId,
        String sourceRid,
        String spotId,
        String actorId) {
        if (!dispatchErrors.flow().enabled(outcome)) {
            return;
        }
        dispatchErrors.flow().trace(new ZLinkMessageFlowEvent(
            outcome,
            surface,
            messageKind,
            packetName,
            channelName,
            topic,
            correlationId,
            sourceRid,
            spotId,
            actorId,
            null));
    }

    private void reportDispatchError(DispatchFailureReport failure) {
        dispatchErrors.report(new ZLinkDispatchFailure(
            failure.surface,
            failure.messageKind,
            failure.reason,
            failure.action,
            failure.packetName == null || failure.packetName.isBlank() ? null : failure.packetName,
            failure.channelName,
            failure.topic,
            failure.spotId == null ? null : failure.spotId.toString(),
            failure.actorId,
            failure.sourceRid == null ? null : failure.sourceRid.toString(),
            failure.correlationId,
            errorType(failure.error),
            errorMessage(failure.error)));
    }

    void reportSpotRouteSendDropped(
        ZLinkBackendReceived received,
        String packetName,
        String spotId) {
        reportDispatchError(DispatchFailureReport.of(
                ZLinkDispatchErrorSurface.SPOT_ROUTE,
                ZLinkDispatchMessageKind.SEND,
                ZLinkDispatchErrorReason.HANDLER_MISSING,
                ZLinkDispatchErrorAction.DROP)
            .packetName(packetName)
            .spotId(spotId)
            .sourceRid(received.routingId().orElse(null)));
    }

    void reportSpotSubscriptionDropped(
        String topic,
        String packetName,
        String spotId,
        ZLinkDispatchErrorReason reason) {
        reportDispatchError(DispatchFailureReport.of(
                ZLinkDispatchErrorSurface.SPOT_SUBSCRIPTION,
                ZLinkDispatchMessageKind.PUBLISH,
                reason,
                ZLinkDispatchErrorAction.DROP)
            .packetName(packetName)
            .topic(topic)
            .spotId(spotId));
    }

    void reportSpotActorHandlerMissing(
        ActorPacketFrames.Header packetHeader,
        String spotId,
        String actorId,
        RoutingId sourceRid) {
        boolean request = packetHeader.requestSeq().isPresent();
        reportDispatchError(DispatchFailureReport.of(
                ZLinkDispatchErrorSurface.SPOT_ACTOR,
                request
                    ? ZLinkDispatchMessageKind.ACTOR_REQUEST
                    : ZLinkDispatchMessageKind.ACTOR_SEND,
                ZLinkDispatchErrorReason.HANDLER_MISSING,
                request
                    ? ZLinkDispatchErrorAction.REPLY_ERROR
                    : ZLinkDispatchErrorAction.DROP)
            .packetName(packetHeader.packetName())
            .spotId(spotId)
            .actorId(actorId)
            .sourceRid(sourceRid)
            .correlationId(packetHeader.requestSeq().map(Object::toString).orElse(null)));
    }


    private static String errorType(Throwable error) {
        if (error == null) {
            return null;
        }
        Throwable current = unwrapDispatchError(error);
        return current.getClass().getSimpleName();
    }

    private static String errorMessage(Throwable error) {
        if (error == null) {
            return null;
        }
        Throwable current = unwrapDispatchError(error);
        return current.getMessage() == null
            ? current.getClass().getName()
            : current.getMessage();
    }

    private static Throwable unwrapDispatchError(Throwable error) {
        Throwable current = error;
        while ((current instanceof CompletionException
            || current instanceof InvocationTargetException)
            && current.getCause() != null) {
            current = current.getCause();
        }
        if (current instanceof ZLinkConfigurationException
            && current.getCause() != null
            && current.getMessage() != null
            && current.getMessage().startsWith("failed to invoke ")) {
            current = current.getCause();
        }
        return current;
    }

    void replySpotRouteDispatchError(
        ZLinkBackendReceived received,
        String packetName,
        String spotId,
        ZLinkDispatchErrorReason reason,
        Throwable error) {
        Throwable cause = unwrapCompletion(error);
        List<Message> reply = ZLinkFrameworkErrorReply.create(errorText(reason, packetName, cause));
        try {
            received.reply(reply);
        } catch (RuntimeException ignored) {
        } finally {
            reply.forEach(Message::close);
        }
        reportDispatchError(DispatchFailureReport.of(
                ZLinkDispatchErrorSurface.SPOT_ROUTE,
                ZLinkDispatchMessageKind.REQUEST,
                reason,
                ZLinkDispatchErrorAction.REPLY_ERROR)
            .packetName(packetName)
            .spotId(spotId)
            .sourceRid(received.routingId().orElse(null))
            .correlationId(received.requestSeq().map(Object::toString).orElse(null))
            .error(cause));
    }

    private static Throwable unwrapCompletion(Throwable error) {
        if (error instanceof CompletionException && error.getCause() != null) {
            return error.getCause();
        }
        return error;
    }

    private static String errorText(
        ZLinkDispatchErrorReason reason,
        String packetName,
        Throwable error) {
        if (error != null && error.getMessage() != null) {
            return error.getMessage();
        }
        return reason + " for packet '" + packetName + "'";
    }

    static ParsedPacket parsePacket(List<Message> parts) {
        if (parts.size() >= 2) {
            return new ParsedPacket(parts.get(0).toUtf8String(), parts.get(1));
        }
        return new ParsedPacket("", parts.get(0));
    }

    static boolean isProbeFrame(List<Message> parts) {
        return parts.isEmpty() || parts.get(0).size() == 0;
    }

    static void traceSpotRouteInbound(
        String phase,
        ZLinkBackendSpot backendSpot,
        ZLinkBackendReceived received) {
        if (!STREAM_TRACE) {
            return;
        }
        LOGGER.fine("[zlink-java-stream-trace] spot-route " + phase
            + " localSpot=" + backendSpot.spotId()
            + " sourceRid=" + received.routingId().map(Object::toString).orElse(null)
            + " sourceSpot=" + received.spotId().map(Object::toString).orElse(null)
            + " requestSeq=" + received.requestSeq().map(Object::toString).orElse(null)
            + " result=" + received.result()
            + " parts=" + describeTraceParts(received.parts()));
    }

    static void traceSpotRouteDispatch(
        String phase,
        ZLinkBackendSpot backendSpot,
        ZLinkBackendReceived received,
        ParsedPacket packet) {
        if (!STREAM_TRACE) {
            return;
        }
        LOGGER.fine("[zlink-java-stream-trace] spot-route " + phase
            + " localSpot=" + backendSpot.spotId()
            + " sourceRid=" + received.routingId().map(Object::toString).orElse(null)
            + " sourceSpot=" + received.spotId().map(Object::toString).orElse(null)
            + " requestSeq=" + received.requestSeq().map(Object::toString).orElse(null)
            + " packet=" + packet.packetName()
            + " payloadBytes=" + packet.payload().size());
    }

    private static String describeTraceParts(List<Message> parts) {
        List<String> descriptions = new ArrayList<>();
        for (int i = 0; i < parts.size(); i++) {
            byte[] bytes = parts.get(i).toByteArray();
            descriptions.add(i + ":" + bytes.length + ":" + traceText(bytes));
        }
        return descriptions.toString();
    }

    private static String traceText(byte[] bytes) {
        if (bytes.length == 0 || bytes.length > 512) {
            return "";
        }
        String text = new String(bytes, StandardCharsets.UTF_8);
        for (int i = 0; i < text.length(); i++) {
            char ch = text.charAt(i);
            if (Character.isISOControl(ch) && !Character.isWhitespace(ch)) {
                return "";
            }
        }
        return text;
    }


    void awaitClosing(CompletionStage<Void> closingStage) {
        try {
            closingStage.toCompletableFuture()
                .get(defaultRequestTimeout.toMillis(), TimeUnit.MILLISECONDS);
        } catch (TimeoutException ex) {
            throw new ZLinkConfigurationException(
                "SPOT closing hook did not complete before timeout.",
                ex);
        } catch (InterruptedException ex) {
            Thread.currentThread().interrupt();
            throw new ZLinkConfigurationException(
                "SPOT closing hook was interrupted.",
                ex);
        } catch (java.util.concurrent.ExecutionException ex) {
            throw new ZLinkConfigurationException(
                "SPOT closing hook failed.",
                ex.getCause());
        }
    }
}
