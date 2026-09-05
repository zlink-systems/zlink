package systems.zlink.framework.runtime.host;
import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.Set;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.Flow;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import java.util.logging.Logger;
import systems.zlink.framework.configuration.ZLinkMessageFlowControl;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.locations.ZLinkLocationReadiness;
import systems.zlink.framework.locations.ZLinkLocationRole;
import systems.zlink.framework.locations.ZLinkPageRequest;
import systems.zlink.framework.monitoring.ZLinkClientServerRuntime;
import systems.zlink.framework.monitoring.ZLinkFanoutRuntime;
import systems.zlink.framework.monitoring.ZLinkFrameworkRuntimeStatus;
import systems.zlink.framework.monitoring.ZLinkListenerKind;
import systems.zlink.framework.monitoring.ZLinkListenerStatus;
import systems.zlink.framework.monitoring.ZLinkObservedStatus;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;
import systems.zlink.framework.runtime.channels.ChannelKind;
import systems.zlink.framework.runtime.channels.ChannelRegistration;
import systems.zlink.framework.runtime.channels.ZLinkMeshApplicationDispatcher;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshBackendAdapter;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerEntry;
import systems.zlink.framework.runtime.internal.drain.ZLinkMeshDrainCoordinator;
import systems.zlink.framework.runtime.internal.locations.ZLinkAutoConnectPeer;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddressResolver;
import systems.zlink.framework.runtime.spots.SpotNodeRegistration;
import systems.zlink.framework.spots.ActorSpotHandleResolver;
import systems.zlink.framework.spots.SpotHandleResolver;

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
import systems.zlink.framework.runtime.mesh.MeshNodeRegistration;
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
import java.util.concurrent.CompletionException;
import java.util.concurrent.TimeUnit;
import java.util.function.Supplier;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

public final class ZLinkFrameworkRuntime
    implements AutoCloseable,
        ZLinkMessageFlowControl {
    public static final Duration DEFAULT_TERMINATION_DEADLINE =
        Duration.ofSeconds(30);
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
    private final systems.zlink.framework.runtime.internal.dispatch
        .ZLinkApplicationJobQueue applicationJobQueue;
    private final ZLinkStateLane capacityStateLane = new ZLinkStateLane();
    private AutoCloseable capacityMetricRegistration = () -> { };
    private AutoCloseable applicationJobQueuePressureMetricRegistration =
        () -> { };
    private long capacityMeasurementEpoch;
    private systems.zlink.framework.monitoring.ZLinkCoreHwmStatus
        lastCoreHwmStatus;
    private final AtomicBoolean coreHwmContextActive =
        new AtomicBoolean(false);
    private final ZLinkFrameworkRegistration registration;
    private final ZLinkRegisteredLocationStores locationStores;
    private final ZLinkMeshDrainCoordinator
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
    private final SpotTransportAddressResolver
        spotTransportAddressResolver;
    private final ZLinkStoreLocationResolvers storeLocationResolvers;
    private final AtomicBoolean spotRuntimeStopped =
        new AtomicBoolean(false);
    private final CompletableFuture<Void> startupReady =
        new CompletableFuture<>();
    private final AtomicReference<
        ZLinkFrameworkRuntimeState> runtimeState =
        new AtomicReference<>(
            ZLinkFrameworkRuntimeState.PREPARING);
    private final AtomicReference<
        ZLinkTerminationIntent> effectiveTerminationIntent =
        new AtomicReference<>();
    private final AtomicReference<
        ZLinkTerminationReason> terminationBlocker =
        new AtomicReference<>();
    private final AtomicReference<
        ZLinkTerminationResult> terminalTermination =
        new AtomicReference<>();
    private final AtomicReference<
        CompletableFuture<ZLinkTerminationResult>>
        activeTermination =
        new AtomicReference<>();
    private final AtomicReference<RelocationOperation>
        activeRelocation =
        new AtomicReference<>();
    private final AtomicReference<
        Instant> terminationDeadline =
        new AtomicReference<>();
    private final AtomicLong terminationSequence =
        new AtomicLong();
    private final AtomicReference<
        ZLinkFrameworkRelocationResult> lastRelocationResult =
        new AtomicReference<>();
    private final AtomicReference<
        ZLinkFrameworkTerminationResult> lastTerminationResult =
        new AtomicReference<>();
    private final AtomicBoolean drainStarted =
        new AtomicBoolean(false);
    private final ZLinkRelocationShutdownGate relocationShutdown =
        new ZLinkRelocationShutdownGate();
    private final AtomicBoolean drainTerminalStarted =
        new AtomicBoolean(false);
    private final ZLinkCloseGate closeGate = new ZLinkCloseGate();
    private final CompletableFuture<
        InternalDrainResult> drained =
        new CompletableFuture<>();
    // Shared, runtime-mutable message-flow mode cell, installed into the diagnostics
    // options so every surface observes setMessageFlowMode live.
    private final AtomicReference<
        ZLinkMessageFlowLogMode> messageFlowMode;
    private final ZLinkRuntimeEventDispatcher eventDispatcher;
    private final systems.zlink.framework.runtime.internal.monitoring
        .ZLinkStatusPublisher<systems.zlink.framework.monitoring
            .ZLinkFrameworkRuntimeStatus> runtimeStatusPublisher =
        systems.zlink.framework.runtime.internal.monitoring
            .ZLinkStatusPublisher.create(
                () -> runtimeStatus(terminationSequence.get()),
                status -> List.of(
                    status.state(),
                    status.isReady(),
                    status.acceptingWork(),
                    status.capacity(),
                    status.deadline(),
                    status.relocationResult(),
                    status.terminationResult(),
                    status.safeToShutdown()),
                ignored -> runtimeState,
                64,
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
        options.registration().codecs().freeze();
        this.eventDispatcher = eventDispatcher;
        this.registration = options.registration();
        this.applicationJobQueue = this.registration.applicationJobQueue();
        this.meshDrains = new systems.zlink.framework.runtime.internal.drain
            .ZLinkMeshDrainCoordinator(this.registration.meshNodes().stream()
                .map(MeshNodeRegistration::meshName)
                .toList());
        var diagnostics = this.registration.dispatchOptions().diagnostics();
        this.messageFlowMode =
            new AtomicReference<>(diagnostics.messageFlow());
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
        this.coreHwmContextActive.set(true);
        this.channels = channelSubsystem.channels();
        this.channels.setHostStateSupplier(runtimeState::get);
        if (this.registration.meshNodes().isEmpty()) {
            this.meshNodes = ZLinkMeshNodesRuntime.empty();
        } else {
            ZLinkMeshBackendAdapter meshAdapter =
                backendFactory.createMeshAdapter(adapterOptions);
            this.meshNodes = ZLinkMeshNodesRuntime.start(
                this.registration.meshNodes(),
                meshAdapter,
                this.backendContext,
                mesh -> new ZLinkMeshApplicationDispatcher(
                    mesh,
                    serializer,
                    this.registration,
                    handlerFactory,
                    this.meshDrains),
                true,
                this.applicationJobQueue);
        }
        this.meshNodes.nodesByName().forEach((meshName, node) ->
            this.channels.registerSpotRouterNode(meshName, node.spotNode()));
        if (this.locationStores != null
            && this.locationStores.unifiedStore()
                instanceof ZLinkLocationRepository store) {
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
        if (this.locationRuntime != null && this.objectDescriptors != null) {
            this.locationRuntime.setOwnerLeaseRecoveryListener(() ->
                this.objectDescriptors.recoverAfterOwnerLease(
                    this.runtimeState.get(),
                    this.locationRuntime.recoveryPreviousOwnerToken()));
        }
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
                    instanceof ZLinkLocationRepository
                        store
                ? store
                : null,
            this.locationRuntime,
            this.storeLocationResolvers,
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
                failure -> Logger.getLogger(
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
            this.meshNodes.nodesByName().values().forEach(node ->
                node.setSessionRelocationSealHandler(
                    this.streams::handleSessionRelocationSeal));
            this.meshNodes.nodesByName().values().forEach(node ->
                node.setBoundSessionSendHandler(
                    this.streams::handleBoundSessionSend));
            this.meshNodes.nodesByName().values().forEach(node ->
                node.setBoundSessionReplacedHandler(
                    this.streams::handleBoundSessionReplaced));
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
                    this.registration.locations().options(),
                    relocationAdapters,
                    this.registration.applicationVersion(),
                    this.registration.maintenanceWave(),
                    this.registration.locations().options()
                        .sessionRelocationSealTimeout())
            : null;

        this.capacityMetricRegistration =
            ZLinkRuntimeMetrics.registerHostCapacity(this::capacityStatus);
        this.applicationJobQueuePressureMetricRegistration =
            ZLinkRuntimeMetrics.registerApplicationJobQueuePressure(
                applicationJobQueue::pressureMetrics);
        locationSubsystem.startup()
            .thenCompose(ignored ->
                this.objectDescriptors == null
                    ? CompletableFuture
                        .completedFuture(null)
                    : this.objectDescriptors.publish(
                        ZLinkFrameworkRuntimeState.PREPARING))
            .thenCompose(ignored ->
                this.locationStores == null
                    || this.locationStores.unifiedStore() == null
                    ? CompletableFuture
                        .completedFuture(null)
                    : CompletableFuture.allOf(
                        this.meshNodes.nodesByName().values().stream()
                            .map(node -> node
                                .refreshLocalAuthorityFence()
                                .toCompletableFuture())
                            .toArray(
                                CompletableFuture[]::new)))
            .thenCompose(ignored -> connectManualObjectPeers())
            .thenCompose(ignored -> spotSubsystem.startup())
            .thenCompose(ignored ->
                this.spotRetire == null
                    ? CompletableFuture
                        .completedFuture(null)
                    : this.spotRetire.startup())
            .thenCompose(ignored ->
                this.authorityRouteRuntime == null
                    ? CompletableFuture
                        .completedFuture(null)
                    : this.authorityRouteRuntime.start())
            .thenCompose(ignored -> ZLinkFrameworkAutoConnectSubsystem.start(
                this.locationAutoConnectHost,
                this.registration,
                this.channels,
                this.meshNodes,
                this.spots))
            .thenCompose(ignored ->
                this.objectDescriptors == null
                    ? CompletableFuture
                        .completedFuture(null)
                    : this.objectDescriptors.publish(
                        ZLinkFrameworkRuntimeState.SERVING))
            .thenRun(() -> this.meshNodes.nodesByName().values().forEach(
                ZLinkInternalMeshNode::markServiceReady))
            .whenComplete((ignored, failure) -> {
                if (failure == null && !drainStarted.get()) {
                    publishRuntimeState(ZLinkFrameworkRuntimeState.SERVING);
                    startupReady.complete(null);
                    Logger.getLogger(
                        ZLinkFrameworkRuntime.class.getName())
                        .info("ZLINK_FRAMEWORK_READY");
                } else if (failure != null) {
                    startupReady.completeExceptionally(
                        unwrapCompletionFailure(failure));
                    publishRuntimeState(ZLinkFrameworkRuntimeState.ERROR);
                    Logger.getLogger(
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

    private CompletionStage<Void>
        connectManualObjectPeers() {
        if (locationStores == null
            || !(locationStores.unifiedStore()
                instanceof systems.zlink.framework.runtime.internal.locations
                    .ZLinkLocationRepository store)) {
            return CompletableFuture.completedFuture(null);
        }
        var tasks = new ArrayList<
            CompletableFuture<?>>();
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
                                connectManualObjectPeer(
                                    source,
                                    peer,
                                    page.items());
                            }
                        }).toCompletableFuture());
        }
        return CompletableFuture.allOf(
            tasks.toArray(CompletableFuture[]::new));
    }

    static boolean connectManualObjectPeer(
        ZLinkInternalMeshNode source,
        MeshNodeRegistration.Peer peer,
        List<systems.zlink.framework.runtime.internal.locations
            .ZLinkMeshNodeDescriptor> descriptors) {
        if (peer.expectedRoutingId() != null) {
            return false;
        }
        return descriptors.stream()
            .filter(target -> target.endpoint().equals(peer.endpoint()))
            .filter(target -> !target.rid().equals(source.status().routingId()))
            .findFirst()
            .map(target -> {
                try {
                    source.replacePeerConnection(
                        target.endpoint(),
                        target.rid(),
                        target.lifecycleGeneration(),
                        target.securityIdentity());
                    return true;
                } catch (IllegalStateException previousConnectionStillOpen) {
                    // The endpoint-only startup intent remains authoritative.
                    // The Location auto-connect loop retries this descriptor
                    // after the raw binding reports the liveness close.
                    return false;
                }
            })
            .orElse(false);
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
    public CompletableFuture<Void> setMessageFlowModeAsync(ZLinkMessageFlowLogMode mode) {
        if (mode == null) {
            throw new IllegalArgumentException("mode is required");
        }
        messageFlowMode.set(mode);
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public ZLinkMessageFlowLogMode messageFlowMode() {
        return registration.dispatchOptions().diagnostics().effectiveMessageFlow();
    }

    public ZLinkFanoutClient fanout() {
        return channels;
    }

    public ZLinkRouteClient route() {
        return channels;
    }

    public ZLinkRouteMeshRuntime
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

    public ZLinkClientServerRuntime
        clientServerRuntime() {
        return channels.clientServerRuntime();
    }

    public ZLinkFanoutRuntime
        fanoutRuntime() {
        return channels.fanoutRuntime();
    }

    /**
     * Returns the endpoint the current local listener provides to remote
     * processes. The query only succeeds after the selected listener has
     * completed its bind operation.
     */
    public ZLinkListenerStatus listenerStatus(
        ZLinkListenerKind kind,
        String name) {
        Objects.requireNonNull(kind, "kind");
        if (name == null || name.isBlank()) {
            throw new IllegalArgumentException("name is required");
        }
        String endpoint = switch (kind) {
            case ROUTE_MESH -> {
                MeshNodeRegistration mesh =
                    registration.meshNodes().stream()
                        .filter(value -> value.meshName().equals(name))
                        .findFirst()
                        .orElseThrow(() -> new ZLinkConfigurationException(
                            "RouteMesh is not configured: " + name));
                ZLinkInternalMeshNode node =
                    meshNodes.nodesByName().get(name);
                if (node == null) {
                    throw new ZLinkConfigurationException(
                        "RouteMesh is not started: " + name);
                }
                String actual = node.status().localEndpoint();
                if (actual == null || actual.isBlank() || actual.endsWith(":0")) {
                    throw new ZLinkConfigurationException(
                        "RouteMesh listener endpoint is not ready: " + name);
                }
                yield mesh.advertisedEndpoint(actual);
            }
            case CLIENT_SERVER, FANOUT -> channels.listenerEndpoint(kind, name);
            case STREAM -> streams.listenerEndpoint(name);
        };
        return new ZLinkListenerStatus(
            kind, name, endpoint, Instant.now());
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

    public Map<String, ZLinkInternalMeshNode>
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
                long storeDeadlineNanos = System.nanoTime()
                    + TimeUnit.MILLISECONDS.toNanos(500);
                String continuation = null;
                do {
                    long remainingMillis = TimeUnit.NANOSECONDS
                        .toMillis(storeDeadlineNanos - System.nanoTime());
                    if (remainingMillis <= 0) {
                        throw new CompletionException(
                            new TimeoutException(
                                "Runtime monitoring Store query deadline exceeded"));
                    }
                    var page = store.listMeshNodes(
                            meshName,
                            new ZLinkPageRequest(
                                128,
                                continuation))
                        .toCompletableFuture()
                        .orTimeout(
                            remainingMillis,
                            TimeUnit.MILLISECONDS)
                        .join();
                    for (var descriptor : page.items()) {
                        if (descriptor.rid().equals(rid)) {
                            return systems.zlink.framework.runtime.internal.monitoring
                                .ZLinkMeshNodeMonitoringProjection.fromDescriptor(
                                    descriptor)
                                .withActiveObjectCounts(
                                    activeActorCount(),
                                    activeSpotCount());
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

    public int activeActorCount() {
        return actors == null ? 0 : actors.activeActorIds().size();
    }

    public int activeSpotCount() {
        return spots == null
            ? 0
            : spots.activeUserSpotCount() + spots.activeInstanceSpotCount();
    }

    public List<String> monitoringMeshNodeChannelNames(String meshName) {
        return registration.meshNodes().stream()
            .filter(candidate -> candidate.meshName().equals(meshName))
            .findFirst()
            .map(MeshNodeRegistration::channelNames)
            .orElseThrow(() -> new ZLinkConfigurationException(
                "RouteMesh is not configured: " + meshName));
    }

    public ZLinkLocationReadiness locationReadiness() {
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

    public ZLinkActorDirectory actorDirectory() {
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

    public SpotHandleResolver spotHandleResolver() {
        if (spotTransportAddressResolver
            instanceof SpotHandleResolver resolver) {
            return resolver;
        }
        throw new ZLinkConfigurationException(
            "SpotHandleResolver requires a configured location store");
    }

    public ActorSpotHandleResolver actorSpotHandleResolver() {
        if (spotTransportAddressResolver
            instanceof ActorSpotHandleResolver resolver) {
            return resolver;
        }
        throw new ZLinkConfigurationException(
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

    public ZLinkFrameworkRuntimeStatus
        status() {
        return runtimeStatus(terminationSequence.get());
    }

    public void resetCapacityMetrics() {
        if (!coreHwmContextActive.get()) {
            throw inactiveCapacityContext();
        }
        inCapacityStateLane(() -> {
            try {
                backendContext.resetCoreHwmBudgetMetrics();
                applicationJobQueue.resetMetrics();
                capacityMeasurementEpoch = Math.incrementExact(
                    capacityMeasurementEpoch);
            } catch (RuntimeException failure) {
                if (!coreHwmContextActive.get()) {
                    throw inactiveCapacityContext(failure);
                }
                throw failure;
            }
            return null;
        });
    }

    public Flow.Publisher<
        ZLinkObservedStatus<
            ZLinkFrameworkRuntimeStatus>>
        observe() {
        return runtimeStatusPublisher;
    }

    public CompletionStage<ZLinkFrameworkRelocationResult>
        relocate(ZLinkFrameworkRelocationOptions options) {
        Objects.requireNonNull(options, "options");
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
        Duration deadline = options.deadline() == null
            ? DEFAULT_TERMINATION_DEADLINE
            : options.deadline();
        if (runtimeState.get() == ZLinkFrameworkRuntimeState.RELOCATED
            && lastRelocationResult.get() != null) {
            return CompletableFuture.completedFuture(
                lastRelocationResult.get());
        }
        RelocationOperation current = activeRelocation.get();
        if (current != null) {
            return current.matches(options.mode(), effectiveTargetVersion)
                ? independentRelocationWaiter(current.completion())
                : CompletableFuture.completedFuture(
                    blockedRelocation(
                        options.mode(),
                        effectiveTargetVersion,
                        ZLinkFrameworkRelocationReason.OPERATION_IN_PROGRESS));
        }
        var completion = new CompletableFuture<
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
        terminationDeadline.set(Instant.now().plus(deadline));
        Instant relocationDeadline = terminationDeadline.get();
        withinRelocationDeadline(
            relocationPreflightUntilDeadline(relocationDeadline),
            relocationDeadline)
            .whenComplete((reason, failure) -> {
            Throwable preflightFailure = unwrapCompletionFailure(failure);
            ZLinkFrameworkRelocationReason blocker = activeTermination.get() != null
                ? ZLinkFrameworkRelocationReason.SHUTDOWN_REQUESTED
                : failure == null
                    ? toPublicRelocationReason(reason)
                    : preflightFailure instanceof TimeoutException
                        || preflightFailure instanceof CancellationException
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
            AtomicBoolean relocationPublished =
                new AtomicBoolean(false);
            CompletionStage<Void> relocation =
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

    private CompletionStage<ZLinkTerminationReason>
        relocationPreflightUntilDeadline(Instant deadline) {
        if (activeTermination.get() != null) {
            return CompletableFuture.completedFuture(
                ZLinkTerminationReason.RUNTIME_NOT_READY);
        }
        return retirePreflight().thenCompose(reason -> {
            if (reason != ZLinkTerminationReason.TARGET_UNAVAILABLE
                || !Instant.now().isBefore(deadline)) {
                return CompletableFuture.completedFuture(reason);
            }
            return CompletableFuture.runAsync(
                    () -> { },
                    CompletableFuture.delayedExecutor(
                        25, TimeUnit.MILLISECONDS))
                .thenCompose(ignored -> relocationPreflightUntilDeadline(deadline));
        });
    }

    private static <T> CompletionStage<T>
        withinRelocationDeadline(
            CompletionStage<T> stage,
            Instant deadline) {
        long remaining = Duration.between(
            Instant.now(), deadline).toMillis();
        if (remaining <= 0) {
            return CompletableFuture.failedFuture(
                new TimeoutException(
                    "Relocation preflight deadline exceeded"));
        }
        return stage.toCompletableFuture().orTimeout(
            remaining, TimeUnit.MILLISECONDS);
    }

    private CompletionStage<Void> relocateWithTargetWait(
        Instant deadline,
        ZLinkFrameworkRelocationMode mode,
        long targetVersion,
        AtomicBoolean relocationPublished) {
        if (activeTermination.get() != null) {
            return CompletableFuture.failedFuture(
                new systems.zlink.framework.runtime.spots
                    .ZLinkUserSpotRetireRuntime.RelocationBlockedException(
                        ZLinkFrameworkRelocationReason.SHUTDOWN_REQUESTED,
                        "Shutdown was requested before relocation admission"));
        }
        if (!Instant.now().isBefore(deadline)) {
            return CompletableFuture.failedFuture(
                new systems.zlink.framework.runtime.spots
                    .ZLinkUserSpotRetireRuntime.RelocationBlockedException(
                        ZLinkFrameworkRelocationReason.TARGET_UNAVAILABLE,
                        "No eligible relocation target became Ready before the deadline"));
        }
        CompletionStage<Void> attempt =
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
                && Instant.now().isBefore(deadline)
                && activeTermination.get() == null) {
                return CompletableFuture.runAsync(
                        () -> { },
                        CompletableFuture.delayedExecutor(
                            25, TimeUnit.MILLISECONDS))
                    .thenCompose(ignored -> relocateWithTargetWait(
                        deadline, mode, targetVersion, relocationPublished));
            }
            return CompletableFuture.failedFuture(cause);
            });
    }

    private CompletionStage<Void>
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

    private CompletionStage<Void>
        validateRelocationTargetPopulation(
            ZLinkFrameworkRelocationMode mode,
            long targetVersion,
            Instant deadline) {
        if (storeLocationResolvers == null) {
            return CompletableFuture.failedFuture(
                new systems.zlink.framework.runtime.spots
                    .ZLinkUserSpotRetireRuntime.RelocationBlockedException(
                        ZLinkFrameworkRelocationReason.STORE_UNAVAILABLE,
                        "Location Store is unavailable"));
        }
        CompletionStage<Void> result =
            CompletableFuture.completedFuture(null);
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
        if (failure instanceof TimeoutException
            || failure instanceof CancellationException) {
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
        CompletableFuture<ZLinkFrameworkRelocationResult>
            completion) {
        private RelocationOperation {
            Objects.requireNonNull(mode, "mode");
            Objects.requireNonNull(completion, "completion");
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
        while (current instanceof CompletionException
            || current instanceof ExecutionException) {
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

    private static CompletionStage<ZLinkFrameworkRelocationResult>
        independentRelocationWaiter(
            CompletionStage<ZLinkFrameworkRelocationResult> source) {
        var waiter =
            new CompletableFuture<
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

    public CompletionStage<ZLinkFrameworkTerminationResult>
        shutdown() {
        return shutdown(DEFAULT_TERMINATION_DEADLINE);
    }

    public CompletionStage<ZLinkFrameworkTerminationResult>
        shutdown(Duration deadline) {
        return beginTermination(ZLinkTerminationIntent.SHUTDOWN, deadline)
            .thenApply(this::toPublicTerminationResult);
    }

    private CompletionStage<ZLinkTerminationResult>
        beginTermination(
            ZLinkTerminationIntent intent,
            Duration deadline) {
        Objects.requireNonNull(intent, "intent");
        Objects.requireNonNull(deadline, "deadline");
        if (deadline.isZero() || deadline.isNegative()) {
            throw new IllegalArgumentException(
                "termination deadline must be positive");
        }
        CompletableFuture<ZLinkTerminationResult>
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
            new CompletableFuture<
                ZLinkTerminationResult>();
        if (!activeTermination.compareAndSet(null, candidate)) {
            return beginTermination(intent, deadline);
        }
        terminalTermination.set(null);
        terminationBlocker.set(null);
        terminationDeadline.set(Instant.now().plus(deadline));
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
            Instant retireDeadline = terminationDeadline.get();
            CompletionStage<Void> relocation =
                spotRetire == null
                    ? CompletableFuture
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
                            return CompletableFuture
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

    private CompletionStage<Void> beginEmptyRelocation() {
        return CompletableFuture.completedFuture(null);
    }

    private void startTermination(
        CompletableFuture<ZLinkTerminationResult>
            completion,
        Duration deadline) {
        startTermination(completion, deadline, null);
    }

    private void startTermination(
        CompletableFuture<ZLinkTerminationResult>
            completion,
        Duration deadline,
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
            } else if (result instanceof InternalForceStopped forced) {
                terminal = new ZLinkTerminationResult(
                    intent,
                    ZLinkTerminationOutcome.FORCE_STOPPED,
                    forced.reason() == InternalDrainForceReason.DEADLINE_EXCEEDED
                        ? ZLinkTerminationReason.DEADLINE_EXCEEDED
                        : ZLinkTerminationReason.TEARDOWN_FAILED);
            } else {
                terminal = new ZLinkTerminationResult(
                    intent,
                    ZLinkTerminationOutcome.FORCE_STOPPED,
                    ZLinkTerminationReason.DEADLINE_EXCEEDED);
            }
            terminalTermination.set(terminal);
            lastTerminationResult.set(mapPublicTerminationResult(terminal));
            publishRuntimeState(ZLinkFrameworkRuntimeState.STOPPED,
                result instanceof InternalForceStopped forced ? forced.failure() : failure);
            completion.complete(terminal);
        });
    }

    private CompletionStage<ZLinkTerminationReason>
        retirePreflight() {
        if (!runtimeState.get().isReadyState()) {
            return CompletableFuture.completedFuture(
                ZLinkTerminationReason.RUNTIME_NOT_READY);
        }
        boolean locationStoreAvailable = storeLocationResolvers != null;
        if (registration.channels().stream().anyMatch(channel ->
                channel.blocksAutomaticRetire(locationStoreAvailable))
            || registration.meshNodes().stream()
                .anyMatch(node -> !node.peers().isEmpty())) {
            return CompletableFuture.completedFuture(
                ZLinkTerminationReason.MANUAL_TOPOLOGY_UNSUPPORTED);
        }
        return automaticRouteMeshRetirePreflight().thenCompose(reason ->
            reason == ZLinkTerminationReason.NONE
                ? retireWorkloadPreflight()
                : CompletableFuture.completedFuture(
                    reason));
    }

    private CompletionStage<ZLinkTerminationReason>
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
                return CompletableFuture.completedFuture(
                    ZLinkTerminationReason.RELOCATION_DISABLED);
            }
            if (spotRetire == null || !spotRetire.supportsActiveInventory()) {
                return CompletableFuture.completedFuture(
                    ZLinkTerminationReason.STATE_INCOMPATIBLE);
            }
        }
        if (actors == null || actors.activeActorTypes().isEmpty()) {
            return CompletableFuture.completedFuture(
                ZLinkTerminationReason.NONE);
        }
        if (storeLocationResolvers == null || spots == null) {
            return CompletableFuture.completedFuture(
                ZLinkTerminationReason.STORE_UNAVAILABLE);
        }
        Set<RoutingId> localNodes = new HashSet<>();
        spots.nodesByName().values().forEach(
            node -> localNodes.add(node.routingId()));
        CompletionStage<ZLinkTerminationReason> result =
            CompletableFuture.completedFuture(
                ZLinkTerminationReason.NONE);
        for (String actorType :
            actors.activeActorTypes().stream().sorted().toList()) {
            result = result.thenCompose(current -> {
                if (current != ZLinkTerminationReason.NONE) {
                    return CompletableFuture
                        .completedFuture(current);
                }
                String meshName =
                    actorDrainMeshName(registration, actorType);
                if (meshName == null) {
                    return CompletableFuture
                        .completedFuture(
                            ZLinkTerminationReason.TARGET_UNAVAILABLE);
                }
                return storeLocationResolvers.listPeers(
                    systems.zlink.framework.runtime.internal.locations
                        .ZLinkAutoConnectType.SPOT_MESH,
                    meshName,
                    ZLinkLocationRole.SPOT)
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

    private CompletionStage<ZLinkTerminationReason>
        automaticRouteMeshRetirePreflight() {
        if (registration.meshNodes().isEmpty()) {
            return CompletableFuture.completedFuture(
                ZLinkTerminationReason.NONE);
        }
        if (storeLocationResolvers == null) {
            return CompletableFuture.completedFuture(
                ZLinkTerminationReason.STORE_UNAVAILABLE);
        }
        CompletionStage<ZLinkTerminationReason> result =
            CompletableFuture.completedFuture(
                ZLinkTerminationReason.NONE);
        for (var mesh : registration.meshNodes().stream()
            .sorted(Comparator.comparing(
                MeshNodeRegistration
                    ::meshName))
            .toList()) {
            result = result.thenCompose(current -> {
                if (current != ZLinkTerminationReason.NONE) {
                    return CompletableFuture
                        .completedFuture(current);
                }
                var local = meshNodes.nodesByName().get(mesh.meshName());
                if (local == null) {
                    return CompletableFuture
                        .completedFuture(
                            ZLinkTerminationReason.TARGET_UNAVAILABLE);
                }
                return storeLocationResolvers.listPeers(
                    systems.zlink.framework.runtime.internal.locations
                        .ZLinkAutoConnectType.ROUTE_MESH,
                    mesh.meshName(),
                    ZLinkLocationRole.ROUTER)
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
        List<ZLinkAutoConnectPeer>
            descriptorSnapshot,
        RoutingId localNodeRid,
        List<MeshPeerEntry>
            corePeers) {
        Objects.requireNonNull(
            descriptorSnapshot, "descriptorSnapshot");
        Objects.requireNonNull(localNodeRid, "localNodeRid");
        Objects.requireNonNull(corePeers, "corePeers");
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
        return new systems.zlink.framework.monitoring
            .ZLinkFrameworkRuntimeStatus(
                state,
                state.isReadyState(),
                state.acceptsWork(!drainStarted.get()),
                Optional.ofNullable(terminationDeadline.get()),
                Optional.ofNullable(lastRelocationResult.get()),
                Optional.ofNullable(lastTerminationResult.get()),
                safeToShutdown(state),
                capacityStatus(),
                sequence,
                Instant.now());
    }

    /**
     * Spec 30 §11 — this source published every relocation unit past its
     * Message Follow route removal point (S4) and its cutover retransmission
     * window ended, so a Shutdown no longer discards follow routes or
     * retransmission copies. Both are source-local observations.
     */
    private boolean safeToShutdown(ZLinkFrameworkRuntimeState state) {
        return state == ZLinkFrameworkRuntimeState.RELOCATED
            && (spotRetire == null
                || spotRetire.relocationSourceQuiescent());
    }

    private systems.zlink.framework.monitoring.ZLinkHostCapacityStatus
        capacityStatus() {
        return inCapacityStateLane(() -> {
            systems.zlink.framework.monitoring.ZLinkCoreHwmStatus core =
                coreHwmBudgetSnapshot()
                    .map(this::projectCoreHwm)
                    .orElseGet(() -> lastCoreHwmStatus == null
                        ? initialCoreHwmStatus()
                        : lastCoreHwmStatus);
            lastCoreHwmStatus = core;
            var queue = applicationJobQueue.snapshot();
            var queueStatus = new systems.zlink.framework.monitoring
                .ZLinkApplicationJobQueueStatus(
                    queue.configuredProfile(),
                    queue.configuredManualMax().isPresent()
                        ? Optional.of(queue.configuredManualMax().getAsLong())
                        : Optional.empty(),
                    queue.configuredPauseThresholdPercent(),
                    queue.configuredResumeThresholdPercent(),
                    queue.effectiveProcessorCount(),
                    queue.effectiveMaxQueuedApplicationJobs(),
                    queue.pausePermitCount(),
                    queue.resumePermitCount(),
                    queue.reservedSupplyPermits(),
                    queue.queuedApplicationJobs(),
                    queue.permitsInUse(),
                    queue.peakPermitsInUse(),
                    queue.pressureState(),
                    queue.currentPauseDuration(),
                    queue.capacityWaiters(),
                    queue.capacityWaitCount(),
                    queue.capacityWaitDuration());
            return new systems.zlink.framework.monitoring
                .ZLinkHostCapacityStatus(
                    capacityMeasurementEpoch,
                    core,
                    queueStatus);
        });
    }

    private systems.zlink.framework.monitoring.ZLinkCoreHwmStatus
        initialCoreHwmStatus() {
        var inbound = registration.inboundDispatch();
        return new systems.zlink.framework.monitoring.ZLinkCoreHwmStatus(
            inbound.coreHwmMemoryLimitBytes().isPresent()
                ? Optional.of(inbound.coreHwmMemoryLimitBytes().getAsLong())
                : Optional.empty(),
            inbound.coreHwmBudgetBytes().isPresent()
                ? Optional.of(inbound.coreHwmBudgetBytes().getAsLong())
                : Optional.empty(),
            inbound.coreHwmProfile(),
            inbound.coreHwmBudgetBytes().orElse(0L),
            0L,
            0L,
            0L,
            0L,
            0L,
            0L,
            0L,
            0L,
            0L,
            0L,
            0L,
            0L,
            0L,
            0L,
            0L,
            0L,
            0L,
            0L,
            0L,
            0L,
            0L,
            0L);
    }

    private systems.zlink.framework.monitoring.ZLinkCoreHwmStatus
        projectCoreHwm(
            systems.zlink.contracts.core.CoreHwmBudgetSnapshot snapshot) {
        var inbound = registration.inboundDispatch();
        return new systems.zlink.framework.monitoring.ZLinkCoreHwmStatus(
            inbound.coreHwmMemoryLimitBytes().isPresent()
                ? Optional.of(inbound.coreHwmMemoryLimitBytes().getAsLong())
                : Optional.empty(),
            inbound.coreHwmBudgetBytes().isPresent()
                ? Optional.of(inbound.coreHwmBudgetBytes().getAsLong())
                : Optional.empty(),
            inbound.coreHwmProfile(),
            snapshot.effectiveCoreBudgetBytes(),
            snapshot.totalAppliedHwmBytes(),
            snapshot.coreQueueAccountedBytes(),
            snapshot.applicationAccountedBytes(),
            snapshot.currentAccountedBytes(),
            snapshot.provisionalAccountedBytes(),
            snapshot.peakAccountedBytes(),
            snapshot.completionCurrentAccountedBytes(),
            snapshot.completionPeakAccountedBytes(),
            snapshot.completionPendingMessageCount(),
            snapshot.totalMessagingAccountedBytes(),
            snapshot.monitorQueueAppliedHwmBytes(),
            snapshot.monitorQueueAccountedBytes(),
            snapshot.totalInstanceAppliedHwmBytes(),
            snapshot.totalInstanceAccountedBytes(),
            snapshot.blockedRatioPpm(),
            snapshot.activeDirectionalQueueCount(),
            snapshot.activeCompletionDirectionalQueueCount(),
            snapshot.activeSendQueueCount(),
            snapshot.activeReceiveQueueCount(),
            snapshot.outstandingApplicationLeaseCount(),
            snapshot.retiredQueueCount(),
            snapshot.deferredOriginCreditBytes());
    }

    private Optional<systems.zlink.contracts.core.CoreHwmBudgetSnapshot>
        coreHwmBudgetSnapshot() {
        if (!coreHwmContextActive.get()) {
            return Optional.empty();
        }
        try {
            return backendContext.coreHwmBudgetSnapshot();
        } catch (RuntimeException failure) {
            if (!coreHwmContextActive.get()) {
                return Optional.empty();
            }
            throw failure;
        }
    }

    private <T> T inCapacityStateLane(Supplier<T> work) {
        try {
            return capacityStateLane.runAsync(work).toCompletableFuture().join();
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

    private static IllegalStateException inactiveCapacityContext() {
        return new IllegalStateException(
            "Capacity metrics reset requires an active Framework runtime context");
    }

    private static IllegalStateException inactiveCapacityContext(
        RuntimeException cause) {
        return new IllegalStateException(
            "Capacity metrics reset requires an active Framework runtime context",
            cause);
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
        publishRuntimeState(state, null);
    }

    private void publishRuntimeState(
        ZLinkFrameworkRuntimeState state, Throwable terminationFailure) {
        runtimeState.set(state);
        if (objectDescriptors != null
            && (state == ZLinkFrameworkRuntimeState.RELOCATING
                || state == ZLinkFrameworkRuntimeState.RELOCATED
                || state == ZLinkFrameworkRuntimeState.DRAINING)) {
            objectDescriptors.publish(state).exceptionally(failure -> {
                Logger.getLogger(
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
            Throwable actual = terminationFailure == null ? null : unwrapCompletionFailure(terminationFailure);
            String stage = "shutdown";
            if (actual instanceof ZLinkFrameworkShutdown.Failure staged) {
                stage = staged.stage();
                actual = staged.getCause();
            }
            eventDispatcher.publishHostStatus(status, stage, actual);
        }
        runtimeStatusPublisher.signal();
        routeMeshRuntime.signalAll();
    }

    private CompletionStage<Void>
        publishRuntimeStateAwaited(ZLinkFrameworkRuntimeState state) {
        runtimeState.set(state);
        CompletionStage<Void> publication =
            objectDescriptors == null
                ? CompletableFuture.completedFuture(null)
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
            return CompletableFuture.failedFuture(
                unwrapCompletionFailure(failure));
        });
    }

    @Override
    public void close() {
        try {
            closeAsync().toCompletableFuture().get();
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            throw new ZLinkConfigurationException("framework shutdown was interrupted", error);
        } catch (ExecutionException error) {
            Throwable cause = error.getCause();
            if (cause instanceof RuntimeException runtimeError) {
                throw runtimeError;
            }
            throw new ZLinkConfigurationException("framework shutdown failed", cause);
        }
    }

    CompletionStage<Void> closeAsync() {
        return closeGate.close(this::closeCoreAsync);
    }

    private void closeBackendContext() {
        inCapacityStateLane(() -> {
            coreHwmBudgetSnapshot().ifPresent(snapshot ->
                lastCoreHwmStatus = projectCoreHwm(snapshot));
            coreHwmContextActive.set(false);
            return null;
        });
        applicationJobQueue.close();
        try {
            capacityMetricRegistration.close();
        } catch (Exception ignored) {
            // Metrics are observational and cannot make runtime teardown fail.
        }
        try {
            applicationJobQueuePressureMetricRegistration.close();
        } catch (Exception ignored) {
            // Metrics are observational and cannot make runtime teardown fail.
        }
        backendContext.close();
    }

    private CompletionStage<Void> closeCoreAsync() {
        ZLinkFrameworkRuntimeState currentState = runtimeState.get();
        if (currentState != ZLinkFrameworkRuntimeState.STOPPED
            && currentState != ZLinkFrameworkRuntimeState.ERROR) {
            publishRuntimeState(ZLinkFrameworkRuntimeState.DRAINING);
        }
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
        shutdown.defer("executor_close", this::closeHandlerExecutor);
        shutdown.defer("context_close", this::closeBackendContext);
        shutdown.defer("route_mesh_close", routeMeshRuntime::close);
        if (authorityRouteRuntime != null) {
            shutdown.defer("authority_route_close", authorityRouteRuntime::close);
        }
        if (storeLocationResolvers != null) {
            shutdown.defer("location_resolvers_close", storeLocationResolvers::close);
        }
        shutdown.defer("mesh_nodes_close", meshNodes::close);
        if (locationRuntime != null) {
            shutdown.defer("location_close", locationRuntime::close);
            shutdown.defer("location_lifecycle_close", locationLifecycle::close);
            shutdown.deferStage("location_stop", locationRuntime::stop);
            if (objectDescriptors != null) {
                shutdown.deferStage("descriptor_remove", objectDescriptors::remove);
            }
        }
        if (spots != null) {
            shutdown.deferStage("spot_close", () -> {
                if (spotRuntimeStopped.compareAndSet(false, true)) {
                    return spots.closeAsync();
                }
                return CompletableFuture.completedFuture(null);
            });
        }
        shutdown.defer("channel_close", channels::close);
        if (locationAutoConnectHost != null) {
            shutdown.deferStage("auto_connect_stop", locationAutoConnectHost::stop);
        }
        if (actors != null) {
            shutdown.deferStage("instance_close", actors::closeAsync);
        }
        if (streams != null) {
            shutdown.deferStage("stream_close", streams::closeAsync);
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

    private CompletionStage<InternalDrainResult> drain(
        Duration deadline) {
        Objects.requireNonNull(deadline, "deadline");
        if (deadline.isZero() || deadline.isNegative()) {
            throw new IllegalArgumentException("deadline must be positive");
        }
        if (drainStarted.compareAndSet(false, true)) {
            effectiveTerminationIntent.compareAndSet(
                null, ZLinkTerminationIntent.SHUTDOWN);
            terminationDeadline.compareAndSet(
                null, Instant.now().plus(deadline));
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
            publishRuntimeState(ZLinkFrameworkRuntimeState.DRAINING);
            runDrain();
            CompletableFuture.delayedExecutor(
                deadline.toMillis(), TimeUnit.MILLISECONDS)
                .execute(() -> forceStop(
                    InternalDrainForceReason.DEADLINE_EXCEEDED));
        }
        return independentWaiter(drained);
    }

    static <T> CompletionStage<T> independentWaiter(
        CompletionStage<T> shared) {
        return shared.thenApply(result -> result);
    }

    public boolean isReady() {
        return runtimeState.get().isReadyState();
    }

    private void runDrain() {
        CompletionStage<Void> markerPublished = locationAutoConnectHost == null
            ? CompletableFuture.completedFuture(null)
            : locationAutoConnectHost.markDraining();
        markerPublished.whenComplete((ignored, publishFailure) -> {
            if (publishFailure != null) {
                forceStop(InternalDrainForceReason.DRAINING_STATE_PUBLISH_FAILED,
                    new ZLinkFrameworkShutdown.Failure("draining_publication", publishFailure));
                return;
            }
            CompletionStage<Void> meshBarrier =
                meshDrains.awaitAllZero();
            CompletionStage<Void> spotBarrier = spots == null
                ? CompletableFuture.completedFuture(null)
                : spots.awaitDrainBarrier();
            CompletionStage<Void> actorBarrier = actors == null
                ? CompletableFuture.completedFuture(null)
                : actors.awaitDrainBarrier();
            CompletionStage<Void> initialStreamBarrier = streams == null
                ? CompletableFuture.completedFuture(null)
                : streams.awaitDrainBarrier();
            CompletionStage<Void> applicationBarrier =
                meshBarrier
                    .thenCompose(barrierStep -> spotBarrier)
                    .thenCompose(barrierStep -> actorBarrier)
                    .thenCompose(barrierStep -> initialStreamBarrier);
                applicationBarrier
                .whenComplete((barrierIgnored, barrierFailure) -> {
                if (barrierFailure != null) {
                    forceStop(
                        InternalDrainForceReason.TEARDOWN_FAILED,
                        new ZLinkFrameworkShutdown.Failure("application_barrier", barrierFailure));
                    return;
                }
                CompletionStage<Void> serverStreamBarrier = streams == null
                    ? CompletableFuture.completedFuture(null)
                    : ZLinkFrameworkShutdown.atStage("stream_drain", () -> streams.awaitDrainBarrier()
                        .thenCompose(streamIgnored -> streams.notifyServerDrain()));
                CompletionStage<Void> actorShutdown =
                    serverStreamBarrier.thenCompose(streamIgnored ->
                        actors == null
                            ? CompletableFuture
                                .completedFuture(null)
                            : ZLinkFrameworkShutdown.atStage("instance_close", actors::closeAsync));
                CompletionStage<Void> spotDrain = actorShutdown.thenCompose(
                    streamIgnored -> spots == null
                        ? CompletableFuture.completedFuture(null)
                        : ZLinkFrameworkShutdown.atStage("spot_close", () -> spots.continueDrain(
                            systems.zlink.framework.spots
                                .ZLinkSpotCloseReason.HOST_SHUTDOWN,
                            Optional.ofNullable(
                                    terminationDeadline.get())
                                .orElseGet(Instant::now))));
                spotDrain.thenCompose(spotIgnored -> awaitWorkloadsDrained())
                    .whenComplete((workloadsIgnored, workloadFailure) -> {
                if (workloadFailure != null) {
                    forceStop(InternalDrainForceReason.TEARDOWN_FAILED, workloadFailure);
                    return;
                }
                completeDrain();
                    });
            });
        });
    }

    static String actorDrainMeshName(
        ZLinkFrameworkRegistration registration,
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
            .map(SpotNodeRegistration::meshName)
            .findFirst()
            .orElse(null);
    }

    static String transferRouteChannelName(
        ZLinkFrameworkRegistration registration,
        String spotMeshName) {
        return registration.channels().stream()
            .filter(channel -> channel.kind()
                == ChannelKind.ROUTE_MESH)
            .filter(channel -> channel.name().equals(spotMeshName))
            .map(ChannelRegistration::name)
            .findFirst()
            .orElse(null);
    }

    static boolean isEligibleActorHandoffTarget(
        ZLinkAutoConnectPeer peer,
        String actorType,
        Set<RoutingId> localNodes) {
        return peer != null
            && actorType != null
            && peer.capabilities() != null
            && peer.capabilities().contains("actor:" + actorType)
            && !localNodes.contains(peer.nodeRid());
    }

    private CompletionStage<Void> awaitWorkloadsDrained() {
        if (drained.isDone() || workloadsDrained()) {
            return CompletableFuture.completedFuture(null);
        }
        CompletableFuture<Void> result = new CompletableFuture<>();
        CompletableFuture.delayedExecutor(10L, TimeUnit.MILLISECONDS)
            .execute(() -> {
                CompletionStage<Void> retrySpotDrain =
                    (actors == null || actors.drainComplete())
                        && spots != null && !spots.drainComplete()
                    ? spots.continueDrain(
                            systems.zlink.framework.spots
                                .ZLinkSpotCloseReason.HOST_SHUTDOWN,
                            Optional.ofNullable(
                                    terminationDeadline.get())
                                .orElseGet(Instant::now))
                        .exceptionally(error -> null)
                    : CompletableFuture.completedFuture(null);
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
            && (actors == null || actors.drainComplete())
            ;
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
                        InternalDrainForceReason.OWNER_CLEANUP_FAILED, failure);
                }
            });
    }

    private void forceStop(InternalDrainForceReason reason) {
        forceStop(reason, null);
    }

    private void forceStop(InternalDrainForceReason reason, Throwable failure) {
        if (!drainTerminalStarted.compareAndSet(false, true)) {
            return;
        }
        ZLinkTeardownExecutor.execute(() -> forceStopOnTeardownThread(reason, failure));
    }

    private void forceStopOnTeardownThread(
        InternalDrainForceReason initialReason, Throwable initialFailure) {
        if (drained.isDone()) {
            return;
        }
        InternalDrainForceReason reason = initialReason;
        ZLinkRuntimeMetrics.increment(
            "zlink.drain.forced", Map.of("kind", streams == null ? "runtime" : "session"));
        CompletionStage<Void> notification = streams == null
            ? CompletableFuture.completedFuture(null)
            : streams.notifyServerDrain();
        InternalDrainForceReason requestedReason = reason;
        notification.handle((ignored, failure) -> null)
            .thenCompose(ignored -> closeAsync())
            .whenComplete((ignored, failure) -> {
                completeForcedStop(failure == null
                    ? requestedReason
                    : InternalDrainForceReason.TEARDOWN_FAILED,
                    initialFailure != null ? initialFailure : failure);
            });
    }

    private void completeForcedStop(
        InternalDrainForceReason reason, Throwable failure) {
        drained.complete(new InternalForceStopped(reason, failure));
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
        InternalDrainForceReason reason, Throwable failure) implements InternalDrainResult {
    }

    private enum InternalDrainForceReason {
        DEADLINE_EXCEEDED,
        DRAINING_STATE_PUBLISH_FAILED,
        OWNER_CLEANUP_FAILED,
        TEARDOWN_FAILED
    }
}
