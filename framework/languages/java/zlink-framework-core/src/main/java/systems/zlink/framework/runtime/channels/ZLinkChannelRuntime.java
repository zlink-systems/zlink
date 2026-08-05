package systems.zlink.framework.runtime.channels;

import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;

import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;

import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;

import systems.zlink.framework.runtime.internal.backend.*;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.Executor;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.locks.LockSupport;
import java.util.function.Supplier;
import java.util.logging.Logger;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.errors.ZlinkCloseException;
import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.ZLinkHandlerFilter;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.channels.ZLinkClient;
import systems.zlink.framework.channels.ZLinkFanoutClient;
import systems.zlink.framework.channels.ZLinkFanoutPublishCall;
import systems.zlink.framework.channels.ZLinkChannelRuntimeOptions;
import systems.zlink.framework.channels.ZLinkClientServerChannelRuntimeOptions;
import systems.zlink.framework.channels.ZLinkRouteMeshChannelRuntimeOptions;
import systems.zlink.framework.channels.ZLinkPublishCall;
import systems.zlink.framework.channels.ZLinkPublishMessageContext;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.channels.ZLinkRequestCall;
import systems.zlink.framework.channels.ZLinkRouteMessageContext;
import systems.zlink.framework.channels.ZLinkSendCall;
import systems.zlink.framework.channels.ZLinkSocketRuntimeOptions;
import systems.zlink.framework.configuration.ZLinkDispatchErrorAction;
import systems.zlink.framework.configuration.ZLinkDispatchErrorReason;
import systems.zlink.framework.configuration.ZLinkDispatchErrorSurface;
import systems.zlink.framework.configuration.ZLinkDispatchMessageKind;
import systems.zlink.framework.configuration.ZLinkMessageFlowEvent;
import systems.zlink.framework.configuration.ZLinkMessageFlowOutcome;
import systems.zlink.framework.configuration.ZLinkDispatchFailure;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue;
import systems.zlink.framework.runtime.internal.locations.ZLinkAutoConnectType;
import systems.zlink.framework.locations.ZLinkLocationRole;
import systems.zlink.framework.runtime.internal.locations.ZLinkClientServerServerDescriptor;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.spots.SpotHandle;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddressResolver;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkRuntimeEventDispatcher;
import systems.zlink.framework.runtime.internal.configuration.ZLinkCodecRegistration;
import systems.zlink.framework.runtime.configuration.ZLinkFrameworkRegistration;
import systems.zlink.framework.runtime.diagnostics.ZLinkDispatchErrorReporter;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerScanner;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerStages;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandler;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerCatalog;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerKind;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerSurface;
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationAdapter;
import systems.zlink.framework.runtime.internal.channels.ZLinkClientServerRuntimeConfiguration;
import systems.zlink.framework.runtime.internal.channels.ZLinkFanoutRuntimeConfiguration;
import systems.zlink.framework.runtime.messaging.ZLinkPayloadEncoding;
import systems.zlink.framework.runtime.messaging.ZLinkMessagePayloads;
import systems.zlink.framework.runtime.messaging.ZLinkFrameworkErrorReply;
import systems.zlink.framework.runtime.messaging.ZLinkApplicationMetadata;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkInboundDispatchBudget;

public final class ZLinkChannelRuntime
    implements ZLinkClient, ZLinkFanoutClient, ZLinkRouteClient, ZLinkChannelRuntimeOptions,
        AutoCloseable {
    private static final Logger LOGGER = Logger.getLogger(ZLinkChannelRuntime.class.getName());
    private static final Duration CLIENT_SERVER_READY_WAIT_CAP = Duration.ofSeconds(5);
    private static final String SPOT_ROUTE_BRIDGE_SEND_PACKET_NAME =
        "__zlink.routed_spot.egress.send";
    private static final String SPOT_ROUTE_BRIDGE_REQUEST_PACKET_NAME =
        "__zlink.routed_spot.egress.request";
    private static final boolean STREAM_TRACE =
        "1".equals(System.getenv("ZLINK_JAVA_STREAM_TRACE"));

    private final ZLinkBackendContext context;
    private final boolean ownsContext;
    private final ZLinkChannelSocketRegistry sockets = new ZLinkChannelSocketRegistry();
    private final ZLinkChannelDispatchRegistry dispatchRegistry;
    private final ZLinkSpotRouteBridgeRawReplies spotRouteBridgeRawReplies =
        new ZLinkSpotRouteBridgeRawReplies();
    private final ZLinkMessageSerializer serializer;
    private final ZLinkChannelReplyDecoder replyDecoder;
    private final ZLinkCodecRegistration codecs;
    private final ZLinkHandlerActivator handlerFactory;
    private final Executor handlerExecutor;
    private final List<ZLinkSuspendInvocationAdapter> suspendHandlerInvokers;
    private final List<Class<? extends ZLinkHandlerFilter>> filterTypes;
    private final ZLinkChannelHandlerInvoker channelHandlerInvoker;
    private final ZLinkChannelReceiveLoops receiveLoops;
    private final ZLinkInboundDispatchBudget inboundDispatchBudget;
    private final Duration defaultRequestTimeout;
    private final ZLinkChannelCallRuntime callRuntime;
    private final SpotTransportAddressResolver spotAddressResolver;
    private final ZLinkSpotRouteBridgeDrainer spotRouteBridgeDrainer;
    private final ZLinkBackendAdapterProvider backendFactory;
    private final ZLinkBackendAdapterOptions adapterOptions;
    private final ZLinkChannelBackendAdapter channelBackend;
    private final ZLinkMonitoringBackendAdapter clientServerMonitoringBackend;
    private final ZLinkMonitoringBackendAdapter fanoutMonitoringBackend;
    private final ZLinkDispatchErrorReporter dispatchErrors;
    private final ZLinkChannelDispatchReporter dispatchReporter;
    private final ZLinkChannelMessageDispatcher messageDispatcher;
    private final ZLinkChannelRouteDispatcher routeDispatcher;
    private ZLinkFanoutLocationRuntime fanoutLocationRuntime;
    private volatile Supplier<ZLinkFrameworkRuntimeState> hostState =
        () -> ZLinkFrameworkRuntimeState.SERVING;
    private final systems.zlink.framework.monitoring.ZLinkClientServerRuntime
        clientServerRuntime = new ZLinkClientServerRuntimeView(
            sockets,
            () -> hostState.get());
    private final systems.zlink.framework.monitoring.ZLinkFanoutRuntime
        fanoutRuntime = new ZLinkFanoutRuntimeView(
            sockets,
            () -> fanoutLocationRuntime,
            () -> hostState.get());
    private Supplier<ZLinkInternalSpotNode> spotRouteBridgeOwner;
    private final ExecutorService spotRouteBridgeExecutor = Executors.newSingleThreadExecutor(task -> {
        Thread thread = new Thread(task, "zlink-java-spot-route-bridge");
        thread.setDaemon(true);
        return thread;
    });
    private final ScheduledExecutorService spotRouteBridgeDrainLoopExecutor =
        Executors.newSingleThreadScheduledExecutor(task -> {
            Thread thread = new Thread(task, "zlink-java-spot-route-bridge-drain");
            thread.setDaemon(true);
            return thread;
        });
    private final ScheduledExecutorService timeoutExecutor = Executors.newSingleThreadScheduledExecutor(task -> {
        Thread thread = new Thread(task, "zlink-java-channel-timeout");
        thread.setDaemon(true);
        return thread;
    });
    private volatile boolean running = true;

    public record AutoConnectSurface(
        ZLinkAutoConnectType type,
        String meshName,
        ZLinkLocationRole role,
        RoutingId nodeRid,
        String endpoint,
        int weight,
        ZLinkBackendConnectableSocket socket,
        List<String> manualEndpoints) {
    }

    @Override
    public ZLinkClientServerChannelRuntimeOptions clientServerChannel(String channelName) {
        ChannelRegistration registration = requireChannel(channelName, ChannelKind.CLIENT_SERVER);
        return new DefaultClientServerChannelRuntimeOptions(this, registration.name());
    }

    @Override
    public ZLinkRouteMeshChannelRuntimeOptions routeMeshChannel(String channelName) {
        ChannelRegistration registration = requireChannel(channelName, ChannelKind.ROUTE_MESH);
        return new DefaultRouteMeshChannelRuntimeOptions(this, registration.name());
    }

    public systems.zlink.framework.monitoring.ZLinkClientServerRuntime
        clientServerRuntime() {
        return clientServerRuntime;
    }

    public systems.zlink.framework.monitoring.ZLinkFanoutRuntime
        fanoutRuntime() {
        return fanoutRuntime;
    }

    public void setHostStateSupplier(
        Supplier<ZLinkFrameworkRuntimeState> hostState) {
        this.hostState = Objects.requireNonNull(hostState, "hostState");
    }

    private ChannelRegistration requireChannel(String channelName, ChannelKind kind) {
        if (channelName == null || channelName.isBlank()) {
            throw new ZLinkConfigurationException("channel name is required");
        }
        ChannelRegistration registration = sockets.registration(channelName);
        if (registration == null) {
            throw new ZLinkConfigurationException("channel is not configured: " + channelName);
        }
        if (registration.kind() != kind) {
            throw new ZLinkConfigurationException(
                "channel has incompatible kind: " + channelName);
        }
        return registration;
    }

    private static String requireRouterChannelId(String routerChannelId) {
        if (routerChannelId == null || routerChannelId.isBlank()) {
            throw new ZLinkConfigurationException("router channel id is required");
        }
        return routerChannelId;
    }

    ZLinkBackendRouterSocket serverSocket(String channelName) {
        ZLinkBackendRouterSocket socket = sockets.server(channelName);
        if (socket == null) {
            throw new ZLinkConfigurationException(
                "client/server channel has no server socket: " + channelName);
        }
        return socket;
    }

    static void validatePeerWeight(int value) {
        if (value < 0 || value > 10_000) {
            throw new ZLinkConfigurationException(
                "Weight must be in 0..10000.");
        }
    }

    public ZLinkChannelRuntime(
        ZLinkChannelBackendAdapter backend,
        ZLinkFrameworkRegistration registration,
        ZLinkMessageSerializer serializer) {
        this(backend, registration, serializer, ZLinkHandlerActivator.reflection());
    }

    public ZLinkChannelRuntime(
        ZLinkChannelBackendAdapter backend,
        ZLinkFrameworkRegistration registration,
        ZLinkMessageSerializer serializer,
        ZLinkHandlerActivator handlerFactory) {
        this(backend, null, null, registration, serializer, handlerFactory);
    }

    public ZLinkChannelRuntime(
        ZLinkChannelBackendAdapter backend,
        ZLinkFrameworkRegistration registration,
        ZLinkMessageSerializer serializer,
        ZLinkHandlerActivator handlerFactory,
        java.util.function.BiFunction<
            ZLinkBackendObject,
            ZLinkBackendAdmissionKey,
            java.util.function.BiFunction<
                java.util.function.Supplier<Boolean>,
                Runnable,
                CompletionStage<Void>>> admission) {
        this(
            backend,
            backend.createContext(),
            true,
            null,
            null,
            registration,
            serializer,
            handlerFactory,
            null,
            admission);
    }

    public ZLinkChannelRuntime(
        ZLinkChannelBackendAdapter backend,
        ZLinkBackendAdapterProvider backendFactory,
        ZLinkBackendAdapterOptions adapterOptions,
        ZLinkFrameworkRegistration registration,
        ZLinkMessageSerializer serializer,
        ZLinkHandlerActivator handlerFactory) {
        this(
            backend,
            backend.createContext(),
            true,
            backendFactory,
            adapterOptions,
            registration,
            serializer,
            handlerFactory,
            null);
    }

    public ZLinkChannelRuntime(
        ZLinkChannelBackendAdapter backend,
        ZLinkBackendContext context,
        ZLinkBackendAdapterProvider backendFactory,
        ZLinkBackendAdapterOptions adapterOptions,
        ZLinkFrameworkRegistration registration,
        ZLinkMessageSerializer serializer,
        ZLinkHandlerActivator handlerFactory) {
        this(
            backend,
            context,
            false,
            backendFactory,
            adapterOptions,
            registration,
            serializer,
            handlerFactory,
            null);
    }

    public ZLinkChannelRuntime(
        ZLinkChannelBackendAdapter backend,
        ZLinkBackendContext context,
        ZLinkBackendAdapterProvider backendFactory,
        ZLinkBackendAdapterOptions adapterOptions,
        ZLinkFrameworkRegistration registration,
        ZLinkMessageSerializer serializer,
        ZLinkHandlerActivator handlerFactory,
        ZLinkRuntimeEventDispatcher eventDispatcher) {
        this(
            backend,
            context,
            false,
            backendFactory,
            adapterOptions,
            registration,
            serializer,
            handlerFactory,
            eventDispatcher,
            (ignoredBackend, ignoredKey) -> (ignoredSubmission, ignoredCleanup) ->
                CompletableFuture.failedFuture(new IllegalStateException(
                    "one-way admission factory is required")));
    }

    public ZLinkChannelRuntime(
        ZLinkChannelBackendAdapter backend,
        ZLinkBackendContext context,
        ZLinkBackendAdapterProvider backendFactory,
        ZLinkBackendAdapterOptions adapterOptions,
        ZLinkFrameworkRegistration registration,
        ZLinkMessageSerializer serializer,
        ZLinkHandlerActivator handlerFactory,
        ZLinkRuntimeEventDispatcher eventDispatcher,
        java.util.function.BiFunction<
            ZLinkBackendObject,
            ZLinkBackendAdmissionKey,
            java.util.function.BiFunction<
                java.util.function.Supplier<Boolean>,
                Runnable,
                CompletionStage<Void>>> admission) {
        this(
            backend,
            context,
            false,
            backendFactory,
            adapterOptions,
            registration,
            serializer,
            handlerFactory,
            eventDispatcher,
            admission);
    }

    private ZLinkChannelRuntime(
        ZLinkChannelBackendAdapter backend,
        ZLinkBackendContext context,
        boolean ownsContext,
        ZLinkBackendAdapterProvider backendFactory,
        ZLinkBackendAdapterOptions adapterOptions,
        ZLinkFrameworkRegistration registration,
        ZLinkMessageSerializer serializer,
        ZLinkHandlerActivator handlerFactory,
        ZLinkRuntimeEventDispatcher eventDispatcher) {
        this(
            backend,
            context,
            ownsContext,
            backendFactory,
            adapterOptions,
            registration,
            serializer,
            handlerFactory,
            eventDispatcher,
            (ignoredBackend, ignoredKey) -> (ignoredSubmission, ignoredCleanup) ->
                CompletableFuture.failedFuture(new IllegalStateException(
                    "one-way admission factory is required")));
    }

    private ZLinkChannelRuntime(
        ZLinkChannelBackendAdapter backend,
        ZLinkBackendContext context,
        boolean ownsContext,
        ZLinkBackendAdapterProvider backendFactory,
        ZLinkBackendAdapterOptions adapterOptions,
        ZLinkFrameworkRegistration registration,
        ZLinkMessageSerializer serializer,
        ZLinkHandlerActivator handlerFactory,
        ZLinkRuntimeEventDispatcher eventDispatcher,
        java.util.function.BiFunction<
            ZLinkBackendObject,
            ZLinkBackendAdmissionKey,
            java.util.function.BiFunction<
                java.util.function.Supplier<Boolean>,
                Runnable,
                CompletionStage<Void>>> admission) {
        this.serializer = Objects.requireNonNull(serializer, "serializer");
        this.replyDecoder = new ZLinkChannelReplyDecoder(this.serializer);
        this.codecs = Objects.requireNonNull(registration.codecs(), "codecs");
        this.handlerFactory = Objects.requireNonNull(handlerFactory, "handlerFactory");
        this.spotAddressResolver = resolveSpotAddressResolver(this.handlerFactory);
        this.handlerExecutor = systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext
            .propagating(Objects.requireNonNull(registration.handlerExecutor(), "handlerExecutor"));
        this.dispatchRegistry = new ZLinkChannelDispatchRegistry(
            registration.serialExecutor());
        this.suspendHandlerInvokers = registration.suspendHandlerInvokers();
        this.filterTypes = List.copyOf(registration.filters());
        this.channelHandlerInvoker = new ZLinkChannelHandlerInvoker(
            this.serializer,
            codecs,
            this.handlerFactory,
            this.handlerExecutor,
            suspendHandlerInvokers,
            filterTypes);
        this.inboundDispatchBudget = registration.inboundDispatchBudget();
        this.receiveLoops = new ZLinkChannelReceiveLoops(
            () -> running, inboundDispatchBudget);
        this.defaultRequestTimeout = registration.defaultRequestTimeout();
        this.spotRouteBridgeDrainer = new ZLinkSpotRouteBridgeDrainer(
            sockets.spotRouteBridges(),
            spotRouteBridgeDrainLoopExecutor,
            () -> running,
            this::reportSpotRouteBridgeDrainFailure);
        this.backendFactory = backendFactory;
        this.adapterOptions = adapterOptions;
        this.channelBackend = Objects.requireNonNull(backend, "backend");
        this.dispatchErrors = new ZLinkDispatchErrorReporter(
            registration.dispatchOptions(),
            handlerFactory,
            this.handlerExecutor,
            eventDispatcher);
        this.callRuntime = new ZLinkChannelCallRuntime(
            dispatchErrors.flow(),
            timeoutExecutor,
            defaultRequestTimeout,
            replyDecoder,
            this::sendToSpotViaRouterChannel,
            this::requestToSpotViaRouterChannel,
            new ZLinkOneWayCalls(admission));
        this.dispatchReporter = new ZLinkChannelDispatchReporter(dispatchErrors);
        this.messageDispatcher = new ZLinkChannelMessageDispatcher(
            dispatchRegistry,
            channelHandlerInvoker,
            dispatchReporter,
            dispatchErrors.flow(),
            inboundDispatchBudget);
        this.routeDispatcher = new ZLinkChannelRouteDispatcher(
            sockets,
            dispatchRegistry,
            spotRouteBridgeRawReplies,
            channelHandlerInvoker,
            dispatchReporter,
            dispatchErrors.flow(),
            spotRouteBridgeDrainer,
            this::resolveSpotRouteBridgeForDispatch,
            inboundDispatchBudget);
        this.context = Objects.requireNonNull(context, "context");
        this.ownsContext = ownsContext;
        this.clientServerMonitoringBackend =
            tryCreateClientServerMonitoringBackend(
                backendFactory,
                adapterOptions,
                registration.channels());
        this.fanoutMonitoringBackend =
            tryCreateFanoutMonitoringBackend(
                backendFactory,
                adapterOptions,
                registration.channels());
        ZLinkScannedHandlerCatalog handlerCatalog =
            ZLinkHandlerScanner.scan(registration.handlerPackageMarkers());
        ZLinkChannelHandlerCatalog channelHandlers =
            new ZLinkChannelHandlerCatalog(handlerCatalog);
        ZLinkChannelRuntimeConfigurator configurator = new ZLinkChannelRuntimeConfigurator(
            backend,
            this.context,
            sockets,
            dispatchRegistry,
            channelHandlers,
            this::startRequestLoop,
            this::startRouteLoop,
            this::startSubscribeLoop,
            clientServerMonitoringBackend != null);
        for (ChannelRegistration channel : registration.channels()) {
            sockets.registerChannel(channel);
            configurator.configure(channel);
        }
        if (backendFactory == null
            || clientServerMonitoringBackend == null) {
            sockets.enableUnmanagedBackendClientMode();
            for (ChannelRegistration channel : registration.channels()) {
                ZLinkBackendDealerSocket client =
                    sockets.client(channel.name());
                if (client != null) {
                    channel.clientConnections().attach(client);
                }
            }
        }
        if (backendFactory != null) {
            sockets.initializeClientServerServerDescriptors(
                "runtime-" + java.util.UUID.randomUUID());
            if (clientServerMonitoringBackend != null) {
                attachManualClientServerAdmissions(
                    registration.channels());
            }
            attachProcessLocalClientServerAdmissions(
                registration.channels());
            timeoutExecutor.scheduleAtFixedRate(
                () -> sockets.tickClientServerLiveness(
                    System.nanoTime(), defaultRequestTimeout),
                100,
                100,
                TimeUnit.MILLISECONDS);
        }
        installClientServerLocationRuntime(handlerFactory);
        installFanoutLocationRuntime(handlerFactory);
    }


    public List<AutoConnectSurface> autoConnectSurfaces() {
        return sockets.autoConnectSurfaces();
    }

    private void installClientServerLocationRuntime(
        ZLinkHandlerActivator activator) {
        ZLinkClientServerRuntimeConfiguration configuration;
        try {
            configuration = (ZLinkClientServerRuntimeConfiguration)
                activator.create(
                    ZLinkClientServerRuntimeConfiguration.class);
        } catch (RuntimeException unavailable) {
            return;
        }
        if (configuration == null || configuration.store() == null) {
            return;
        }
        if (backendFactory == null || adapterOptions == null) {
            throw new ZLinkConfigurationException(
                "automatic ClientServer discovery requires a backend provider");
        }
        ZLinkClientServerLocationRuntime runtime =
            new ZLinkClientServerLocationRuntime(
                configuration.store(),
                configuration.owner(),
                backendFactory,
                context,
                adapterOptions,
                sockets,
                configuration.options().pollingInterval(),
                1000);
        List<AutoConnectSurface> surfaces = autoConnectSurfaces();
        configuration.install(
            new ZLinkClientServerRuntimeConfiguration.Lifecycle() {
                @Override
                public CompletionStage<Void> start() {
                    return runtime.start(surfaces);
                }

                @Override
                public CompletionStage<Void> markDraining() {
                    return runtime.markDraining();
                }

                @Override
                public CompletionStage<Void> stop() {
                    return runtime.stop();
                }
            });
    }

    private static ZLinkMonitoringBackendAdapter
        tryCreateClientServerMonitoringBackend(
            ZLinkBackendAdapterProvider backendFactory,
            ZLinkBackendAdapterOptions adapterOptions,
            List<ChannelRegistration> registrations) {
        if (backendFactory == null
            || registrations.stream().noneMatch(
                channel -> channel.kind() == ChannelKind.CLIENT_SERVER
                    && channel.clientEnabled())) {
            return null;
        }
        try {
            return backendFactory.createMonitoringAdapter(
                adapterOptions);
        } catch (UnsupportedOperationException unavailable) {
            return null;
        }
    }

    private void installFanoutLocationRuntime(
        ZLinkHandlerActivator activator) {
        ZLinkFanoutRuntimeConfiguration configuration;
        try {
            configuration = (ZLinkFanoutRuntimeConfiguration)
                activator.create(ZLinkFanoutRuntimeConfiguration.class);
        } catch (RuntimeException unavailable) {
            return;
        }
        if (configuration == null || configuration.store() == null) {
            return;
        }
        boolean hasAutomaticSubscriber = autoConnectSurfaces().stream()
            .anyMatch(surface ->
                surface.type() == ZLinkAutoConnectType.FANOUT
                    && surface.role() == ZLinkLocationRole.SUB);
        if (hasAutomaticSubscriber
            && (backendFactory == null
                || adapterOptions == null
                || fanoutMonitoringBackend == null)) {
            throw new ZLinkConfigurationException(
                "automatic classic fanout discovery requires "
                    + "a monitoring backend");
        }
        ZLinkMonitoringBackendAdapter monitoring =
            fanoutMonitoringBackend == null
                ? socket -> {
                    throw new UnsupportedOperationException(
                        "fanout subscriber monitoring is unavailable");
                }
                : fanoutMonitoringBackend;
        ZLinkFanoutLocationRuntime runtime =
            new ZLinkFanoutLocationRuntime(
                configuration.store(),
                configuration.owner(),
                channelBackend,
                monitoring,
                context,
                sockets,
                configuration.options().pollingInterval(),
                1000,
                messageDispatcher::dispatchPublish);
        fanoutLocationRuntime = runtime;
        List<AutoConnectSurface> surfaces = autoConnectSurfaces();
        configuration.install(
            new ZLinkFanoutRuntimeConfiguration.Lifecycle() {
                @Override
                public CompletionStage<Void> start() {
                    return runtime.start(surfaces);
                }

                @Override
                public CompletionStage<Void> markDraining() {
                    return runtime.markDraining();
                }

                @Override
                public CompletionStage<Void> stop() {
                    return runtime.stop();
                }
            });
    }

    private static ZLinkMonitoringBackendAdapter
        tryCreateFanoutMonitoringBackend(
            ZLinkBackendAdapterProvider backendFactory,
            ZLinkBackendAdapterOptions adapterOptions,
            List<ChannelRegistration> registrations) {
        if (backendFactory == null
            || registrations.stream().noneMatch(
                channel -> channel.kind() == ChannelKind.FANOUT
                    && channel.automaticSubscriberEnabled())) {
            return null;
        }
        try {
            return backendFactory.createMonitoringAdapter(
                adapterOptions);
        } catch (UnsupportedOperationException unavailable) {
            return null;
        }
    }

    private void attachManualClientServerAdmissions(
        List<ChannelRegistration> registrations) {
        for (ChannelRegistration registration : registrations) {
            if (registration.kind() != ChannelKind.CLIENT_SERVER
                || !registration.clientEnabled()) {
                continue;
            }
            String channelName = registration.name();
            registration.clientConnections().attach(
                new ZLinkBackendConnectableSocket() {
                    @Override
                    public String name() {
                        return "clientServerAdmissionController";
                    }

                    @Override
                    public void bind(String endpoint) {
                        throw new UnsupportedOperationException(
                            "ClientServer client controller cannot bind");
                    }

                    @Override
                    public void connect(String endpoint) {
                        openManualClientServerConnection(
                            channelName, endpoint);
                    }

                    @Override
                    public void disconnect(String endpoint) {
                        String connectionId =
                            manualConnectionId(channelName, endpoint);
                        sockets.removeClientServerConnection(connectionId);
                    }

                    @Override
                    public void close() {
                    }
                });
        }
    }

    private void attachProcessLocalClientServerAdmissions(
        List<ChannelRegistration> registrations) {
        for (ChannelRegistration registration : registrations) {
            if (registration.kind() != ChannelKind.CLIENT_SERVER
                || !registration.clientEnabled()
                || registration.serverBinds().isEmpty()) {
                continue;
            }
            ZLinkClientServerServerDescriptor local =
                sockets.clientServerServerDescriptor(registration.name());
            if (local != null) {
                // Local selection still traverses DEALER -> ROUTER admission.
                openManualClientServerConnection(
                    registration.name(), local.endpoint());
            }
        }
    }

    private void openManualClientServerConnection(
        String channelName,
        String endpoint) {
        String connectionId =
            manualConnectionId(channelName, endpoint);
        ZLinkBackendDealerSocket dealer =
            channelBackend.createDealerSocket(context);
        dealer.setChannelName(channelName);
        ZLinkClientServerServerDescriptor pending =
            new ZLinkClientServerServerDescriptor(
                channelName,
                RoutingId.from(java.util.UUID.randomUUID()),
                1,
                1,
                endpoint,
                0,
                ZLinkFrameworkRuntimeState.PREPARING,
                "default",
                "manual",
                1,
                java.time.Instant.EPOCH);
        try {
            sockets.addClientServerConnection(
                connectionId, pending, dealer);
            ZLinkBackendSocketMonitor monitor =
                clientServerMonitoringBackend.openSocketMonitor(dealer);
            sockets.registerClientServerMonitor(connectionId, monitor);
            monitor.onEvent(event -> {
                if (isConnectionReady(event.event())) {
                    ZLinkChannelSocketRegistry.AdmissionFence fence =
                        sockets.clientServerTransportReady(
                            connectionId, dealer);
                    if (fence != null) {
                        requestManualClientServerAdmission(
                            connectionId,
                            channelName,
                            endpoint,
                            dealer,
                            fence);
                    }
                } else if (isConnectionTerminated(event.event())) {
                    sockets.clientServerTransportTerminated(
                        connectionId, dealer);
                }
            });
            dealer.connect(endpoint);
        } catch (RuntimeException failure) {
            sockets.removeClientServerConnection(connectionId);
            throw failure;
        }
    }

    private void requestManualClientServerAdmission(
        String connectionId,
        String channelName,
        String endpoint,
        ZLinkBackendDealerSocket dealer,
        ZLinkChannelSocketRegistry.AdmissionFence fence) {
        byte[] hello = ZLinkClientServerServiceWire.encodeHello(
            new ZLinkClientServerServiceWire.Hello(
                channelName, "default", Integer.MAX_VALUE));
        try (Message message = Message.from(hello)) {
            boolean submitted = dealer.request(
                List.of(message),
                reply -> completeManualClientServerAdmission(
                    connectionId,
                    channelName,
                    endpoint,
                    fence,
                    reply),
                SendFlags.DONT_WAIT,
                defaultRequestTimeout(channelName));
            if (!submitted) {
                sockets.reconnectClientServerConnection(connectionId);
            }
        } catch (ZlinkSubmitException ignored) {
            // A monitor callback may race with draining or socket teardown.
            // Treat the failed admission request as a terminated candidate;
            // reconnect policy decides whether a later attempt is required.
            sockets.clientServerTransportTerminated(connectionId, dealer);
        }
    }

    private static String manualConnectionId(
        String channelName,
        String endpoint) {
        return "manual\0" + channelName + '\0' + endpoint;
    }

    private void completeManualClientServerAdmission(
        String connectionId,
        String channelName,
        String endpoint,
        ZLinkChannelSocketRegistry.AdmissionFence fence,
        ZLinkBackendReceived reply) {
        try (reply) {
            if (reply.result() != ZLinkBackendRequestResult.OK
                || reply.parts().size() != 1) {
                sockets.reconnectClientServerConnection(connectionId);
                return;
            }
            ZLinkClientServerServiceWire.Control control =
                ZLinkClientServerServiceWire.decode(
                    reply.parts().get(0).toByteArray());
            if (!(control instanceof ZLinkClientServerServiceWire.Admit admit)
                || !admit.admission().channelName().equals(channelName)
                || !admit.admission().securityIdentity().equals("default")) {
                sockets.reconnectClientServerConnection(connectionId);
                return;
            }
            ZLinkClientServerServiceWire.Admission value =
                admit.admission();
            ZLinkClientServerServerDescriptor descriptor =
                new ZLinkClientServerServerDescriptor(
                    value.channelName(),
                    value.serverRid(),
                    value.lifecycleGeneration(),
                    value.descriptorRevision(),
                    value.advertisedEndpoint(),
                    value.weight(),
                    value.state(),
                    value.securityIdentity(),
                    "manual",
                    1,
                    java.time.Instant.EPOCH);
            sockets.admitClientServerConnection(
                connectionId, descriptor, fence);
        } catch (RuntimeException failure) {
            sockets.reconnectClientServerConnection(connectionId);
        }
    }

    private static boolean isConnectionReady(String event) {
        return "CONNECTION_READY".equals(event)
            || "ConnectionReady".equals(event);
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

    @Override
    public ZLinkSendCall sendToChannel(String channelName, Object message) {
        rejectAfterRelocationReady("Channel send");
        ZLinkPayloadEncoding.EncodedPayload encoded =
            encodePayload(message);
        ZLinkBackendDealerSocket client = sockets.clientForOutbound(channelName);
        if (client != null) {
            return new SendCall(
                callRuntime,
                client,
                encoded.payload(),
                Optional.of(encoded.packetName()),
                encoded.contentType());
        }
        ZLinkInternalSpotNode node = sockets.spotRouterNode(channelName);
        if (node != null) {
            return new MeshChannelRouteSendCall(
                callRuntime,
                channelName,
                node,
                encoded.payload(),
                Optional.of(encoded.packetName()),
                encoded.contentType(),
                ZLinkApplicationMetadata.empty());
        }
        if (sockets.hasClientRegistration(channelName)) {
            return new SendCall(
                callRuntime,
                awaitClientServerTarget(channelName),
                encoded.payload(),
                Optional.of(encoded.packetName()),
                encoded.contentType());
        }
        throw new ZLinkConfigurationException(
            "channel is not configured: " + channelName);
    }

    private ZLinkPayloadEncoding.EncodedPayload encodePayload(Object message) {
        Class<?> payloadType = message == null ? null : message.getClass();
        return ZLinkPayloadEncoding.encode(
            serializer,
            message,
            codecs.contentTypeFor(payloadType));
    }

    @Override
    public ZLinkRequestCall requestToChannel(String channelName, Object message) {
        rejectAfterRelocationReady("Channel request");
        ZLinkPayloadEncoding.EncodedPayload encoded =
            encodePayload(message);
        ZLinkBackendDealerSocket client = sockets.clientForOutbound(channelName);
        if (client != null) {
            return new RequestCall(
                callRuntime,
                client,
                encoded.payload(),
                Optional.of(encoded.packetName()),
                defaultRequestTimeout(channelName),
                ZLinkRequestMetricTags.forChannel(channelName),
                encoded.contentType());
        }
        ZLinkInternalSpotNode node = sockets.spotRouterNode(channelName);
        if (node != null) {
            return new MeshChannelRouteRequestCall(
                callRuntime,
                channelName,
                node,
                encoded.payload(),
                Optional.of(encoded.packetName()),
                defaultRequestTimeout(channelName),
                encoded.contentType(),
                ZLinkApplicationMetadata.empty());
        }
        if (sockets.hasClientRegistration(channelName)) {
            return new RequestCall(
                callRuntime,
                awaitClientServerTarget(channelName),
                encoded.payload(),
                Optional.of(encoded.packetName()),
                defaultRequestTimeout(channelName),
                ZLinkRequestMetricTags.forChannel(channelName),
                encoded.contentType());
        }
        throw new ZLinkConfigurationException(
            "channel is not configured: " + channelName);
    }

    /**
     * Resolves the ClientServer send target for a Channel whose ready candidate set is still empty,
     * per {@code framework/doc/framework/common/spec/08-channel-messaging.ko.md} 짠3.2: the call
     * waits a bounded period and then fails with no-target. The bound is the shorter of this
     * Channel's request timeout and five seconds, mirroring the .NET reference
     * {@code ZLinkClientServerClientRuntime.WaitForReadyAsync}. Framework startup never waits for
     * admission, and this wait does not trigger one.
     */
    private ZLinkBackendDealerSocket awaitClientServerTarget(String channelName) {
        Duration channelTimeout = defaultRequestTimeout(channelName);
        Duration bound = channelTimeout.compareTo(CLIENT_SERVER_READY_WAIT_CAP) < 0
            ? channelTimeout
            : CLIENT_SERVER_READY_WAIT_CAP;
        ZLinkBackendDealerSocket ready =
            sockets.awaitClientForOutbound(channelName, bound);
        if (ready == null) {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.NOT_FOUND,
                "client/server channel has no ready server: " + channelName);
        }
        return ready;
    }

    @Override
    public ZLinkFanoutPublishCall publish(String channelName, Object message) {
        rejectAfterRelocationReady("Channel publish");
        ZLinkPayloadEncoding.EncodedPayload encoded =
            encodePayload(message);
        return new PublishCall(
            callRuntime,
            requirePublisher(channelName),
            encoded.packetName(),
            encoded.payload(),
            Optional.of(encoded.packetName()),
            encoded.contentType());
    }

    @Override
    public ZLinkFanoutPublishCall publish(
        String channelName,
        String topic,
        Object message) {
        rejectAfterRelocationReady("Channel publish");
        ZLinkPayloadEncoding.EncodedPayload encoded =
            encodePayload(message);
        return new PublishCall(
            callRuntime,
            requirePublisher(channelName),
            topic,
            encoded.payload(),
            Optional.of(encoded.packetName()),
            encoded.contentType());
    }

    private static void rejectAfterRelocationReady(String operation) {
        systems.zlink.framework.runtime.internal.handlers
            .ZLinkSuspendInvocationContext.rejectAfterRelocationReady(
                operation);
    }

    @Override
    public ZLinkSendCall sendToNode(String channelName, RoutingId target, Object message) {
        ZLinkPayloadEncoding.EncodedPayload encoded =
            encodePayload(message);
        ZLinkBackendRouterSocket router = sockets.routeRouter(channelName);
        if (router == null) {
            ZLinkInternalSpotNode node = sockets.spotRouterNode(channelName);
            if (node != null) {
                return new MeshNodeRouteSendCall(
                    callRuntime,
                    node,
                    target,
                    encoded.payload(),
                    Optional.of(encoded.packetName()),
                    encoded.contentType(),
                    ZLinkApplicationMetadata.empty());
            }
            throw new ZLinkConfigurationException(
                "route mesh channel is not configured: " + channelName);
        }
        return new RouteSendCall(
            callRuntime,
            router,
            target,
            encoded.payload(),
            Optional.of(encoded.packetName()),
            encoded.contentType());
    }

    @Override
    public systems.zlink.framework.spots.ZLinkSpotSendCall sendToSpot(
        String spotId,
        Object message) {
        Objects.requireNonNull(spotId, "spotId");
        ZLinkPayloadEncoding.EncodedPayload encoded =
            encodePayload(message);
        return new RouteSpotSendCall(
            callRuntime,
            null,
            spotAddressResolver,
            () -> instanceSpotCallRuntime,
            spotId,
            encoded.payload(),
            Optional.of(encoded.packetName()),
            encoded.contentType(),
            false, null, null,
            ZLinkApplicationMetadata.empty());
    }

    @Override
    public ZLinkRequestCall requestToNode(String channelName, RoutingId target, Object message) {
        ZLinkPayloadEncoding.EncodedPayload encoded =
            encodePayload(message);
        ZLinkBackendRouterSocket router = sockets.routeRouter(channelName);
        if (router == null) {
            ZLinkInternalSpotNode node = sockets.spotRouterNode(channelName);
            if (node != null) {
                return new MeshNodeRouteRequestCall(
                    callRuntime,
                    channelName,
                    node,
                    target,
                    encoded.payload(),
                    Optional.of(encoded.packetName()),
                    defaultRequestTimeout(channelName),
                    encoded.contentType(),
                    ZLinkApplicationMetadata.empty());
            }
            throw new ZLinkConfigurationException(
                "route mesh channel is not configured: " + channelName);
        }
        return new RouteRequestCall(
            callRuntime,
            channelName,
            router,
            target,
            encoded.payload(),
            Optional.of(encoded.packetName()),
            defaultRequestTimeout(channelName),
            encoded.contentType());
    }

    @Override
    public systems.zlink.framework.spots.ZLinkSpotRequestCall requestToSpot(
        String spotId,
        Object message) {
        Objects.requireNonNull(spotId, "spotId");
        ZLinkPayloadEncoding.EncodedPayload encoded =
            encodePayload(message);
        return new RouteSpotRequestCall(
            callRuntime,
            null,
            spotAddressResolver,
            () -> instanceSpotCallRuntime,
            spotId,
            encoded.payload(),
            Optional.of(encoded.packetName()),
            Duration.ofSeconds(1),
            encoded.contentType(),
            false, null, null,
            ZLinkApplicationMetadata.empty());
    }

    private volatile systems.zlink.framework.runtime.internal.spots
        .ZLinkInstanceSpotCallRuntime instanceSpotCallRuntime;

    public void registerInstanceSpotCallRuntime(
        systems.zlink.framework.runtime.internal.spots
            .ZLinkInstanceSpotCallRuntime runtime) {
        instanceSpotCallRuntime = java.util.Objects.requireNonNull(
            runtime, "runtime");
    }

    private static SpotTransportAddressResolver resolveSpotAddressResolver(
        ZLinkHandlerActivator handlerFactory) {
        try {
            return (SpotTransportAddressResolver) handlerFactory.create(
                SpotTransportAddressResolver.class);
        } catch (RuntimeException ignored) {
            return null;
        }
    }

    public void registerSpotRouteBridgeOwner(
        Supplier<ZLinkInternalSpotNode> owner) {
        this.spotRouteBridgeOwner = Objects.requireNonNull(owner, "owner");
    }

    public void registerSpotRouteBridgeDispatchDrainer(Runnable drainer) {
        spotRouteBridgeDrainer.setDispatchDrainer(Objects.requireNonNull(drainer, "drainer"));
    }

    public boolean attachSpotRouteBridgeToServer(
        String channelName,
        ZLinkInternalSpotNode node) {
        ZLinkBackendRouterSocket router = sockets.server(channelName);
        if (router == null) {
            router = sockets.routeRouter(channelName);
        }
        if (router == null) {
            return false;
        }
        ZLinkBackendSpotRouteBridge bridge = node.createRouteBridge();
        bridge.attachRouterChannel(
            channelName,
            router);
        sockets.registerSpotRouteBridge(channelName, bridge);
        spotRouteBridgeDrainer.start();
        return true;
    }

    public void registerSpotRouterNode(
        String routerChannelId,
        ZLinkInternalSpotNode node) {
        String channelId = requireRouterChannelId(routerChannelId);
        sockets.registerSpotRouterNode(channelId, Objects.requireNonNull(node, "node"));
    }

    private Duration defaultRequestTimeout(String channelName) {
        ChannelRegistration registration = sockets.registration(channelName);
        if (registration != null && registration.defaultRequestTimeout() != null) {
            return registration.defaultRequestTimeout();
        }
        return defaultRequestTimeout;
    }

    private Duration effectiveRouteTimeout(Duration timeout) {
        return timeout == null || timeout.isZero()
            ? defaultRequestTimeout
            : timeout;
    }

    public void registerRouteInternalRequestHandler(
        String packetName,
        RouteInternalRequestHandler handler) {
        if (packetName == null || packetName.isBlank()) {
            throw new ZLinkConfigurationException("internal route packet name is required");
        }
        dispatchRegistry.registerInternalRequest(
            packetName,
            Objects.requireNonNull(handler, "handler"));
    }

    /** Sends one framework-internal request without exposing its packet as a public codec type. */
    public CompletionStage<Message> requestInternalToNode(
        String channelName,
        RoutingId target,
        String packetName,
        Message payload,
        Duration timeout) {
        CompletableFuture<Message> result = new CompletableFuture<>();
        Duration effectiveTimeout = effectiveRouteTimeout(timeout);
        callRuntime.track(result, effectiveTimeout);
        List<Message> requestParts = ZLinkChannelCallRuntime.parts(
            Optional.of(packetName), payload);
        try {
            callRuntime.submitRoute(
                requireRouteRouter(channelName),
                target,
                requestParts,
                reply -> {
                    try {
                        if (reply.result() != ZLinkBackendRequestResult.OK) {
                            result.completeExceptionally(new ZLinkFrameworkException(
                                ZLinkFrameworkErrorKind.INTERNAL_FAILURE,
                                "internal route request failed: " + reply.result()));
                        } else if (reply.parts().isEmpty()) {
                            result.completeExceptionally(new ZLinkConfigurationException(
                                "internal route request reply was empty: " + packetName));
                        } else {
                            result.complete(Message.from(reply.parts().get(0)));
                        }
                    } catch (RuntimeException error) {
                        result.completeExceptionally(error);
                    } finally {
                        reply.parts().forEach(Message::close);
                    }
                },
                effectiveTimeout,
                result);
        } finally {
            requestParts.forEach(Message::close);
        }
        return ZLinkAsyncSerialQueue.manageCurrent(result);
    }

    public CompletionStage<Void> sendToSpotViaRouterChannel(
        String routerChannelId,
        RoutingId targetNodeRid,
        String targetSpotId,
        List<Message> spotParts) {
        return sendToSpotViaRouterChannel(
            routerChannelId, targetNodeRid, targetSpotId, 0L, spotParts);
    }

    public CompletionStage<Void> sendToSpotViaRouterChannel(
        String routerChannelId,
        RoutingId targetNodeRid,
        String targetSpotId,
        long targetSpotGeneration,
        List<Message> spotParts) {
        ZLinkSpotRouteTarget target = resolveSpotRouteTarget(routerChannelId, targetNodeRid);
        if (target instanceof ZLinkSpotRouterNodeTarget spotRouterNodeTarget) {
            return sendToSpotViaSpotRouterNode(
                routerChannelId,
                spotRouterNodeTarget.node(),
                targetNodeRid,
                targetSpotId,
                targetSpotGeneration,
                spotParts);
        }
        try {
            ZLinkBackendSpotRouteBridge bridge = requireSpotRouteBridge(routerChannelId);
            List<byte[]> bridgePayloads = spotParts.stream()
                .map(Message::toByteArray)
                .toList();
            CompletableFuture<Void> result = new CompletableFuture<>();
            ZLinkSpotRouteBridgeDispatcher.submitSendWithRetry(
                bridge,
                routerChannelId,
                targetNodeRid,
                targetSpotId,
                bridgePayloads,
                effectiveRouteTimeout(defaultRequestTimeout(routerChannelId)),
                timeoutExecutor,
                result);
            return result;
        } catch (RuntimeException ex) {
            CompletableFuture<Void> result = new CompletableFuture<>();
            result.completeExceptionally(ex);
            return result;
        }
    }

    public CompletionStage<List<Message>> requestToSpotViaRouterChannel(
        String routerChannelId,
        RoutingId targetNodeRid,
        String targetSpotId,
        List<Message> spotParts,
        Duration timeout) {
        return requestToSpotViaRouterChannel(
            routerChannelId,
            targetNodeRid,
            targetSpotId,
            0L,
            spotParts,
            timeout);
    }

    public CompletionStage<List<Message>> requestToSpotViaRouterChannel(
        String routerChannelId,
        RoutingId targetNodeRid,
        String targetSpotId,
        long targetSpotGeneration,
        List<Message> spotParts,
        Duration timeout) {
        trace("spot-route request-start router=" + routerChannelId
            + " targetNode=" + targetNodeRid
            + " targetSpot=" + targetSpotId
            + " parts=" + describeTraceParts(spotParts));
        ZLinkSpotRouteTarget target = resolveSpotRouteTarget(routerChannelId, targetNodeRid);
        if (target instanceof ZLinkSpotRouterNodeTarget spotRouterNodeTarget) {
            trace("spot-route request-path=spot-router-node router=" + routerChannelId
                + " targetNode=" + targetNodeRid
                + " targetSpot=" + targetSpotId);
            return requestToSpotViaSpotRouterNode(
                routerChannelId,
                spotRouterNodeTarget.node(),
                targetNodeRid,
                targetSpotId,
                targetSpotGeneration,
                spotParts,
                timeout);
        }
        CompletableFuture<List<Message>> result = new CompletableFuture<>();
        callRuntime.track(result, timeout);
        try {
            ZLinkBackendSpotRouteBridge bridge = requireSpotRouteBridge(routerChannelId);
            trace("spot-route request-path=route-bridge router=" + routerChannelId
                + " targetNode=" + targetNodeRid
                + " targetSpot=" + targetSpotId);
            ZLinkSpotRouteBridgeDispatcher.submitRequest(
                bridge,
                routerChannelId,
                targetNodeRid,
                targetSpotId,
                copyMessages(spotParts),
                timeout,
                spotRouteBridgeExecutor,
                spotRouteBridgeRawReplies,
                result);
            return result;
        } catch (RuntimeException ex) {
            trace("spot-route request-exception router=" + routerChannelId
                + " targetNode=" + targetNodeRid
                + " targetSpot=" + targetSpotId
                + " error=" + ex);
            spotRouteBridgeRawReplies.remove(routerChannelId, result);
            result.completeExceptionally(ex);
            return result;
        }
    }

    private ZLinkSpotRouteTarget resolveSpotRouteTarget(
        String routerChannelId,
        RoutingId targetNodeRid) {
        if (spotRouteBridgeOwner != null) {
            ZLinkInternalSpotNode localNode = spotRouteBridgeOwner.get();
            if (localNode != null && localNode.routingId().equals(targetNodeRid)) {
                return new ZLinkSpotRouterNodeTarget(localNode);
            }
        }
        ChannelRegistration registration = sockets.registration(routerChannelId);
        if (registration != null && registration.kind() == ChannelKind.ROUTE_MESH) {
            return new ZLinkRouteBridgeTarget();
        }
        ZLinkInternalSpotNode spotRouterNode = sockets.spotRouterNode(routerChannelId);
        if (spotRouterNode != null) {
            return new ZLinkSpotRouterNodeTarget(spotRouterNode);
        }
        trace("spot-route missing-router router=" + routerChannelId);
        throw new ZLinkConfigurationException(
            "route mesh channel is not configured: " + routerChannelId);
    }

    private CompletionStage<Void> sendToSpotViaSpotRouterNode(
        String routerChannelId,
        ZLinkInternalSpotNode node,
        RoutingId targetNodeRid,
        String targetSpotId,
        long targetSpotGeneration,
        List<Message> spotParts) {
        return ZLinkSpotRouterNodeDispatcher.send(
            routerChannelId,
            node,
            targetNodeRid,
            targetSpotId,
            targetSpotGeneration,
            spotParts,
            effectiveRouteTimeout(defaultRequestTimeout(routerChannelId)),
            callRuntime::track,
            callRuntime::retryRouteRequest);
    }

    private CompletionStage<List<Message>> requestToSpotViaSpotRouterNode(
        String routerChannelId,
        ZLinkInternalSpotNode node,
        RoutingId targetNodeRid,
        String targetSpotId,
        long targetSpotGeneration,
        List<Message> spotParts,
        Duration timeout) {
        return ZLinkSpotRouterNodeDispatcher.request(
            routerChannelId,
            node,
            targetNodeRid,
            targetSpotId,
            targetSpotGeneration,
            spotParts,
            timeout,
            callRuntime::track,
            callRuntime::retryRouteRequest);
    }

    static void trace(String message) {
        if (STREAM_TRACE) {
            LOGGER.warning("[zlink-java-stream-trace] " + message);
        }
    }

    static String describeTraceParts(List<Message> parts) {
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

    static long elapsedMillis(long startedNanos) {
        return TimeUnit.NANOSECONDS.toMillis(System.nanoTime() - startedNanos);
    }

    @Override
    public void close() {
        beginClose();
        if (fanoutLocationRuntime != null) {
            fanoutLocationRuntime.close();
        }
        receiveLoops.close();
        spotRouteBridgeDrainLoopExecutor.shutdownNow();
        spotRouteBridgeExecutor.shutdown();
        timeoutExecutor.shutdownNow();
        receiveLoops.awaitTermination();
        awaitTerminated(spotRouteBridgeDrainLoopExecutor);
        awaitTerminated(spotRouteBridgeExecutor);
        awaitTerminated(timeoutExecutor);
        closeSpotRouteBridges();
        sockets.closeAll();
        if (ownsContext) {
            context.close();
        }
    }

    public void closeSpotRouteBridges() {
        sockets.closeSpotRouteBridges();
    }

    public void beginClose() {
        running = false;
        callRuntime.beginClose();
    }

    static void awaitTerminated(java.util.concurrent.ExecutorService executor) {
        try {
            executor.awaitTermination(1, TimeUnit.SECONDS);
        } catch (InterruptedException ex) {
            Thread.currentThread().interrupt();
        }
    }

    private ZLinkBackendDealerSocket requireClient(String channelName) {
        ZLinkBackendDealerSocket client = sockets.client(channelName);
        if (client == null) {
            throw new ZLinkConfigurationException("channel client is not configured: " + channelName);
        }
        return client;
    }

    private ZLinkBackendSpotRouteBridge requireSpotRouteBridge(String channelName) {
        ZLinkBackendSpotRouteBridge existing = sockets.spotRouteBridge(channelName);
        if (existing != null) {
            return existing;
        }
        if (spotRouteBridgeOwner == null) {
            throw new ZLinkConfigurationException(
                "routed SPOT egress requires a router-capable SPOT node");
        }
        ZLinkBackendSpotRouteBridge bridge =
            spotRouteBridgeOwner.get().createRouteBridge();
        ChannelRegistration registration = sockets.registration(channelName);
        if (registration != null && registration.kind() == ChannelKind.ROUTE_MESH) {
            bridge.attachRouterChannel(
                channelName,
                requireRouteRouter(channelName));
        } else {
            throw new ZLinkConfigurationException(
                "SPOT route bridge requires a router channel: " + channelName);
        }
        sockets.registerSpotRouteBridge(channelName, bridge);
        spotRouteBridgeDrainer.start();
        return bridge;
    }

    private ZLinkBackendSpotRouteBridge resolveSpotRouteBridgeForDispatch(
        String channelName) {
        return spotRouteBridgeOwner == null ? null : requireSpotRouteBridge(channelName);
    }

    private ZLinkBackendPublisherSocket requirePublisher(String channelName) {
        ZLinkBackendPublisherSocket publisher = sockets.publisher(channelName);
        if (publisher == null) {
            throw new ZLinkConfigurationException("fanout publisher is not configured: " + channelName);
        }
        return publisher;
    }

    private ZLinkBackendRouterSocket requireRouteRouter(String channelName) {
        ZLinkBackendRouterSocket router = sockets.routeRouter(channelName);
        if (router == null) {
            throw new ZLinkConfigurationException("route mesh channel is not configured: " + channelName);
        }
        return router;
    }

    static List<Message> copyMessages(List<Message> parts) {
        List<Message> copy = new ArrayList<>(parts.size());
        try {
            for (Message part : parts) {
                copy.add(Message.from(part));
            }
            return copy;
        } catch (RuntimeException ex) {
            copy.forEach(Message::close);
            throw ex;
        }
    }

    private void startRequestLoop(String channelName, ZLinkBackendRouterSocket router) {
        receiveLoops.startRequest(
            router,
            received -> {
                if (sockets.tryHandleClientServerControl(
                    channelName, router, received)) {
                    return;
                }
                if (routeDispatcher.dispatchBridgePacket(channelName, received)) {
                    received.close();
                } else {
                    messageDispatcher.dispatchRequest(channelName, router, received);
                }
            },
            error -> reportReceiveFailure(
                ZLinkDispatchErrorSurface.CHANNEL,
                ZLinkDispatchMessageKind.REQUEST,
                channelName,
                error));
    }



    public boolean dispatchSpotRouteBridgePacket(ZLinkBackendReceived received) {
        return routeDispatcher.dispatchBridgePacket(received);
    }

    static boolean looksLikeSpotRouteBridgePacket(List<Message> parts) {
        if (parts.isEmpty()) {
            return false;
        }
        String packetName = parts.get(0).toUtf8String();
        return SPOT_ROUTE_BRIDGE_SEND_PACKET_NAME.equals(packetName)
            || SPOT_ROUTE_BRIDGE_REQUEST_PACKET_NAME.equals(packetName);
    }

    static boolean sameMessageParts(
        List<byte[]> expected,
        List<Message> actual) {
        if (expected.size() != actual.size()) {
            return false;
        }
        for (int i = 0; i < expected.size(); i++) {
            if (!java.util.Arrays.equals(expected.get(i), actual.get(i).toByteArray())) {
                return false;
            }
        }
        return true;
    }

    static boolean sameFirstMessagePart(
        List<byte[]> expected,
        List<Message> actual) {
        if (expected.isEmpty() || actual.isEmpty()) {
            return false;
        }
        return java.util.Arrays.equals(expected.get(0), actual.get(0).toByteArray());
    }

    static boolean isFrameworkErrorReply(List<Message> parts) {
        return ZLinkFrameworkErrorReply.isReply(parts);
    }

    private static boolean isFrameworkErrorPacket(String packetName) {
        return ZLinkFrameworkErrorReply.isPacketName(packetName);
    }

    static String frameworkErrorReplyMessage(List<Message> parts) {
        return ZLinkFrameworkErrorReply.message(parts);
    }

    static ZLinkFrameworkErrorKind frameworkErrorReplyKind(
        List<Message> parts) {
        return ZLinkFrameworkErrorReply.kind(parts);
    }

    private void startRouteLoop(String channelName, ZLinkBackendRouterSocket router) {
        receiveLoops.startRoute(
            router,
            () -> sockets.routeSocketLock(channelName, this),
            () -> spotRouteBridgeDrainer.drainNow(channelName),
            received -> routeDispatcher.dispatch(channelName, router, received),
            error -> reportReceiveFailure(
                ZLinkDispatchErrorSurface.ROUTE_MESH_CHANNEL,
                ZLinkDispatchMessageKind.REQUEST,
                channelName,
                error));
    }

    private void reportSpotRouteBridgeDrainFailure(
        String channelName,
        Throwable error) {
        reportReceiveFailure(
            ZLinkDispatchErrorSurface.ROUTE_MESH_CHANNEL,
            ZLinkDispatchMessageKind.REQUEST,
            channelName,
            error);
    }

    private void reportReceiveFailure(
        ZLinkDispatchErrorSurface surface,
        ZLinkDispatchMessageKind messageKind,
        String channelName,
        Throwable error) {
        dispatchReporter.report(
            surface,
            messageKind,
            ZLinkDispatchErrorReason.INVALID_FRAME,
            ZLinkDispatchErrorAction.DROP,
            null,
            channelName,
            null,
            error);
    }

    static boolean isNoDataReceive(Throwable error) {
        Throwable current = error;
        while (current != null) {
            if (current instanceof ZlinkRecvException ex) {
                return true;
            }
            current = current.getCause();
        }
        return false;
    }


    private void startSubscribeLoop(
        String channelName,
        ZLinkBackendSubscriberSocket subscriber) {
        receiveLoops.startSubscribe(
            subscriber,
            received -> messageDispatcher.dispatchPublish(channelName, received),
            error -> reportReceiveFailure(
                ZLinkDispatchErrorSurface.CHANNEL,
                ZLinkDispatchMessageKind.PUBLISH,
                channelName,
                error));
    }






    static String requestErrorSummary(Throwable error) {
        Throwable current = error;
        while (current != null) {
            if (current instanceof ZlinkRequestException request) {
                return "request result=" + request.getResult()
                    + ", errno=" + request.getNativeErrno();
            }
            if (current instanceof ZlinkSubmitException submit) {
                return "submit result=" + submit.getResult()
                    + ", errno=" + submit.getNativeErrno();
            }
            current = current.getCause();
        }
        return String.valueOf(error);
    }

    @FunctionalInterface
    public interface RouteInternalRequestHandler {
        CompletionStage<Message> handle(RoutingId sourceRoutingId, Message payload);
    }
}
