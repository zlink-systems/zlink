package systems.zlink.framework.runtime.host;

import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;

import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;

import systems.zlink.framework.runtime.internal.backend.*;

import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.actors.ZLinkActorClient;
import systems.zlink.framework.actors.ZLinkActorDirectory;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.channels.ZLinkClient;
import systems.zlink.framework.channels.ZLinkChannelRuntimeOptions;
import systems.zlink.framework.channels.ZLinkFanoutClient;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkRuntimeEventDispatcher;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.configuration.ZLinkFrameworkRegistration;
import systems.zlink.framework.runtime.actors.ZLinkActorRuntime;
import systems.zlink.framework.runtime.actors.ZLinkSessionActorsRuntime;
import systems.zlink.framework.runtime.channels.ZLinkChannelRuntime;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.locations.ZLinkLocationLifecycle;
import systems.zlink.framework.runtime.locations.ZLinkLocationAutoConnectHost;
import systems.zlink.framework.runtime.locations.ZLinkLocationRuntime;
import systems.zlink.framework.runtime.locations.ZLinkLocationReadinessService;
import systems.zlink.framework.runtime.locations.ZLinkRegisteredLocationStores;
import systems.zlink.framework.runtime.locations.ZLinkStoreLocationResolvers;
import systems.zlink.framework.runtime.locations.ZLinkStatefulAuthorityRouteRuntime;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;
import systems.zlink.framework.runtime.mesh.ZLinkMeshNodesRuntime;
import systems.zlink.framework.runtime.spots.ZLinkSpotRuntime;
import systems.zlink.framework.runtime.streams.ZLinkStreamRuntime;
import systems.zlink.framework.spots.ZLinkSpotOutbound;
import systems.zlink.framework.spots.ZLinkSpotPublisherClient;
import systems.zlink.framework.spots.ZLinkSpotManager;
import systems.zlink.framework.locations.ZLinkLocationRuntimeQuery;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.channels.ZLinkRouteMeshRuntimeOptions;
import systems.zlink.contracts.core.RoutingId;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.TimeUnit;

public final class ZLinkFrameworkRuntime
    implements AutoCloseable,
        systems.zlink.framework.configuration.ZLinkMessageFlowControl {
    public static final java.time.Duration DEFAULT_TERMINATION_DEADLINE =
        java.time.Duration.ofSeconds(30);
    private final ZLinkChannelRuntime channels;
    private final ZLinkMeshNodesRuntime meshNodes;
    private final ZLinkSpotRuntime spots;
    private final ZLinkActorRuntime actors;
    private final systems.zlink.framework.runtime.spots
        .ZLinkUserSpotRetireRuntime spotRetire;
    private final ZLinkActorDirectory actorDirectory;
    private final ZLinkActorClient actorClient;
    private final ZLinkStreamRuntime streams;
    private final ZLinkBackendContext backendContext;
    private final ZLinkFrameworkRegistration registration;
    private final ZLinkRegisteredLocationStores locationStores;
    private final systems.zlink.framework.runtime.internal.drain.ZLinkMeshDrainCoordinator
        meshDrains;
    private final ZLinkLocationRuntime locationRuntime;
    private final ZLinkLocationRuntimeQuery locationRuntimeQuery;
    private final ZLinkLocationLifecycle locationLifecycle;
    private final ZLinkLocationAutoConnectHost locationAutoConnectHost;
    private final ZLinkStatefulAuthorityRouteRuntime
        authorityRouteRuntime;
    private final systems.zlink.framework.runtime.internal.locations
        .ZLinkObjectServerDescriptorPublisher objectDescriptors;
    private volatile ZLinkRouteMeshRuntimeOptions routeMeshRuntimeOptions;
    private final systems.zlink.framework.runtime.internal.spots.SpotTransportAddressResolver
        spotTransportAddressResolver;
    private final ZLinkStoreLocationResolvers storeLocationResolvers;
    private final java.util.concurrent.atomic.AtomicBoolean spotRuntimeStopped =
        new java.util.concurrent.atomic.AtomicBoolean(false);
    private final java.util.concurrent.atomic.AtomicBoolean ready =
        new java.util.concurrent.atomic.AtomicBoolean(false);
    private final java.util.concurrent.CompletableFuture<Void> startupReady =
        new java.util.concurrent.CompletableFuture<>();
    private final java.util.concurrent.atomic.AtomicReference<
        ZLinkFrameworkRuntimeState> runtimeState =
        new java.util.concurrent.atomic.AtomicReference<>(
            ZLinkFrameworkRuntimeState.PREPARING);
    private final java.util.concurrent.atomic.AtomicReference<
        ZLinkTerminationIntent> effectiveTerminationIntent =
        new java.util.concurrent.atomic.AtomicReference<>();
    private final java.util.concurrent.atomic.AtomicReference<
        ZLinkTerminationReason> terminationBlocker =
        new java.util.concurrent.atomic.AtomicReference<>();
    private final java.util.concurrent.atomic.AtomicReference<
        ZLinkTerminationResult> terminalTermination =
        new java.util.concurrent.atomic.AtomicReference<>();
    private final java.util.concurrent.atomic.AtomicReference<
        java.util.concurrent.CompletableFuture<ZLinkTerminationResult>>
        activeTermination =
        new java.util.concurrent.atomic.AtomicReference<>();
    private final java.util.concurrent.atomic.AtomicReference<RelocationOperation>
        activeRelocation =
        new java.util.concurrent.atomic.AtomicReference<>();
    private final java.util.concurrent.atomic.AtomicReference<
        java.time.Instant> terminationDeadline =
        new java.util.concurrent.atomic.AtomicReference<>();
    private final java.util.concurrent.atomic.AtomicLong terminationSequence =
        new java.util.concurrent.atomic.AtomicLong();
    private final java.util.concurrent.atomic.AtomicReference<
        ZLinkFrameworkRelocationResult> lastRelocationResult =
        new java.util.concurrent.atomic.AtomicReference<>();
    private final java.util.concurrent.atomic.AtomicReference<
        ZLinkFrameworkTerminationResult> lastTerminationResult =
        new java.util.concurrent.atomic.AtomicReference<>();
    private final java.util.concurrent.atomic.AtomicBoolean drainStarted =
        new java.util.concurrent.atomic.AtomicBoolean(false);
    private final ZLinkRelocationShutdownGate relocationShutdown =
        new ZLinkRelocationShutdownGate();
    private final java.util.concurrent.atomic.AtomicBoolean drainTerminalStarted =
        new java.util.concurrent.atomic.AtomicBoolean(false);
    private final ZLinkCloseGate closeGate = new ZLinkCloseGate();
    private final java.util.concurrent.CompletableFuture<
        InternalDrainResult> drained =
        new java.util.concurrent.CompletableFuture<>();
    // Shared, runtime-mutable message-flow mode cell, installed into the diagnostics
    // options so every surface observes setMessageFlowMode live.
    private final java.util.concurrent.atomic.AtomicReference<
        systems.zlink.framework.configuration.ZLinkMessageFlowLogMode> messageFlowMode;
    private final ZLinkRuntimeEventDispatcher eventDispatcher;
    private final systems.zlink.framework.runtime.internal.monitoring
        .ZLinkStatusPublisher<systems.zlink.framework.monitoring
            .ZLinkFrameworkRuntimeStatus> runtimeStatusPublisher =
        systems.zlink.framework.runtime.internal.monitoring
            .ZLinkStatusPublisher.create(
                () -> runtimeStatus(terminationSequence.get()),
                status -> java.util.List.of(
                    status.state(),
                    status.isReady(),
                    status.acceptingWork(),
                    status.deadline(),
                    status.relocationResult(),
                    status.terminationResult(),
                    status.inboundDispatch()),
                java.util.concurrent.Flow.defaultBufferSize(),
                status -> status.state() == ZLinkFrameworkRuntimeState.STOPPED
                    || status.state() == ZLinkFrameworkRuntimeState.ERROR,
                status -> status.state()
                    == ZLinkFrameworkRuntimeState.RELOCATED);
    private final ZLinkRouteMeshRuntimeView
        routeMeshRuntime = new ZLinkRouteMeshRuntimeView(this);

    ZLinkFrameworkRuntime(
        DefaultZLinkFrameworkOptions options,
        ZLinkBackendAdapterProvider backendFactory,
        ZLinkMessageSerializer serializer) {
        this(options, backendFactory, serializer, ZLinkHandlerActivator.reflection());
    }

    ZLinkFrameworkRuntime(
        DefaultZLinkFrameworkOptions options,
        ZLinkBackendAdapterProvider backendFactory,
        ZLinkMessageSerializer serializer,
        ZLinkHandlerActivator handlerFactory) {
        this(options, backendFactory, serializer, handlerFactory, null);
    }

    ZLinkFrameworkRuntime(
        DefaultZLinkFrameworkOptions options,
        ZLinkBackendAdapterProvider backendFactory,
        ZLinkMessageSerializer serializer,
        ZLinkHandlerActivator handlerFactory,
        ZLinkRuntimeEventDispatcher eventDispatcher) {
        options.validate();
        this.eventDispatcher = eventDispatcher;
        this.registration = options.registration();
        this.meshDrains = new systems.zlink.framework.runtime.internal.drain
            .ZLinkMeshDrainCoordinator(this.registration.meshNodes().stream()
                .map(systems.zlink.framework.runtime.mesh.MeshNodeRegistration::meshName)
                .toList());
        var diagnostics = this.registration.dispatchOptions().diagnostics();
        this.messageFlowMode =
            new java.util.concurrent.atomic.AtomicReference<>(diagnostics.messageFlow());
        diagnostics.installLiveMode(this.messageFlowMode);
        ZLinkBackendAdapterOptions adapterOptions =
            new ZLinkBackendAdapterOptions(options.defaultRequestTimeout());
        ZLinkStreamCodec defaultStreamCodec = defaultStreamCodec(options);
        ZLinkHandlerActivator.MutableServices runtimeHandlers =
            ZLinkHandlerActivator.services(handlerFactory);
        runtimeHandlers.add(ZLinkFrameworkRegistration.class, this.registration);
        runtimeHandlers.add(ZLinkFrameworkRuntime.class, this);
        var relocationAdapters = new systems.zlink.framework.runtime.internal.relocation
            .ZLinkRelocationAdapterRegistry(this.registration, handlerFactory);
        if (relocationAdapters.hasAdapters()) {
            runtimeHandlers.add(
                systems.zlink.framework.runtime.internal.relocation
                    .ZLinkRelocationAdapterRegistry.class,
                relocationAdapters);
        }
        ZLinkFrameworkLocationSubsystem locationSubsystem =
            ZLinkFrameworkLocationSubsystem.create(this.registration, runtimeHandlers);
        if (this.registration.relocationStore() != null) {
            runtimeHandlers.add(
                systems.zlink.framework.runtime.internal.locations
                    .ZLinkRelocationStore.class,
                this.registration.relocationStore());
        }
        this.locationStores = locationSubsystem.locationStores();
        if (this.locationStores != null
            && this.locationStores.authorityStore() != null
            && this.registration.relocationStore() != null) {
            runtimeHandlers.add(
                systems.zlink.framework.runtime.internal.locations
                    .ZLinkRelocationPublicationCoordinator.class,
                new systems.zlink.framework.runtime.internal.locations
                    .ZLinkRelocationPublicationCoordinator(
                        this.locationStores.authorityStore(),
                        this.registration.relocationStore()));
        }
        if (locationSubsystem.enabled()) {
            this.locationRuntime = locationSubsystem.locationRuntime();
            this.locationRuntimeQuery = locationSubsystem.locationRuntimeQuery();
            this.locationLifecycle = locationSubsystem.locationLifecycle();
            this.locationAutoConnectHost = locationSubsystem.locationAutoConnectHost();
            this.spotTransportAddressResolver = locationSubsystem.spotTransportAddressResolver();
            this.storeLocationResolvers = locationSubsystem.storeLocationResolvers();
        } else {
            this.locationRuntime = null;
            this.locationRuntimeQuery = null;
            this.locationLifecycle = null;
            this.locationAutoConnectHost = null;
            this.spotTransportAddressResolver = null;
            this.storeLocationResolvers = null;
        }
        ZLinkFrameworkChannelSubsystem channelSubsystem = ZLinkFrameworkChannelSubsystem.create(
            options,
            backendFactory,
            adapterOptions,
            serializer,
            runtimeHandlers,
            eventDispatcher);
        this.backendContext = channelSubsystem.backendContext();
        this.channels = channelSubsystem.channels();
        this.channels.setHostStateSupplier(runtimeState::get);
        if (this.registration.meshNodes().isEmpty()) {
            this.meshNodes = ZLinkMeshNodesRuntime.empty();
        } else {
            systems.zlink.framework.runtime.internal.backend.ZLinkMeshBackendAdapter meshAdapter =
                backendFactory.createMeshAdapter(adapterOptions);
            this.meshNodes = ZLinkMeshNodesRuntime.start(
                this.registration.meshNodes(),
                meshAdapter,
                this.backendContext,
                mesh -> new systems.zlink.framework.runtime.channels.ZLinkMeshApplicationDispatcher(
                    mesh,
                    serializer,
                    this.registration,
                    handlerFactory,
                    this.meshDrains,
                    this.registration.inboundDispatchBudget()));
        }
        this.meshNodes.nodesByName().forEach((meshName, node) ->
            this.channels.registerSpotRouterNode(meshName, node.spotNode()));
        if (this.locationStores != null
            && this.locationStores.unifiedStore()
                instanceof systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository store) {
            var peerAuthorityResolver =
                new systems.zlink.framework.runtime.locations
                    .ZLinkMeshPeerAuthorityResolver(
                        store,
                        this.registration.locations().options()
                            .pollingInterval());
            this.meshNodes.nodesByName().values().forEach(
                node -> node.setPeerAuthorityResolver(
                    peerAuthorityResolver));
        }
        this.objectDescriptors =
            this.locationRuntime != null
                && this.locationStores.unifiedStore()
                    instanceof systems.zlink.framework.runtime.internal.locations
                        .ZLinkLocationRepository store
                ? new systems.zlink.framework.runtime.internal.locations
                    .ZLinkObjectServerDescriptorPublisher(
                        store,
                        this.locationRuntime,
                        this.registration,
                        this.meshNodes.nodesByName())
                : null;
        this.routeMeshRuntimeOptions =
            new systems.zlink.framework.runtime.channels
                .ZLinkRouteMeshRuntimeOptionsRuntime(
                    this.meshNodes.nodesByName(),
                    () -> {
                        if (this.objectDescriptors != null) {
                            this.objectDescriptors
                                .publish(this.runtimeState.get())
                                .toCompletableFuture()
                                .join();
                        }
                    });
        runtimeHandlers.add(
            ZLinkRouteMeshRuntimeOptions.class,
            this.routeMeshRuntimeOptions);

        ZLinkFrameworkSpotSubsystem spotSubsystem = ZLinkFrameworkSpotSubsystem.create(
            options,
            backendFactory,
            adapterOptions,
            serializer,
            runtimeHandlers,
            eventDispatcher,
            this.channels,
            this.backendContext,
            this.locationLifecycle,
            this.locationStores == null
                ? null : this.locationStores.authorityStore(),
            this.locationStores != null
                && this.locationStores.unifiedStore()
                    instanceof systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository
                        store
                ? store
                : null,
            this.locationRuntime,
            this.spotTransportAddressResolver,
            this.meshNodes.nodesByName());
        this.spots = spotSubsystem.spots();

        this.authorityRouteRuntime =
            this.locationStores != null
                && this.locationStores.authorityStore() != null
                && !this.meshNodes.nodesByName().isEmpty()
            ? new ZLinkStatefulAuthorityRouteRuntime(
                this.locationStores.authorityStore(),
                this.meshNodes.nodesByName(),
                this.registration.locations().options()
                    .pollingInterval(),
                failure -> java.util.logging.Logger.getLogger(
                    ZLinkStatefulAuthorityRouteRuntime.class
                        .getName())
                    .warning(
                        "Durable authority route reconcile failed: "
                            + failure.getMessage()))
            : null;

        ZLinkFrameworkActorSubsystem actorSubsystem = ZLinkFrameworkActorSubsystem.create(
            backendFactory,
            this.registration,
            serializer,
            runtimeHandlers,
            handlerFactory,
            eventDispatcher,
            defaultStreamCodec,
            this.channels,
            this.spots,
            this.locationLifecycle,
            this.storeLocationResolvers,
            spotSubsystem.remoteAddressResolver(),
            this.locationStores != null
                && this.locationStores.unifiedStore()
                    instanceof systems.zlink.framework.runtime.internal.locations
                        .ZLinkLocationRepository store
                ? store
                : null,
            this.meshNodes.nodesByName(),
            this.startupReady);
        this.actors = actorSubsystem.actors();
        this.actorDirectory = actorSubsystem.actorDirectory();
        this.actorClient = actorSubsystem.actorClient();

        ZLinkFrameworkStreamSubsystem streamSubsystem = ZLinkFrameworkStreamSubsystem.create(
            options,
            backendFactory,
            adapterOptions,
            serializer,
            runtimeHandlers,
            eventDispatcher,
            this.meshNodes,
            this.backendContext,
            this.spots,
            this.actors);
        this.streams = streamSubsystem.streams();
        if (this.streams != null) {
            this.meshNodes.nodesByName().values().forEach(node ->
                node.setSessionRelocationRouteHandler(
                    this.streams::handleSessionRelocationRoute));
        }
        this.spotRetire = this.spots != null
            && this.actors != null
            && this.locationStores != null
            && this.locationStores.authorityStore() != null
            && this.locationStores.unifiedStore()
                instanceof systems.zlink.framework.runtime.internal.locations
                    .ZLinkLocationRepository locationStore
            && this.registration.relocationStore() != null
            ? new systems.zlink.framework.runtime.spots
                .ZLinkUserSpotRetireRuntime(
                    this.spots,
                    this.actors,
                    this.registration.meshNodes(),
                    this.meshNodes.nodesByName(),
                    locationStore,
                    this.locationStores.authorityStore(),
                    this.registration.relocationStore(),
                    this.registration.locations().options(),
                    relocationAdapters,
                    this.registration.applicationVersion(),
                    this.registration.maintenanceWave())
            : null;

        locationSubsystem.startup()
            .thenCompose(ignored ->
                this.objectDescriptors == null
                    ? java.util.concurrent.CompletableFuture
                        .completedFuture(null)
                    : this.objectDescriptors.publish(
                        this.spotRetire != null
                            && this.spotRetire.requiresStartupRecovery()
                            ? ZLinkFrameworkRuntimeState.PREPARING
                            : ZLinkFrameworkRuntimeState.SERVING))
            .thenCompose(ignored ->
                this.locationStores == null
                    || this.locationStores.unifiedStore() == null
                    ? java.util.concurrent.CompletableFuture
                        .completedFuture(null)
                    : java.util.concurrent.CompletableFuture.allOf(
                        this.meshNodes.nodesByName().values().stream()
                            .map(node -> node
                                .refreshLocalAuthorityFence()
                                .toCompletableFuture())
                            .toArray(
                                java.util.concurrent.CompletableFuture[]::new)))
            .thenCompose(ignored -> connectManualObjectPeers())
            .thenCompose(ignored -> spotSubsystem.startup())
            .thenCompose(ignored ->
                this.spotRetire == null
                    ? java.util.concurrent.CompletableFuture
                        .completedFuture(null)
                    : this.spotRetire.startup())
            .thenCompose(ignored ->
                this.objectDescriptors == null
                    || this.spotRetire == null
                    || !this.spotRetire.requiresStartupRecovery()
                    ? java.util.concurrent.CompletableFuture
                        .completedFuture(null)
                    : this.objectDescriptors.publish(
                        ZLinkFrameworkRuntimeState.SERVING))
            .thenCompose(ignored ->
                this.authorityRouteRuntime == null
                    ? java.util.concurrent.CompletableFuture
                        .completedFuture(null)
                    : this.authorityRouteRuntime.start())
            .thenCompose(ignored -> ZLinkFrameworkAutoConnectSubsystem.start(
                this.locationAutoConnectHost,
                this.registration,
                this.channels,
                this.meshNodes,
                this.spots))
            .whenComplete((ignored, failure) -> {
                if (failure == null && !drainStarted.get()) {
                    ready.set(true);
                    startupReady.complete(null);
                    publishRuntimeState(ZLinkFrameworkRuntimeState.SERVING);
                    java.util.logging.Logger.getLogger(
                        ZLinkFrameworkRuntime.class.getName())
                        .info("ZLINK_FRAMEWORK_READY");
                } else if (failure != null) {
                    startupReady.completeExceptionally(
                        unwrapCompletionFailure(failure));
                    publishRuntimeState(ZLinkFrameworkRuntimeState.ERROR);
                    java.util.logging.Logger.getLogger(
                        ZLinkFrameworkRuntime.class.getName())
                        .warning(
                        "Framework startup failed: "
                            + failure.getMessage());
                } else {
                    startupReady.completeExceptionally(
                        new IllegalStateException(
                            "Framework startup was interrupted by shutdown"));
                }
            });
    }

    private java.util.concurrent.CompletionStage<Void>
        connectManualObjectPeers() {
        if (locationStores == null
            || !(locationStores.unifiedStore()
                instanceof systems.zlink.framework.runtime.internal.locations
                    .ZLinkLocationRepository store)) {
            return java.util.concurrent.CompletableFuture.completedFuture(null);
        }
        var tasks = new java.util.ArrayList<
            java.util.concurrent.CompletableFuture<?>>();
        for (var registration : this.registration.meshNodes()) {
            var source = meshNodes.nodesByName().get(
                registration.meshName());
            if (source == null || !registration.objectRoleEnabled()) {
                continue;
            }
            var unresolved = registration.peers().stream()
                .filter(peer -> peer.expectedRoutingId() == null)
                .toList();
            if (unresolved.isEmpty()) {
                continue;
            }
            tasks.add(store.listMeshNodes(
                    registration.meshName(),
                    new systems.zlink.framework.locations
                        .ZLinkPageRequest(1000, null))
                .thenAccept(page -> {
                    for (var peer : unresolved) {
                        page.items().stream()
                            .filter(target -> target.endpoint().equals(
                                peer.endpoint()))
                            .filter(target -> !target.rid().equals(
                                source.status().routingId()))
                            .findFirst()
                            .ifPresent(target -> source.connectPeer(
                                target.endpoint(), target.rid()));
                    }
                }).toCompletableFuture());
        }
        return java.util.concurrent.CompletableFuture.allOf(
            tasks.toArray(java.util.concurrent.CompletableFuture[]::new));
    }

    static ZLinkFrameworkRuntime start(
        DefaultZLinkFrameworkOptions options,
        ZLinkBackendAdapterProvider backendFactory) {
        return new ZLinkFrameworkRuntime(options, backendFactory, serializerFor(options));
    }

    static ZLinkFrameworkRuntime start(
        DefaultZLinkFrameworkOptions options,
        ZLinkBackendAdapterProvider backendFactory,
        ZLinkHandlerActivator handlerFactory) {
        return start(options, backendFactory, handlerFactory, null);
    }

    static ZLinkFrameworkRuntime start(
        DefaultZLinkFrameworkOptions options,
        ZLinkBackendAdapterProvider backendFactory,
        ZLinkHandlerActivator handlerFactory,
        ZLinkRuntimeEventDispatcher eventDispatcher) {
        return new ZLinkFrameworkRuntime(
            options,
            backendFactory,
            serializerFor(options),
            handlerFactory,
            eventDispatcher);
    }

    static ZLinkMessageSerializer serializerFor(DefaultZLinkFrameworkOptions options) {
        java.util.Optional<ZLinkMessageSerializer> custom =
            options.registration().codecs().customSerializer();
        if (custom.isPresent()) {
            return custom.get();
        }
        return options.registration().codecs().serializerWithFallback(new ZLinkJsonMessageSerializer());
    }

    private static ZLinkStreamCodec defaultStreamCodec(DefaultZLinkFrameworkOptions options) {
        return options.registration().codecs().streamCodecForCustomSerializer()
            .orElse(ZLinkStreamCodec.JSON);
    }

    public ZLinkClient client() {
        return channels;
    }

    public ZLinkChannelRuntimeOptions channelRuntimeOptions() {
        return channels;
    }

    // Runtime toggle (ZLinkMessageFlowControl): flip the shared live-mode cell so every
    // surface starts/stops tracing without a restart. Thread-safe.
    @Override
    public void setMessageFlowMode(systems.zlink.framework.configuration.ZLinkMessageFlowLogMode mode) {
        if (mode == null) {
            throw new IllegalArgumentException("mode is required");
        }
        messageFlowMode.set(mode);
    }

    @Override
    public systems.zlink.framework.configuration.ZLinkMessageFlowLogMode messageFlowMode() {
        return registration.dispatchOptions().diagnostics().effectiveMessageFlow();
    }

    public ZLinkFanoutClient fanout() {
        return channels;
    }

    public ZLinkRouteClient route() {
        return channels;
    }

    public systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime
        routeMeshRuntime() {
        return routeMeshRuntime;
    }

    ZLinkRouteMeshRuntimeOptions routeMeshRuntimeOptionsInternal() {
        ZLinkRouteMeshRuntimeOptions options = routeMeshRuntimeOptions;
        if (options == null) {
            throw new ZLinkConfigurationException(
                "RouteMesh runtime options are not configured");
        }
        return options;
    }

    public systems.zlink.framework.monitoring.ZLinkClientServerRuntime
        clientServerRuntime() {
        return channels.clientServerRuntime();
    }

    public systems.zlink.framework.monitoring.ZLinkFanoutRuntime
        fanoutRuntime() {
        return channels.fanoutRuntime();
    }

    public ZLinkSpotManager spotManager() {
        if (spots == null) {
            throw new ZLinkConfigurationException("Spot runtime is not configured");
        }
        return spots;
    }

    public ZLinkSpotOutbound spotOutbound() {
        if (spots == null) {
            throw new ZLinkConfigurationException("Spot runtime is not configured");
        }
        return spots.outbound();
    }

    public ZLinkSpotPublisherClient spotPublisherClient() {
        if (spots == null) {
            throw new ZLinkConfigurationException("Spot runtime is not configured");
        }
        return spots.publisherClient();
    }

    ZLinkInternalMeshNode monitoringMeshNode(String meshName) {
        return meshNodes.nodesByName().get(meshName);
    }

    public java.util.Map<String, ZLinkInternalMeshNode>
        meshNodesForInternalMonitoring() {
        return meshNodes.nodesByName();
    }

    public ZLinkLocationRuntimeQuery monitoringLocationRuntimeQuery() {
        if (locationRuntimeQuery == null) {
            throw new ZLinkConfigurationException("Location runtime is not configured");
        }
        return locationRuntimeQuery;
    }

    public systems.zlink.framework.runtime.internal.monitoring
        .ZLinkMeshNodeMonitoringProjection monitoringMeshNodeProjection(
            String meshName,
            RoutingId rid) {
        var configured = registration.meshNodes().stream()
            .filter(candidate -> candidate.meshName().equals(meshName))
            .findFirst()
            .orElseThrow(() -> new ZLinkConfigurationException(
                "RouteMesh is not configured: " + meshName));
        ZLinkInternalMeshNode node = meshNodes.nodesByName().get(meshName);
        if (node == null) {
            throw new ZLinkConfigurationException(
                "RouteMesh is not configured: " + meshName);
        }
        if (locationStores != null) {
            var store = locationStores.unifiedStore();
            try {
                String continuation = null;
                do {
                    var page = store.listMeshNodes(
                            meshName,
                            new systems.zlink.framework.locations.ZLinkPageRequest(
                                128,
                                continuation))
                        .toCompletableFuture()
                        .join();
                    for (var descriptor : page.items()) {
                        if (descriptor.rid().equals(rid)) {
                            return systems.zlink.framework.runtime.internal.monitoring
                                .ZLinkMeshNodeMonitoringProjection.fromDescriptor(descriptor);
                        }
                    }
                    continuation = page.continuationToken();
                } while (continuation != null);
            } catch (RuntimeException ignored) {
                // Monitoring keeps the configured limits available while the
                // descriptor store is unavailable.
            }
        }
        return systems.zlink.framework.runtime.internal.monitoring
            .ZLinkMeshNodeMonitoringProjection.fromRegistration(
                configured,
                node.status().descriptorRevision(),
                node.placementWeight());
    }

    public java.util.List<String> monitoringMeshNodeChannelNames(String meshName) {
        return registration.meshNodes().stream()
            .filter(candidate -> candidate.meshName().equals(meshName))
            .findFirst()
            .map(systems.zlink.framework.runtime.mesh.MeshNodeRegistration::channelNames)
            .orElseThrow(() -> new ZLinkConfigurationException(
                "RouteMesh is not configured: " + meshName));
    }

    public systems.zlink.framework.locations.ZLinkLocationReadiness locationReadiness() {
        if (locationRuntimeQuery == null) {
            throw new ZLinkConfigurationException("Location runtime is not configured");
        }
        return new ZLinkLocationReadinessService(locationRuntimeQuery);
    }

    public boolean stopSpotRuntime() {
        if (spots == null || !spotRuntimeStopped.compareAndSet(false, true)) {
            return false;
        }
        spots.beginClose();
        spots.closeAsync();
        return true;
    }

    public ZLinkActorManager actorManager() {
        if (actors == null) {
            throw new ZLinkConfigurationException("Actor runtime is not configured");
        }
        return actors;
    }

    public systems.zlink.framework.actors.ZLinkActorDirectory actorDirectory() {
        if (actorDirectory == null) {
            throw new ZLinkConfigurationException("Actor directory requires a SPOT node and location store");
        }
        return actorDirectory;
    }

    public ZLinkActorClient actorClient() {
        if (actorClient == null) {
            throw new ZLinkConfigurationException("Actor client requires a SPOT node and location store");
        }
        return actorClient;
    }

    public systems.zlink.framework.spots.SpotHandleResolver spotHandleResolver() {
        if (spotTransportAddressResolver
            instanceof systems.zlink.framework.spots.SpotHandleResolver resolver) {
            return resolver;
        }
        throw new systems.zlink.framework.errors.ZLinkConfigurationException(
            "SpotHandleResolver requires a configured location store");
    }

    public systems.zlink.framework.spots.ActorSpotHandleResolver actorSpotHandleResolver() {
        if (spotTransportAddressResolver
            instanceof systems.zlink.framework.spots.ActorSpotHandleResolver resolver) {
            return resolver;
        }
        throw new systems.zlink.framework.errors.ZLinkConfigurationException(
            "ActorSpotHandleResolver requires a configured location store");
    }

    public ZLinkSessionActorsRuntime sessionActors(String streamNodeName, RoutingId sessionRid) {
        if (streams == null) {
            throw new ZLinkConfigurationException("Stream runtime is not configured");
        }
        return streams.sessionActors(streamNodeName, sessionRid, actors);
    }

    ZLinkFrameworkRuntimeState state() {
        return runtimeState.get();
    }

    public systems.zlink.framework.monitoring.ZLinkFrameworkRuntimeStatus
        status() {
        return runtimeStatus(terminationSequence.get());
    }

    public java.util.concurrent.Flow.Publisher<
        systems.zlink.framework.monitoring.ZLinkObservedStatus<
            systems.zlink.framework.monitoring.ZLinkFrameworkRuntimeStatus>>
        observe() {
        return runtimeStatusPublisher;
    }

    public java.util.concurrent.CompletionStage<ZLinkFrameworkRelocationResult>
        relocate(ZLinkFrameworkRelocationOptions options) {
        java.util.Objects.requireNonNull(options, "options");
        long sourceVersion = registration.applicationVersion();
        long effectiveTargetVersion;
        if (options.mode() == ZLinkFrameworkRelocationMode.PLANNED_MAINTENANCE) {
            if (options.targetApplicationVersion() != null) {
                throw new IllegalArgumentException(
                    "planned maintenance does not accept a target application version");
            }
            effectiveTargetVersion = sourceVersion;
        } else {
            if (options.targetApplicationVersion() == null
                || options.targetApplicationVersion() <= sourceVersion) {
                throw new IllegalArgumentException(
                    "rolling update requires a target application version greater than the source version");
            }
            effectiveTargetVersion = options.targetApplicationVersion();
        }
        java.time.Duration deadline = options.deadline() == null
            ? DEFAULT_TERMINATION_DEADLINE
            : options.deadline();
        if (runtimeState.get() == ZLinkFrameworkRuntimeState.RELOCATED
            && lastRelocationResult.get() != null) {
            return java.util.concurrent.CompletableFuture.completedFuture(
                lastRelocationResult.get());
        }
        RelocationOperation current = activeRelocation.get();
        if (current != null) {
            return current.matches(options.mode(), effectiveTargetVersion)
                ? independentRelocationWaiter(current.completion())
                : java.util.concurrent.CompletableFuture.completedFuture(
                    blockedRelocation(
                        options.mode(),
                        effectiveTargetVersion,
                        ZLinkFrameworkRelocationReason.OPERATION_IN_PROGRESS));
        }
        var completion = new java.util.concurrent.CompletableFuture<
            ZLinkFrameworkRelocationResult>();
        var candidate = new RelocationOperation(
            options.mode(), effectiveTargetVersion, completion);
        if (!activeRelocation.compareAndSet(null, candidate)) {
            return relocate(options);
        }
        if (runtimeState.get() != ZLinkFrameworkRuntimeState.SERVING
            || activeTermination.get() != null) {
            ZLinkFrameworkRuntimeState currentState = runtimeState.get();
            completeRelocation(
                candidate,
                blockedRelocation(
                    options.mode(),
                    effectiveTargetVersion,
                    activeTermination.get() == null
                        ? ZLinkFrameworkRelocationReason.RUNTIME_NOT_READY
                        : ZLinkFrameworkRelocationReason.SHUTDOWN_REQUESTED),
                currentState);
            return independentRelocationWaiter(completion);
        }
        terminationDeadline.set(java.time.Instant.now().plus(deadline));
        java.time.Instant relocationDeadline = terminationDeadline.get();
        withinRelocationDeadline(
            relocationPreflightUntilDeadline(relocationDeadline),
            relocationDeadline)
            .whenComplete((reason, failure) -> {
            Throwable preflightFailure = unwrapCompletionFailure(failure);
            ZLinkFrameworkRelocationReason blocker = activeTermination.get() != null
                ? ZLinkFrameworkRelocationReason.SHUTDOWN_REQUESTED
                : failure == null
                    ? toPublicRelocationReason(reason)
                    : preflightFailure instanceof java.util.concurrent.TimeoutException
                        || preflightFailure instanceof java.util.concurrent.CancellationException
                        ? ZLinkFrameworkRelocationReason.DEADLINE_EXCEEDED
                        : ZLinkFrameworkRelocationReason.STORE_UNAVAILABLE;
            if (blocker != ZLinkFrameworkRelocationReason.NONE) {
                completeRelocation(
                    candidate,
                    blockedRelocation(
                        options.mode(), effectiveTargetVersion, blocker),
                    ZLinkFrameworkRuntimeState.SERVING);
                return;
            }
            java.util.concurrent.atomic.AtomicBoolean relocationPublished =
                new java.util.concurrent.atomic.AtomicBoolean(false);
            java.util.concurrent.CompletionStage<Void> relocation =
                relocateWithTargetWait(
                    relocationDeadline,
                    options.mode(),
                    effectiveTargetVersion,
                    relocationPublished);
            relocation.whenComplete((ignored, relocationFailure) -> {
                Throwable relocationCause =
                    unwrapCompletionFailure(relocationFailure);
                ZLinkFrameworkRelocationReason failureReason =
                    relocationFailureReason(
                        relocationCause, relocationPublished.get());
                ZLinkFrameworkRelocationResult result = relocationFailure == null
                    && activeTermination.get() == null
                    ? new ZLinkFrameworkRelocationResult(
                        options.mode(),
                        effectiveTargetVersion,
                        ZLinkFrameworkRelocationOutcome.RELOCATED,
                        ZLinkFrameworkRelocationReason.NONE)
                    : blockedRelocation(
                        options.mode(),
                        effectiveTargetVersion,
                        failureReason);
                completeRelocation(
                    candidate,
                    result,
                    result.outcome()
                        == ZLinkFrameworkRelocationOutcome.RELOCATED
                        ? ZLinkFrameworkRuntimeState.RELOCATED
                        : ZLinkFrameworkRuntimeState.SERVING);
            });
        });
        return independentRelocationWaiter(completion);
    }

    private java.util.concurrent.CompletionStage<ZLinkTerminationReason>
        relocationPreflightUntilDeadline(java.time.Instant deadline) {
        if (activeTermination.get() != null) {
            return java.util.concurrent.CompletableFuture.completedFuture(
                ZLinkTerminationReason.RUNTIME_NOT_READY);
        }
        return retirePreflight().thenCompose(reason -> {
            if (reason != ZLinkTerminationReason.TARGET_UNAVAILABLE
                || !java.time.Instant.now().isBefore(deadline)) {
                return java.util.concurrent.CompletableFuture.completedFuture(reason);
            }
            return java.util.concurrent.CompletableFuture.runAsync(
                    () -> { },
                    java.util.concurrent.CompletableFuture.delayedExecutor(
                        25, java.util.concurrent.TimeUnit.MILLISECONDS))
                .thenCompose(ignored -> relocationPreflightUntilDeadline(deadline));
        });
    }

    private static <T> java.util.concurrent.CompletionStage<T>
        withinRelocationDeadline(
            java.util.concurrent.CompletionStage<T> stage,
            java.time.Instant deadline) {
        long remaining = java.time.Duration.between(
            java.time.Instant.now(), deadline).toMillis();
        if (remaining <= 0) {
            return java.util.concurrent.CompletableFuture.failedFuture(
                new java.util.concurrent.TimeoutException(
                    "Relocation preflight deadline exceeded"));
        }
        return stage.toCompletableFuture().orTimeout(
            remaining, java.util.concurrent.TimeUnit.MILLISECONDS);
    }

    private java.util.concurrent.CompletionStage<Void> relocateWithTargetWait(
        java.time.Instant deadline,
        ZLinkFrameworkRelocationMode mode,
        long targetVersion,
        java.util.concurrent.atomic.AtomicBoolean relocationPublished) {
        if (activeTermination.get() != null) {
            return java.util.concurrent.CompletableFuture.failedFuture(
                new systems.zlink.framework.runtime.spots
                    .ZLinkUserSpotRetireRuntime.RelocationBlockedException(
                        ZLinkFrameworkRelocationReason.SHUTDOWN_REQUESTED,
                        "Shutdown was requested before relocation admission"));
        }
        if (!java.time.Instant.now().isBefore(deadline)) {
            return java.util.concurrent.CompletableFuture.failedFuture(
                new systems.zlink.framework.runtime.spots
                    .ZLinkUserSpotRetireRuntime.RelocationBlockedException(
                        ZLinkFrameworkRelocationReason.TARGET_UNAVAILABLE,
                        "No eligible relocation target became Ready before the deadline"));
        }
        java.util.concurrent.CompletionStage<Void> attempt =
            validateRelocationTargetPopulation(mode, targetVersion, deadline)
            .thenCompose(validated -> spotRetire == null
                ? beginRelocationAdmission()
                    .thenRun(() -> relocationPublished.set(true))
                : spotRetire.relocateAll(
                    deadline,
                    () -> activeTermination.get() != null,
                    mode,
                    targetVersion,
                    () -> beginRelocationAdmission()
                        .thenRun(() -> relocationPublished.set(true))));
        return withinRelocationDeadline(attempt, deadline)
            .exceptionallyCompose(failure -> {
            Throwable cause = unwrapCompletionFailure(failure);
            if (!relocationPublished.get()
                && cause instanceof systems.zlink.framework.runtime.spots
                    .ZLinkUserSpotRetireRuntime.RelocationBlockedException blocked
                && blocked.reason()
                    == ZLinkFrameworkRelocationReason.TARGET_UNAVAILABLE
                && java.time.Instant.now().isBefore(deadline)
                && activeTermination.get() == null) {
                return java.util.concurrent.CompletableFuture.runAsync(
                        () -> { },
                        java.util.concurrent.CompletableFuture.delayedExecutor(
                            25, java.util.concurrent.TimeUnit.MILLISECONDS))
                    .thenCompose(ignored -> relocateWithTargetWait(
                        deadline, mode, targetVersion, relocationPublished));
            }
            return java.util.concurrent.CompletableFuture.failedFuture(cause);
            });
    }

    private java.util.concurrent.CompletionStage<Void>
        beginRelocationAdmission() {
        if (spots != null) {
            spots.beginRelocation();
        }
        if (actors != null) {
            actors.beginRelocation();
        }
        return publishRuntimeStateAwaited(
            ZLinkFrameworkRuntimeState.RELOCATING);
    }

    private java.util.concurrent.CompletionStage<Void>
        validateRelocationTargetPopulation(
            ZLinkFrameworkRelocationMode mode,
            long targetVersion,
            java.time.Instant deadline) {
        if (storeLocationResolvers == null) {
            return java.util.concurrent.CompletableFuture.failedFuture(
                new systems.zlink.framework.runtime.spots
                    .ZLinkUserSpotRetireRuntime.RelocationBlockedException(
                        ZLinkFrameworkRelocationReason.STORE_UNAVAILABLE,
                        "Location Store is unavailable"));
        }
        java.util.concurrent.CompletionStage<Void> result =
            java.util.concurrent.CompletableFuture.completedFuture(null);
        for (var registration : this.registration.meshNodes()) {
            result = result.thenCompose(ignored ->
                withinRelocationDeadline(
                    storeLocationResolvers.listMeshNodes(
                        registration.meshName()),
                    deadline).thenAccept(nodes -> {
                        boolean found = nodes.stream().anyMatch(node ->
                            node.state() == ZLinkFrameworkRuntimeState.SERVING
                                && !node.rid().equals(registration.routingId())
                                && node.applicationVersion() == targetVersion
                                && this.registration.maintenanceWave()
                                        .map(source -> node.maintenanceWave()
                                            .map(target -> !target.equals(source))
                                            .orElse(true))
                                        .orElse(true));
                        if (!found) {
                            throw new systems.zlink.framework.runtime.spots
                                .ZLinkUserSpotRetireRuntime.RelocationBlockedException(
                                    ZLinkFrameworkRelocationReason.TARGET_UNAVAILABLE,
                                    "No eligible relocation target is Ready in Mesh: "
                                        + registration.meshName());
                        }
                    }));
        }
        return result;
    }

    private ZLinkFrameworkRelocationReason relocationFailureReason(
        Throwable failure,
        boolean relocationPublished) {
        if (activeTermination.get() != null) {
            return ZLinkFrameworkRelocationReason.SHUTDOWN_REQUESTED;
        }
        if (failure instanceof systems.zlink.framework.runtime.spots
            .ZLinkUserSpotRetireRuntime.RelocationBlockedException blocked) {
            return blocked.reason();
        }
        if (failure instanceof java.util.concurrent.TimeoutException
            || failure instanceof java.util.concurrent.CancellationException) {
            return ZLinkFrameworkRelocationReason.DEADLINE_EXCEEDED;
        }
        return relocationPublished
            ? ZLinkFrameworkRelocationReason.RELOCATION_FAILED
            : ZLinkFrameworkRelocationReason.STORE_UNAVAILABLE;
    }

    private void completeRelocation(
        RelocationOperation operation,
        ZLinkFrameworkRelocationResult result,
        ZLinkFrameworkRuntimeState state) {
        lastRelocationResult.set(result);
        if (state == ZLinkFrameworkRuntimeState.SERVING) {
            if (spots != null) {
                spots.cancelRelocation();
            }
            if (actors != null) {
                actors.cancelRelocation();
            }
        }
        publishRuntimeState(
            result.reason() == ZLinkFrameworkRelocationReason.SHUTDOWN_REQUESTED
                && activeTermination.get() != null
                ? runtimeState.get()
                : state);
        operation.completion().complete(result);
        activeRelocation.compareAndSet(operation, null);
    }

    private record RelocationOperation(
        ZLinkFrameworkRelocationMode mode,
        long effectiveTargetVersion,
        java.util.concurrent.CompletableFuture<ZLinkFrameworkRelocationResult>
            completion) {
        private RelocationOperation {
            java.util.Objects.requireNonNull(mode, "mode");
            java.util.Objects.requireNonNull(completion, "completion");
        }

        boolean matches(
            ZLinkFrameworkRelocationMode requestedMode,
            long requestedTargetVersion) {
            return mode == requestedMode
                && effectiveTargetVersion == requestedTargetVersion;
        }
    }

    private static ZLinkFrameworkRelocationResult blockedRelocation(
        ZLinkFrameworkRelocationMode mode,
        long targetVersion,
        ZLinkFrameworkRelocationReason reason) {
        return new ZLinkFrameworkRelocationResult(
            mode,
            targetVersion,
            ZLinkFrameworkRelocationOutcome.BLOCKED,
            reason);
    }

    private static Throwable unwrapCompletionFailure(Throwable failure) {
        Throwable current = failure;
        while (current instanceof java.util.concurrent.CompletionException
            || current instanceof java.util.concurrent.ExecutionException) {
            if (current.getCause() == null) {
                break;
            }
            current = current.getCause();
        }
        return current;
    }

    private static ZLinkFrameworkRelocationReason toPublicRelocationReason(
        ZLinkTerminationReason reason) {
        return switch (reason) {
            case NONE -> ZLinkFrameworkRelocationReason.NONE;
            case TARGET_UNAVAILABLE -> ZLinkFrameworkRelocationReason.TARGET_UNAVAILABLE;
            case STORE_UNAVAILABLE -> ZLinkFrameworkRelocationReason.STORE_UNAVAILABLE;
            case RELOCATION_DISABLED -> ZLinkFrameworkRelocationReason.RELOCATION_DISABLED;
            case STATE_INCOMPATIBLE -> ZLinkFrameworkRelocationReason.STATE_INCOMPATIBLE;
            case DEADLINE_EXCEEDED -> ZLinkFrameworkRelocationReason.DEADLINE_EXCEEDED;
            case RELOCATION_FAILED, TEARDOWN_FAILED ->
                ZLinkFrameworkRelocationReason.RELOCATION_FAILED;
            case RUNTIME_NOT_READY -> ZLinkFrameworkRelocationReason.RUNTIME_NOT_READY;
            case MANUAL_TOPOLOGY_UNSUPPORTED ->
                ZLinkFrameworkRelocationReason.MANUAL_TOPOLOGY_UNSUPPORTED;
        };
    }

    private static java.util.concurrent.CompletionStage<ZLinkFrameworkRelocationResult>
        independentRelocationWaiter(
            java.util.concurrent.CompletionStage<ZLinkFrameworkRelocationResult> source) {
        var waiter =
            new java.util.concurrent.CompletableFuture<
                ZLinkFrameworkRelocationResult>();
        source.whenComplete((result, failure) -> {
            if (failure == null) {
                waiter.complete(result);
            } else {
                waiter.completeExceptionally(failure);
            }
        });
        return waiter;
    }

    public java.util.concurrent.CompletionStage<ZLinkFrameworkTerminationResult>
        shutdown() {
        return shutdown(DEFAULT_TERMINATION_DEADLINE);
    }

    public java.util.concurrent.CompletionStage<ZLinkFrameworkTerminationResult>
        shutdown(java.time.Duration deadline) {
        return beginTermination(ZLinkTerminationIntent.SHUTDOWN, deadline)
            .thenApply(this::toPublicTerminationResult);
    }

    private java.util.concurrent.CompletionStage<ZLinkTerminationResult>
        beginTermination(
            ZLinkTerminationIntent intent,
            java.time.Duration deadline) {
        java.util.Objects.requireNonNull(intent, "intent");
        java.util.Objects.requireNonNull(deadline, "deadline");
        if (deadline.isZero() || deadline.isNegative()) {
            throw new IllegalArgumentException(
                "termination deadline must be positive");
        }
        java.util.concurrent.CompletableFuture<ZLinkTerminationResult>
            current = activeTermination.get();
        if (current != null) {
            if (intent == ZLinkTerminationIntent.SHUTDOWN
                && !drainStarted.get()
                && effectiveTerminationIntent.compareAndSet(
                    ZLinkTerminationIntent.RETIRE,
                    ZLinkTerminationIntent.SHUTDOWN)) {
                if (relocationShutdown.requestShutdown()) {
                    startTermination(current, deadline);
                }
            }
            return independentWaiter(current);
        }
        var candidate =
            new java.util.concurrent.CompletableFuture<
                ZLinkTerminationResult>();
        if (!activeTermination.compareAndSet(null, candidate)) {
            return beginTermination(intent, deadline);
        }
        terminalTermination.set(null);
        terminationBlocker.set(null);
        terminationDeadline.set(java.time.Instant.now().plus(deadline));
        effectiveTerminationIntent.set(intent);
        publishRuntimeState(runtimeState.get());
        if (intent == ZLinkTerminationIntent.SHUTDOWN) {
            startTermination(candidate, deadline);
            return independentWaiter(candidate);
        }
        retirePreflight().whenComplete((reason, failure) -> {
            if (candidate.isDone()) {
                return;
            }
            if (effectiveTerminationIntent.get()
                == ZLinkTerminationIntent.SHUTDOWN) {
                startTermination(candidate, deadline);
                return;
            }
            ZLinkTerminationReason blocker = failure == null
                ? reason
                : ZLinkTerminationReason.STORE_UNAVAILABLE;
            if (blocker != ZLinkTerminationReason.NONE) {
                terminationBlocker.set(blocker);
                ZLinkTerminationResult blocked =
                    new ZLinkTerminationResult(
                        ZLinkTerminationIntent.RETIRE,
                        ZLinkTerminationOutcome.BLOCKED,
                        blocker);
                terminalTermination.set(blocked);
                publishRuntimeState(ZLinkFrameworkRuntimeState.SERVING);
                candidate.complete(blocked);
                effectiveTerminationIntent.compareAndSet(
                    ZLinkTerminationIntent.RETIRE, null);
                activeTermination.compareAndSet(candidate, null);
                return;
            }
            java.time.Instant retireDeadline = terminationDeadline.get();
            java.util.concurrent.CompletionStage<Void> relocation =
                spotRetire == null
                    ? java.util.concurrent.CompletableFuture
                        .completedFuture(null)
                    : spotRetire.relocateAll(
                        retireDeadline,
                        relocationShutdown::stopBeforeNextUnit,
                        ZLinkFrameworkRelocationMode.PLANNED_MAINTENANCE,
                        registration.applicationVersion(),
                        () -> {
                            if (relocationShutdown.beginRelocationUnit()) {
                                return beginRelocationAdmission();
                            }
                            return java.util.concurrent.CompletableFuture
                                .completedFuture(null);
                        });
            relocation.whenComplete((ignored, relocationFailure) -> {
                relocationShutdown.finishRelocationUnit();
                if (relocationFailure == null) {
                    startTermination(candidate, deadline);
                } else {
                    startTermination(
                        candidate,
                        deadline,
                        ZLinkTerminationReason.RELOCATION_FAILED);
                }
            });
        });
        return independentWaiter(candidate);
    }

    private java.util.concurrent.CompletionStage<Void> beginEmptyRelocation() {
        return java.util.concurrent.CompletableFuture.completedFuture(null);
    }

    private void startTermination(
        java.util.concurrent.CompletableFuture<ZLinkTerminationResult>
            completion,
        java.time.Duration deadline) {
        startTermination(completion, deadline, null);
    }

    private void startTermination(
        java.util.concurrent.CompletableFuture<ZLinkTerminationResult>
            completion,
        java.time.Duration deadline,
        ZLinkTerminationReason forcedReason) {
        drain(deadline).whenComplete((result, failure) -> {
            if (completion.isDone()) {
                return;
            }
            ZLinkTerminationIntent intent =
                effectiveTerminationIntent.get();
            if (intent == null) {
                intent = ZLinkTerminationIntent.SHUTDOWN;
            }
            ZLinkTerminationResult terminal;
            if (forcedReason != null) {
                terminal = new ZLinkTerminationResult(
                    intent,
                    ZLinkTerminationOutcome.FORCE_STOPPED,
                    forcedReason);
            } else if (failure != null) {
                terminal = new ZLinkTerminationResult(
                    intent,
                    ZLinkTerminationOutcome.FORCE_STOPPED,
                    ZLinkTerminationReason.TEARDOWN_FAILED);
            } else if (
                result instanceof InternalDrained) {
                terminal = new ZLinkTerminationResult(
                    intent,
                    ZLinkTerminationOutcome.STOPPED,
                    ZLinkTerminationReason.NONE);
            } else {
                terminal = new ZLinkTerminationResult(
                    intent,
                    ZLinkTerminationOutcome.FORCE_STOPPED,
                    ZLinkTerminationReason.DEADLINE_EXCEEDED);
            }
            terminalTermination.set(terminal);
            lastTerminationResult.set(mapPublicTerminationResult(terminal));
            publishRuntimeState(ZLinkFrameworkRuntimeState.STOPPED);
            completion.complete(terminal);
        });
    }

    private java.util.concurrent.CompletionStage<ZLinkTerminationReason>
        retirePreflight() {
        if (!ready.get()) {
            return java.util.concurrent.CompletableFuture.completedFuture(
                ZLinkTerminationReason.RUNTIME_NOT_READY);
        }
        boolean locationStoreAvailable = storeLocationResolvers != null;
        if (registration.channels().stream().anyMatch(channel ->
                channel.blocksAutomaticRetire(locationStoreAvailable))
            || registration.meshNodes().stream()
                .anyMatch(node -> !node.peers().isEmpty())) {
            return java.util.concurrent.CompletableFuture.completedFuture(
                ZLinkTerminationReason.MANUAL_TOPOLOGY_UNSUPPORTED);
        }
        return automaticRouteMeshRetirePreflight().thenCompose(reason ->
            reason == ZLinkTerminationReason.NONE
                ? retireWorkloadPreflight()
                : java.util.concurrent.CompletableFuture.completedFuture(
                    reason));
    }

    private java.util.concurrent.CompletionStage<ZLinkTerminationReason>
        retireWorkloadPreflight() {
        boolean hasActiveRelocatableWork =
            (actors != null && !actors.activeActorTypes().isEmpty())
                || (spots != null && spots.activeUserSpotCount() > 0);
        if (hasActiveRelocatableWork) {
            boolean relocationPolicyConfigured =
                registration.meshNodes().stream()
                    .anyMatch(
                        systems.zlink.framework.runtime.mesh
                            .MeshNodeRegistration::requiresRelocationStore);
            if (!relocationPolicyConfigured) {
                return java.util.concurrent.CompletableFuture.completedFuture(
                    ZLinkTerminationReason.RELOCATION_DISABLED);
            }
            if (spotRetire == null || !spotRetire.supportsActiveInventory()) {
                return java.util.concurrent.CompletableFuture.completedFuture(
                    ZLinkTerminationReason.STATE_INCOMPATIBLE);
            }
        }
        if (actors == null || actors.activeActorTypes().isEmpty()) {
            return java.util.concurrent.CompletableFuture.completedFuture(
                ZLinkTerminationReason.NONE);
        }
        if (storeLocationResolvers == null || spots == null) {
            return java.util.concurrent.CompletableFuture.completedFuture(
                ZLinkTerminationReason.STORE_UNAVAILABLE);
        }
        java.util.Set<RoutingId> localNodes = new java.util.HashSet<>();
        spots.nodesByName().values().forEach(
            node -> localNodes.add(node.routingId()));
        java.util.concurrent.CompletionStage<ZLinkTerminationReason> result =
            java.util.concurrent.CompletableFuture.completedFuture(
                ZLinkTerminationReason.NONE);
        for (String actorType :
            actors.activeActorTypes().stream().sorted().toList()) {
            result = result.thenCompose(current -> {
                if (current != ZLinkTerminationReason.NONE) {
                    return java.util.concurrent.CompletableFuture
                        .completedFuture(current);
                }
                String meshName =
                    actorDrainMeshName(registration, actorType);
                if (meshName == null) {
                    return java.util.concurrent.CompletableFuture
                        .completedFuture(
                            ZLinkTerminationReason.TARGET_UNAVAILABLE);
                }
                return storeLocationResolvers.listPeers(
                    systems.zlink.framework.runtime.internal.locations
                        .ZLinkAutoConnectType.SPOT_MESH,
                    meshName,
                    systems.zlink.framework.locations.ZLinkLocationRole.SPOT)
                    .thenApply(found -> found.stream()
                        .filter(peer -> !peer.draining())
                        .anyMatch(peer -> isEligibleActorHandoffTarget(
                            peer,
                            actorType,
                            localNodes))
                            ? ZLinkTerminationReason.NONE
                            : ZLinkTerminationReason.TARGET_UNAVAILABLE);
            });
        }
        return result;
    }

    private java.util.concurrent.CompletionStage<ZLinkTerminationReason>
        automaticRouteMeshRetirePreflight() {
        if (registration.meshNodes().isEmpty()) {
            return java.util.concurrent.CompletableFuture.completedFuture(
                ZLinkTerminationReason.NONE);
        }
        if (storeLocationResolvers == null) {
            return java.util.concurrent.CompletableFuture.completedFuture(
                ZLinkTerminationReason.STORE_UNAVAILABLE);
        }
        java.util.concurrent.CompletionStage<ZLinkTerminationReason> result =
            java.util.concurrent.CompletableFuture.completedFuture(
                ZLinkTerminationReason.NONE);
        for (var mesh : registration.meshNodes().stream()
            .sorted(java.util.Comparator.comparing(
                systems.zlink.framework.runtime.mesh.MeshNodeRegistration
                    ::meshName))
            .toList()) {
            result = result.thenCompose(current -> {
                if (current != ZLinkTerminationReason.NONE) {
                    return java.util.concurrent.CompletableFuture
                        .completedFuture(current);
                }
                var local = meshNodes.nodesByName().get(mesh.meshName());
                if (local == null) {
                    return java.util.concurrent.CompletableFuture
                        .completedFuture(
                            ZLinkTerminationReason.TARGET_UNAVAILABLE);
                }
                return storeLocationResolvers.listPeers(
                    systems.zlink.framework.runtime.internal.locations
                        .ZLinkAutoConnectType.ROUTE_MESH,
                    mesh.meshName(),
                    systems.zlink.framework.locations.ZLinkLocationRole.ROUTER)
                    .thenApply(snapshot -> {
                        return hasExactReadyReplacement(
                            snapshot,
                            local.status().routingId(),
                            local.peers())
                            ? ZLinkTerminationReason.NONE
                            : ZLinkTerminationReason.TARGET_UNAVAILABLE;
                    });
            });
        }
        return result;
    }

    static boolean hasExactReadyReplacement(
        java.util.List<systems.zlink.framework.runtime.internal.locations.ZLinkAutoConnectPeer>
            descriptorSnapshot,
        RoutingId localNodeRid,
        java.util.List<systems.zlink.framework.runtime.internal.binding.spot.MeshPeerEntry>
            corePeers) {
        java.util.Objects.requireNonNull(
            descriptorSnapshot, "descriptorSnapshot");
        java.util.Objects.requireNonNull(localNodeRid, "localNodeRid");
        java.util.Objects.requireNonNull(corePeers, "corePeers");
        var replacements = descriptorSnapshot.stream()
            .filter(peer -> !peer.draining())
            .filter(peer -> !peer.nodeRid().equals(localNodeRid))
            .toList();
        return replacements.stream().anyMatch(descriptor ->
                corePeers.stream().anyMatch(peer ->
                    peer.routingId().equals(descriptor.nodeRid())
                        && peer.lifecycleGeneration()
                            == descriptor.generation()
                        && peer.state()
                            == systems.zlink.framework.runtime.internal.binding.spot
                                .MeshPeerState.ADMITTED));
    }

    private systems.zlink.framework.monitoring
        .ZLinkFrameworkRuntimeStatus runtimeStatus(long sequence) {
        ZLinkFrameworkRuntimeState state = runtimeState.get();
        systems.zlink.framework.runtime.internal.dispatch
            .ZLinkInboundDispatchBudget.Snapshot inbound =
            registration.inboundDispatchBudget().snapshot();
        systems.zlink.framework.monitoring.ZLinkInboundDispatchStatus inboundStatus =
            new systems.zlink.framework.monitoring.ZLinkInboundDispatchStatus(
                inbound.applicationHwmBytes(),
                inbound.pendingPayloadBytes(),
                inbound.queuedPayloadBytes(),
                inbound.activePayloadBytes(),
                inbound.applicationReceivePaused(),
                registration.inboundDispatchBudget().pendingCompletionSends(),
                registration.inboundDispatchBudget().completionSendLimit());
        return new systems.zlink.framework.monitoring
            .ZLinkFrameworkRuntimeStatus(
                state,
                state == ZLinkFrameworkRuntimeState.SERVING,
                state == ZLinkFrameworkRuntimeState.SERVING
                    && ready.get()
                    && !drainStarted.get(),
                java.util.Optional.ofNullable(terminationDeadline.get()),
                java.util.Optional.ofNullable(lastRelocationResult.get()),
                java.util.Optional.ofNullable(lastTerminationResult.get()),
                inboundStatus,
                sequence,
                java.time.Instant.now());
    }

    private ZLinkFrameworkTerminationResult toPublicTerminationResult(
        ZLinkTerminationResult result) {
        ZLinkFrameworkTerminationResult mapped =
            mapPublicTerminationResult(result);
        lastTerminationResult.set(mapped);
        return mapped;
    }

    private static ZLinkFrameworkTerminationResult mapPublicTerminationResult(
        ZLinkTerminationResult result) {
        return new ZLinkFrameworkTerminationResult(
            result.outcome() == ZLinkTerminationOutcome.STOPPED
                ? ZLinkFrameworkTerminationOutcome.STOPPED
                : ZLinkFrameworkTerminationOutcome.FORCE_STOPPED,
            switch (result.reason()) {
                case NONE -> ZLinkFrameworkTerminationReason.NONE;
                case DEADLINE_EXCEEDED ->
                    ZLinkFrameworkTerminationReason.DEADLINE_EXCEEDED;
                default -> ZLinkFrameworkTerminationReason.TEARDOWN_FAILED;
            });
    }

    private void publishRuntimeState(
        ZLinkFrameworkRuntimeState state) {
        runtimeState.set(state);
        if (objectDescriptors != null
            && (state == ZLinkFrameworkRuntimeState.RELOCATING
                || state == ZLinkFrameworkRuntimeState.RELOCATED
                || state == ZLinkFrameworkRuntimeState.DRAINING)) {
            objectDescriptors.publish(state).exceptionally(failure -> {
                java.util.logging.Logger.getLogger(
                    ZLinkFrameworkRuntime.class.getName())
                    .warning(
                        "Object Server descriptor state publication failed: "
                            + failure.getMessage());
                return null;
            });
        }
        long sequence = terminationSequence.incrementAndGet();
        var status = runtimeStatus(sequence);
        if (eventDispatcher != null) {
            eventDispatcher.publishHostStatus(status);
        }
        runtimeStatusPublisher.signal();
        routeMeshRuntime.signalAll();
    }

    private java.util.concurrent.CompletionStage<Void>
        publishRuntimeStateAwaited(ZLinkFrameworkRuntimeState state) {
        runtimeState.set(state);
        java.util.concurrent.CompletionStage<Void> publication =
            objectDescriptors == null
                ? java.util.concurrent.CompletableFuture.completedFuture(null)
                : objectDescriptors.publish(state);
        return publication.thenRun(() -> {
            long sequence = terminationSequence.incrementAndGet();
            var status = runtimeStatus(sequence);
            if (eventDispatcher != null) {
                eventDispatcher.publishHostStatus(status);
            }
            runtimeStatusPublisher.signal();
            routeMeshRuntime.signalAll();
        }).exceptionallyCompose(failure -> {
            runtimeState.set(ZLinkFrameworkRuntimeState.SERVING);
            if (spots != null) {
                spots.cancelRelocation();
            }
            if (actors != null) {
                actors.cancelRelocation();
            }
            return java.util.concurrent.CompletableFuture.failedFuture(
                unwrapCompletionFailure(failure));
        });
    }

    @Override
    public void close() {
        try {
            closeAsync().toCompletableFuture().get();
        } catch (java.lang.InterruptedException error) {
            Thread.currentThread().interrupt();
            throw new ZLinkConfigurationException("framework shutdown was interrupted", error);
        } catch (java.util.concurrent.ExecutionException error) {
            Throwable cause = error.getCause();
            if (cause instanceof RuntimeException runtimeError) {
                throw runtimeError;
            }
            throw new ZLinkConfigurationException("framework shutdown failed", cause);
        }
    }

    java.util.concurrent.CompletionStage<Void> closeAsync() {
        return closeGate.close(this::closeCoreAsync);
    }

    private java.util.concurrent.CompletionStage<Void> closeCoreAsync() {
        ready.set(false);
        startupReady.completeExceptionally(
            new IllegalStateException("Framework runtime is shutting down"));
        if (spots != null && !spotRuntimeStopped.get()) {
            spots.beginClose();
        }
        channels.beginClose();
        ZLinkFrameworkShutdown shutdown = new ZLinkFrameworkShutdown();
        // Close completion admission after accepted runtime components have
        // finished their teardown, so graceful drain can still publish the
        // replies it already accepted.
        shutdown.defer(registration.inboundDispatchBudget()::close);
        shutdown.defer(this::closeHandlerExecutor);
        shutdown.defer(backendContext::close);
        shutdown.defer(routeMeshRuntime::close);
        if (authorityRouteRuntime != null) {
            shutdown.defer(authorityRouteRuntime::close);
        }
        if (storeLocationResolvers != null) {
            shutdown.defer(storeLocationResolvers::close);
        }
        shutdown.defer(meshNodes::close);
        if (locationRuntime != null) {
            shutdown.defer(locationRuntime::close);
            shutdown.defer(locationLifecycle::close);
            shutdown.deferStage(locationRuntime::stop);
            if (objectDescriptors != null) {
                shutdown.deferStage(objectDescriptors::remove);
            }
        }
        if (spots != null) {
            shutdown.deferStage(() -> {
                if (spotRuntimeStopped.compareAndSet(false, true)) {
                    return spots.closeAsync();
                }
                return java.util.concurrent.CompletableFuture.completedFuture(null);
            });
        }
        shutdown.defer(channels::close);
        if (locationAutoConnectHost != null) {
            shutdown.deferStage(locationAutoConnectHost::stop);
        }
        if (actors != null) {
            shutdown.deferStage(actors::closeAsync);
        }
        if (streams != null) {
            shutdown.deferStage(streams::closeAsync);
        }
        return shutdown.closeAsync().whenComplete((ignored, failure) -> {
            if (!drainStarted.get() && failure == null) {
                drained.complete(new InternalDrained());
            }
            if (failure != null) {
                publishRuntimeState(ZLinkFrameworkRuntimeState.ERROR);
            } else if (!drainStarted.get()) {
                publishRuntimeState(ZLinkFrameworkRuntimeState.STOPPED);
            } else if (activeTermination.get() == null) {
                terminalTermination.set(new ZLinkTerminationResult(
                    ZLinkTerminationIntent.SHUTDOWN,
                    ZLinkTerminationOutcome.STOPPED,
                    ZLinkTerminationReason.NONE));
                publishRuntimeState(ZLinkFrameworkRuntimeState.STOPPED);
            }
        });
    }

    private java.util.concurrent.CompletionStage<InternalDrainResult> drain(
        java.time.Duration deadline) {
        java.util.Objects.requireNonNull(deadline, "deadline");
        if (deadline.isZero() || deadline.isNegative()) {
            throw new IllegalArgumentException("deadline must be positive");
        }
        if (drainStarted.compareAndSet(false, true)) {
            effectiveTerminationIntent.compareAndSet(
                null, ZLinkTerminationIntent.SHUTDOWN);
            terminationDeadline.compareAndSet(
                null, java.time.Instant.now().plus(deadline));
            meshDrains.sealAll();
            if (streams != null) {
                streams.beginDrain();
            }
            if (spots != null) {
                spots.beginDrain().exceptionally(error -> null);
            }
            if (actors != null) {
                actors.beginDrain();
            }
            ready.set(false);
            publishRuntimeState(ZLinkFrameworkRuntimeState.DRAINING);
            runDrain();
            java.util.concurrent.CompletableFuture.delayedExecutor(
                deadline.toMillis(), java.util.concurrent.TimeUnit.MILLISECONDS)
                .execute(() -> forceStop(
                    InternalDrainForceReason.DEADLINE_EXCEEDED));
        }
        return independentWaiter(drained);
    }

    static <T> java.util.concurrent.CompletionStage<T> independentWaiter(
        java.util.concurrent.CompletionStage<T> shared) {
        return shared.thenApply(result -> result);
    }

    public boolean isReady() {
        return ready.get();
    }

    private void runDrain() {
        java.util.concurrent.CompletionStage<Void> markerPublished = locationAutoConnectHost == null
            ? java.util.concurrent.CompletableFuture.completedFuture(null)
            : locationAutoConnectHost.markDraining();
        markerPublished.whenComplete((ignored, publishFailure) -> {
            if (publishFailure != null) {
                forceStop(InternalDrainForceReason.DRAINING_STATE_PUBLISH_FAILED);
                return;
            }
            java.util.concurrent.CompletionStage<Void> applicationBarrier =
                meshDrains.awaitAllZero();
            if (spots != null) {
                applicationBarrier = applicationBarrier.thenCompose(
                    acceptedIgnored -> spots.awaitDrainBarrier());
            }
            if (actors != null) {
                applicationBarrier = applicationBarrier.thenCompose(
                    acceptedIgnored -> actors.awaitDrainBarrier());
            }
            if (streams != null) {
                applicationBarrier = applicationBarrier.thenCompose(
                    acceptedIgnored -> streams.awaitDrainBarrier());
            }
            applicationBarrier
                .whenComplete((barrierIgnored, barrierFailure) -> {
                if (barrierFailure != null) {
                    forceStop(
                        InternalDrainForceReason.TEARDOWN_FAILED);
                    return;
                }
                java.util.concurrent.CompletionStage<Void> streamBarrier = streams == null
                    ? java.util.concurrent.CompletableFuture.completedFuture(null)
                    : streams.awaitDrainBarrier()
                        .thenCompose(streamIgnored -> streams.notifyServerDrain());
                java.util.concurrent.CompletionStage<Void> actorShutdown =
                    streamBarrier.thenCompose(streamIgnored ->
                        actors == null
                            ? java.util.concurrent.CompletableFuture
                                .completedFuture(null)
                            : actors.closeAsync());
                java.util.concurrent.CompletionStage<Void> spotDrain = actorShutdown.thenCompose(
                    streamIgnored -> spots == null
                        ? java.util.concurrent.CompletableFuture.completedFuture(null)
                        : spots.continueDrain(
                            systems.zlink.framework.spots
                                .ZLinkSpotCloseReason.HOST_SHUTDOWN,
                            java.util.Optional.ofNullable(
                                    terminationDeadline.get())
                                .orElseGet(java.time.Instant::now)));
                spotDrain.thenCompose(spotIgnored -> awaitWorkloadsDrained())
                    .whenComplete((workloadsIgnored, workloadFailure) -> {
                if (workloadFailure != null) {
                    forceStop(InternalDrainForceReason.TEARDOWN_FAILED);
                    return;
                }
                completeDrain();
                    });
            });
        });
    }

    static String actorDrainMeshName(
        systems.zlink.framework.runtime.configuration.ZLinkFrameworkRegistration registration,
        String actorType) {
        String objectMesh = registration.meshNodes().stream()
            .filter(node -> node.actorFactories().containsKey(actorType))
            .map(systems.zlink.framework.runtime.mesh
                .MeshNodeRegistration::meshName)
            .findFirst()
            .orElse(null);
        if (objectMesh != null) {
            return objectMesh;
        }
        return registration.spotNodes().stream()
            .filter(node -> node.actorFactories().containsKey(actorType))
            .map(systems.zlink.framework.runtime.spots.SpotNodeRegistration::meshName)
            .findFirst()
            .orElse(null);
    }

    static String transferRouteChannelName(
        systems.zlink.framework.runtime.configuration.ZLinkFrameworkRegistration registration,
        String spotMeshName) {
        return registration.channels().stream()
            .filter(channel -> channel.kind()
                == systems.zlink.framework.runtime.channels.ChannelKind.ROUTE_MESH)
            .filter(channel -> channel.name().equals(spotMeshName))
            .map(systems.zlink.framework.runtime.channels.ChannelRegistration::name)
            .findFirst()
            .orElse(null);
    }

    static boolean isEligibleActorHandoffTarget(
        systems.zlink.framework.runtime.internal.locations.ZLinkAutoConnectPeer peer,
        String actorType,
        java.util.Set<RoutingId> localNodes) {
        return peer != null
            && actorType != null
            && peer.capabilities() != null
            && peer.capabilities().contains("actor:" + actorType)
            && !localNodes.contains(peer.nodeRid());
    }

    private java.util.concurrent.CompletionStage<Void> awaitWorkloadsDrained() {
        if (drained.isDone() || workloadsDrained()) {
            return java.util.concurrent.CompletableFuture.completedFuture(null);
        }
        java.util.concurrent.CompletableFuture<Void> result = new java.util.concurrent.CompletableFuture<>();
        java.util.concurrent.CompletableFuture.delayedExecutor(10L, java.util.concurrent.TimeUnit.MILLISECONDS)
            .execute(() -> {
                java.util.concurrent.CompletionStage<Void> retrySpotDrain =
                    (actors == null || actors.drainComplete())
                        && spots != null && !spots.drainComplete()
                    ? spots.continueDrain(
                            systems.zlink.framework.spots
                                .ZLinkSpotCloseReason.HOST_SHUTDOWN,
                            java.util.Optional.ofNullable(
                                    terminationDeadline.get())
                                .orElseGet(java.time.Instant::now))
                        .exceptionally(error -> null)
                    : java.util.concurrent.CompletableFuture.completedFuture(null);
                retrySpotDrain.thenCompose(ignored -> awaitWorkloadsDrained())
                    .whenComplete((ignored, failure) -> {
                if (failure != null) {
                    result.completeExceptionally(failure);
                } else {
                    result.complete(null);
                }
                    });
            });
        return result;
    }

    private boolean workloadsDrained() {
        return (spots == null || spots.drainComplete())
            && (actors == null || actors.drainComplete());
    }

    private void completeDrain() {
        if (!drainTerminalStarted.compareAndSet(false, true)) {
            return;
        }
        ZLinkTeardownExecutor.execute(this::completeDrainOnTeardownThread);
    }

    private void completeDrainOnTeardownThread() {
        if (drained.isDone()) {
            return;
        }
        closeAsync()
            .whenComplete((ignored, failure) -> {
                if (failure == null) {
                    drained.complete(new InternalDrained());
                } else {
                    completeForcedStop(
                        InternalDrainForceReason.OWNER_CLEANUP_FAILED);
                }
            });
    }

    private void forceStop(InternalDrainForceReason reason) {
        if (!drainTerminalStarted.compareAndSet(false, true)) {
            return;
        }
        ZLinkTeardownExecutor.execute(() -> forceStopOnTeardownThread(reason));
    }

    private void forceStopOnTeardownThread(
        InternalDrainForceReason initialReason) {
        if (drained.isDone()) {
            return;
        }
        InternalDrainForceReason reason = initialReason;
        systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics.increment(
            "zlink.drain.forced", java.util.Map.of("kind", streams == null ? "runtime" : "session"));
        java.util.concurrent.CompletionStage<Void> notification = streams == null
            ? java.util.concurrent.CompletableFuture.completedFuture(null)
            : streams.notifyServerDrain();
        InternalDrainForceReason requestedReason = reason;
        notification.handle((ignored, failure) -> null)
            .thenCompose(ignored -> closeAsync())
            .whenComplete((ignored, failure) -> completeForcedStop(failure == null
                ? requestedReason
                : InternalDrainForceReason.TEARDOWN_FAILED));
    }

    private void completeForcedStop(
        InternalDrainForceReason reason) {
        drained.complete(new InternalForceStopped(reason));
    }

    private void closeHandlerExecutor() {
        RuntimeException failure = null;
        if (registration.closeHandlerExecutor()) {
            try {
                closeConfiguredHandlerExecutor();
            } catch (RuntimeException ex) {
                failure = ex;
            }
        }
        try {
            closeSerialExecutor();
        } catch (RuntimeException ex) {
            if (failure == null) {
                failure = ex;
            } else {
                failure.addSuppressed(ex);
            }
        }
        if (failure != null) {
            throw failure;
        }
    }

    private void closeConfiguredHandlerExecutor() {
        if (registration.handlerExecutor() instanceof ExecutorService executor) {
            executor.shutdown();
            try {
                if (!executor.awaitTermination(1, TimeUnit.SECONDS)) {
                    executor.shutdownNow();
                    executor.awaitTermination(4, TimeUnit.SECONDS);
                }
            } catch (InterruptedException ex) {
                executor.shutdownNow();
                Thread.currentThread().interrupt();
                throw new ZLinkConfigurationException(
                    "failed to close handler executor", ex);
            }
        } else if (registration.handlerExecutor() instanceof AutoCloseable closeable) {
            try {
                closeable.close();
            } catch (Exception ex) {
                throw new ZLinkConfigurationException(
                    "failed to close handler executor", ex);
            }
        }
    }

    private void closeSerialExecutor() {
        registration.serialExecutor().shutdown();
        try {
            if (!registration.serialExecutor().awaitTermination(1, TimeUnit.SECONDS)) {
                registration.serialExecutor().shutdownNow();
                registration.serialExecutor().awaitTermination(4, TimeUnit.SECONDS);
            }
        } catch (InterruptedException ex) {
            registration.serialExecutor().shutdownNow();
            Thread.currentThread().interrupt();
            throw new ZLinkConfigurationException(
                "failed to close serial executor", ex);
        }
    }

    private sealed interface InternalDrainResult
        permits InternalDrained, InternalForceStopped {
    }

    private record InternalDrained() implements InternalDrainResult {
    }

    private record InternalForceStopped(
        InternalDrainForceReason reason) implements InternalDrainResult {
    }

    private enum InternalDrainForceReason {
        DEADLINE_EXCEEDED,
        DRAINING_STATE_PUBLISH_FAILED,
        OWNER_CLEANUP_FAILED,
        TEARDOWN_FAILED
    }
}
