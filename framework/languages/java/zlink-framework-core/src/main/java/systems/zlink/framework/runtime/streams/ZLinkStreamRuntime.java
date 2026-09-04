package systems.zlink.framework.runtime.streams;
import java.util.Objects;
import java.util.function.BiFunction;
import java.util.function.Function;
import java.util.function.Supplier;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorSurface;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchMessageKind;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowEvent;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowOutcome;

import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;

import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;

import systems.zlink.framework.runtime.internal.backend.*;

import java.time.Duration;
import java.util.ArrayList;
import java.util.EnumSet;
import java.util.HashSet;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Set;
import java.util.function.Predicate;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.Executor;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.logging.Level;
import java.util.logging.Logger;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkRuntimeEventDispatcher;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkReceiveBatchBudget;
import systems.zlink.framework.runtime.actors.ZLinkActorRuntime;
import systems.zlink.framework.runtime.actors.ZLinkSessionActorsRuntime;
import systems.zlink.framework.runtime.configuration.ZLinkFrameworkRegistration;
import systems.zlink.framework.runtime.internal.configuration.ZLinkCodecRegistration;
import systems.zlink.framework.runtime.configuration.ZLinkMetadataPolicyRegistration;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerStages;
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationAdapter;
import systems.zlink.framework.runtime.messaging.ZLinkMessagePayloads;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;
import systems.zlink.framework.runtime.spots.ZLinkSpotRuntime;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionActors;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionPacketDispatcher;
import systems.zlink.framework.streams.ZLinkStreamCompressionCodec;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamError;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;
import systems.zlink.framework.streams.ZLinkStreamSessionError;

public final class ZLinkStreamRuntime implements AutoCloseable {
    private static final Duration PHYSICAL_DISCONNECT_TIMEOUT = Duration.ofSeconds(5);
    private static final Logger LOGGER = Logger.getLogger(ZLinkStreamRuntime.class.getName());
    private static final String HEARTBEAT_PING_NAME = "$zlink.heartbeat.ping";
    private static final String HEARTBEAT_PONG_NAME = "$zlink.heartbeat.pong";
    private static final long HEARTBEAT_TIMEOUT_NANOS = TimeUnit.SECONDS.toNanos(5);
    private static final long IDLE_TIMEOUT_NANOS = TimeUnit.SECONDS.toNanos(30);
    private static final Duration BOUND_SESSION_REPLACEMENT_CLOSE_DELAY =
        Duration.ofMillis(100);
    private static final Duration RECEIVE_POLL_TIMEOUT = Duration.ofMillis(250);
    private static final boolean STREAM_TRACE =
        "1".equals(System.getenv("ZLINK_JAVA_STREAM_TRACE"));
    private final ZLinkBackendContext context;
    private final boolean ownsContext;
    private final ZLinkFrameworkRegistration registration;
    private final systems.zlink.framework.runtime.internal.dispatch
        .ZLinkApplicationJobQueue applicationJobQueue;
    private final ZLinkMessageSerializer serializer;
    private final ZLinkActorRuntime actors;
    private final Map<String, ZLinkInternalMeshNode> meshNodes;
    private final ZLinkHandlerActivator handlerFactory;
    private final Executor handlerExecutor;
    private final Executor serialExecutor;
    private final ZLinkMessageFlowTracer flow;
    private final List<ZLinkSuspendInvocationAdapter> suspendHandlerInvokers;
    private final ZLinkStreamCodec defaultCodec;
    private final ZLinkStreamCompressionCodec compressionCodec;
    private final Predicate<RoutingId> sessionRelayRouteReady;
    private final ZLinkSessionActorsRuntime.LocalActorDispatcher localActorDispatcher;
    private final ZLinkMetadataPolicyRegistration metadataPolicy;
    private final Duration sessionRelocationSealTimeout;
    private final Duration sessionReplacementCallbackTimeout;
    private final List<ZLinkBackendStreamSocket> streams = new ArrayList<>();
    private final Map<String, ZLinkBackendStreamSocket> streamsByName = new HashMap<>();
    private final Map<String, Boolean> streamSessionRelayAttached = new HashMap<>();
    private final Map<String, ZLinkInternalSpotNode> streamSessionRelaySpotNodes = new HashMap<>();
    private final Map<String, SessionState> sessions = new HashMap<>();
    private final Map<String, CompletableFuture<SessionState>> pendingSessionCreations =
        new HashMap<>();
    private final ThreadLocal<Set<CompletableFuture<SessionState>>>
        sessionCreationProducers = ThreadLocal.withInitial(HashSet::new);
    private final ZLinkStateLane stateLane = new ZLinkStateLane();
    private final ScheduledExecutorService livenessExecutor;
    private final ScheduledExecutorService replyRetryExecutor;
    private final ExecutorService receiveExecutor;
    private final List<StreamReceiveLoop> receiveLoops = new ArrayList<>();
    private final Set<ZLinkStreamSessionContextState> sessionContexts =
        ConcurrentHashMap.newKeySet();
    private volatile boolean draining;

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

    public ZLinkStreamRuntime(
        ZLinkBackendAdapterProvider backendFactory,
        ZLinkBackendAdapterOptions adapterOptions,
        ZLinkFrameworkRegistration registration,
        Map<String, ZLinkInternalSpotNode> spotNodes,
        ZLinkMessageSerializer serializer,
        ZLinkActorRuntime actors,
        ZLinkHandlerActivator handlerFactory) {
        this(
            backendFactory,
            adapterOptions,
            registration,
            spotNodes,
            Map.of(),
            serializer,
            actors,
            handlerFactory,
            ignored -> true,
            null,
            null,
            backendFactory.createChannelAdapter(adapterOptions).createContext(),
            true);
    }

    public ZLinkStreamRuntime(
        ZLinkBackendAdapterProvider backendFactory,
        ZLinkBackendAdapterOptions adapterOptions,
        ZLinkFrameworkRegistration registration,
        Map<String, ZLinkInternalSpotNode> spotNodes,
        ZLinkMessageSerializer serializer,
        ZLinkActorRuntime actors,
        ZLinkHandlerActivator handlerFactory,
        Predicate<RoutingId> sessionRelayRouteReady,
        ZLinkSpotRuntime spots) {
        this(
            backendFactory,
            adapterOptions,
            registration,
            spotNodes,
            Map.of(),
            serializer,
            actors,
            handlerFactory,
            sessionRelayRouteReady,
            spots,
            null,
            backendFactory.createChannelAdapter(adapterOptions).createContext(),
            true);
    }

    public ZLinkStreamRuntime(
        ZLinkBackendAdapterProvider backendFactory,
        ZLinkBackendAdapterOptions adapterOptions,
        ZLinkFrameworkRegistration registration,
        Map<String, ZLinkInternalSpotNode> spotNodes,
        Map<String, ZLinkInternalMeshNode> meshNodes,
        ZLinkMessageSerializer serializer,
        ZLinkActorRuntime actors,
        ZLinkHandlerActivator handlerFactory,
        Predicate<RoutingId> sessionRelayRouteReady,
        ZLinkSpotRuntime spots,
        ZLinkRuntimeEventDispatcher eventDispatcher,
        ZLinkBackendContext context,
        boolean ownsContext) {
        this(
            backendFactory,
            adapterOptions,
            registration,
            spotNodes,
            meshNodes,
            serializer,
            actors,
            handlerFactory,
            sessionRelayRouteReady,
            spots,
            eventDispatcher,
            context,
            ownsContext,
            (ignoredBackend, ignoredKey) -> (ignoredSubmission, ignoredCleanup) ->
                CompletableFuture.failedFuture(new IllegalStateException(
                    "one-way admission factory is required")));
    }

    public ZLinkStreamRuntime(
        ZLinkBackendAdapterProvider backendFactory,
        ZLinkBackendAdapterOptions adapterOptions,
        ZLinkFrameworkRegistration registration,
        Map<String, ZLinkInternalSpotNode> spotNodes,
        Map<String, ZLinkInternalMeshNode> meshNodes,
        ZLinkMessageSerializer serializer,
        ZLinkActorRuntime actors,
        ZLinkHandlerActivator handlerFactory,
        Predicate<RoutingId> sessionRelayRouteReady,
        ZLinkSpotRuntime spots,
        ZLinkRuntimeEventDispatcher eventDispatcher,
        ZLinkBackendContext context,
        boolean ownsContext,
        BiFunction<
            ZLinkBackendObject,
            ZLinkBackendAdmissionKey,
            BiFunction<
                Supplier<Boolean>,
                Runnable,
                CompletionStage<Void>>> admission) {
        if (registration.streamNodes().isEmpty()) {
            throw new ZLinkConfigurationException("at least one stream node is required");
        }
        this.registration = Objects.requireNonNull(
            registration, "registration");
        this.sessionRelocationSealTimeout = registration.locations()
            .options()
            .sessionRelocationSealTimeout();
        this.sessionReplacementCallbackTimeout =
            registration.sessionReplacementCallbackTimeout();
        this.applicationJobQueue = registration.applicationJobQueue();
        this.serializer = serializer;
        this.actors = actors;
        this.meshNodes = Map.copyOf(meshNodes);
        this.handlerFactory = handlerFactory;
        this.handlerExecutor = ZLinkFlowContext.propagating(Objects.requireNonNull(
            registration.handlerExecutor(),
            "handlerExecutor"));
        this.serialExecutor = registration.serialExecutor();
        this.flow = new ZLinkMessageFlowTracer(
            registration.dispatchOptions(), handlerFactory, this.handlerExecutor, eventDispatcher);
        this.suspendHandlerInvokers = registration.suspendHandlerInvokers();
        this.defaultCodec = defaultCodec(registration);
        this.compressionCodec = registration.streamCompressionCodec();
        this.sessionRelayRouteReady =
            sessionRelayRouteReady == null ? ignored -> true : sessionRelayRouteReady;
        this.localActorDispatcher = spots == null ? null : spots::dispatchLocalSessionActor;
        this.metadataPolicy = registration.metadataPolicy();
        this.livenessExecutor = Executors.newSingleThreadScheduledExecutor(task -> {
            Thread thread = new Thread(task, "zlink-stream-liveness");
            thread.setDaemon(true);
            return thread;
        });
        this.replyRetryExecutor = Executors.newSingleThreadScheduledExecutor(task -> {
            Thread thread = new Thread(task, "zlink-stream-reply-retry");
            thread.setDaemon(true);
            return thread;
        });
        this.receiveExecutor = Executors.newFixedThreadPool(
            Math.max(1, registration.streamNodes().size()),
            task -> {
                Thread thread = new Thread(task, "zlink-stream-recv");
                thread.setDaemon(true);
                return thread;
            });
        ZLinkStreamBackendAdapter streamAdapter =
            backendFactory.createStreamAdapter(adapterOptions);
        this.context = Objects.requireNonNull(context, "context");
        this.ownsContext = ownsContext;
        for (StreamNodeRegistration streamNode : registration.streamNodes()) {
            String actorMeshName = streamNode.actorDispatchEnabled()
                ? resolveActorDispatchMeshName()
                : null;
            ZLinkInternalMeshNode meshNode = actorMeshName == null
                ? null
                : meshNodes.get(actorMeshName);
            ZLinkBackendStreamSocket stream =
                streamAdapter.createStreamSocket(context, meshNode);
            if (streamNode.tlsServer() != null) {
                stream.setTlsServer(
                    streamNode.tlsServer().certificatePath(),
                    streamNode.tlsServer().keyPath(),
                    streamNode.tlsServer().requireClientCertificate());
            }
            stream.setMaxMessageSize(streamNode.socketConfig().maxMessageSize());
            // Notification records must be enabled before bind. Framework
            // ingress below uses recv mode and never registers onPacket.
            for (String bindEndpoint : streamNode.bindEndpoints()) {
                trace(STREAM_TRACE ? "stream-node bind node=" + streamNode.name() + " endpoint=" + bindEndpoint : null);
                stream.bind(bindEndpoint);
            }
            stream.onTransportError((routingId, nativeCode, message) ->
                reportTransportError(streamNode, routingId, nativeCode, message));
            stream.startSessionService();
            ZLinkInternalSpotNode spotNode = resolveSessionRelayNode(spotNodes);
            streams.add(stream);
            streamsByName.put(streamNode.name(), stream);
            streamSessionRelayAttached.put(
                streamNode.name(),
                spotNode != null);
            if (spotNode != null) {
                streamSessionRelaySpotNodes.put(streamNode.name(), spotNode);
            }
            receiveLoops.add(new StreamReceiveLoop(streamNode, stream));
        }
        receiveLoops.forEach(StreamReceiveLoop::start);
        livenessExecutor.scheduleAtFixedRate(
            this::checkSessionLiveness, 1L, 1L, TimeUnit.SECONDS);
    }

    private String resolveActorDispatchMeshName() {
        if (actors == null || actors.meshName() == null
            || actors.meshName().isBlank()) {
            throw new ZLinkConfigurationException(
                "stream actor dispatch requires a configured actor authority mesh");
        }
        if (!meshNodes.containsKey(actors.meshName())) {
            throw new ZLinkConfigurationException(
                "stream actor dispatch authority mesh is not configured: "
                    + actors.meshName());
        }
        return actors.meshName();
    }

    private static ZLinkInternalSpotNode resolveSessionRelayNode(
        Map<String, ZLinkInternalSpotNode> spotNodes) {
        return spotNodes.values().stream().findFirst().orElse(null);
    }

    public ZLinkSessionActorsRuntime sessionActors(
        String streamNodeName,
        RoutingId sessionRid,
        ZLinkActorRuntime actors) {
        ZLinkBackendStreamSocket stream = streamsByName.get(streamNodeName);
        if (stream == null) {
            throw new ZLinkConfigurationException(
                "stream node is not running: " + streamNodeName);
        }
        return new ZLinkSessionActorsRuntime(
            streamSessionRelaySpotNodes.get(streamNodeName),
            stream,
            sessionRid,
            actors,
            serializer,
            sessionRelayRouteReady,
            localActorDispatcher,
            streamSessionRelayAttached.getOrDefault(streamNodeName, false),
            defaultCodec,
            flow,
            sessionRelocationSealTimeout)
            .metadataPolicy(
                metadataPolicy.sessionToActorKeys(),
                metadataPolicy.actorToSessionKeys());
    }

    public String listenerEndpoint(String streamNodeName) {
        StreamNodeRegistration registration = this.registration.streamNodes().stream()
            .filter(value -> value.name().equals(streamNodeName))
            .findFirst()
            .orElseThrow(() -> new ZLinkConfigurationException(
                "stream node is not configured: " + streamNodeName));
        ZLinkBackendStreamSocket stream = streamsByName.get(streamNodeName);
        if (stream == null) {
            throw new ZLinkConfigurationException(
                "stream node is not started: " + streamNodeName);
        }
        String actual = stream.lastEndpoint();
        if (actual == null || actual.isBlank()) {
            actual = registration.bindEndpoint();
        }
        if (actual == null || actual.isBlank() || actual.endsWith(":0")) {
            throw new ZLinkConfigurationException(
                "stream listener endpoint is not ready: " + streamNodeName);
        }
        return registration.advertisedEndpoint(actual);
    }

    public CompletionStage<Void> handleSessionRelocationRoute(
        RoutingId transportSource,
        byte[] command44) {
        var codec = new systems.zlink.framework.runtime.internal.service
            .ZLinkServiceM6BWireCodec();
        var command = codec.decodeSessionRelocationRoute(command44);
        RoutingId expectedSource = command.action()
                == systems.zlink.framework.runtime.internal.service
                    .ZLinkServiceM6BWireCodec
                    .SessionRelocationRouteAction.COMMIT
            ? command.targetNodeRid()
            : command.coordinator().nodeRid();
        if (!expectedSource.equals(transportSource)) {
            return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                "command 44 transport source differs from its sender fence"));
        }
        List<SessionState> matches = inStateLane(() ->
            sessions.values().stream()
                .filter(state -> state.routingId().equals(
                    command.session().sessionRid()))
                .toList());
        if (matches.size() != 1) {
            return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                "command 44 requires one exact local Session"));
        }
        return matches.getFirst().context()
            .applyRelocationRouteCommand(command);
    }

    /**
     * Command 42 endpoint. The relocation source addresses the seal to this
     * Session owner; the sender fence is the Actor owner node the binding
     * still points at. The exact command 42 contract permits only the source
     * role; command 43 echoes the seal fields without adding completion
     * state.
     */
    public CompletionStage<byte[]> handleSessionRelocationSeal(
        RoutingId transportSource,
        byte[] command42) {
        var codec = new systems.zlink.framework.runtime.internal.service
            .ZLinkServiceM6BWireCodec();
        var command = codec.decodeSessionRelocationSeal(command42);
        RoutingId expectedSource = command.senderRole()
                == systems.zlink.framework.runtime.internal.service
                    .ZLinkServiceM6BWireCodec.RelocationRole.SOURCE
            ? command.actor().actor().nodeRid()
            : command.coordinator().nodeRid();
        if (!expectedSource.equals(transportSource)) {
            return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                "command 42 transport source differs from the sender fence"));
        }
        List<SessionState> matches = inStateLane(() ->
            sessions.values().stream()
                .filter(state -> state.routingId().equals(
                    command.session().sessionRid()))
                .toList());
        if (matches.size() != 1) {
            return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                "command 42 requires one exact local Session"));
        }
        return matches.getFirst().context()
            .applyRelocationSealCommand(command)
            .thenApply(codec::encodeSessionRelocationSealed);
    }

    public boolean handleBoundSessionSend(
        RoutingId sourceNodeRid,
        long sourceNodeGeneration,
        ZLinkServiceM6BWireCodec.BoundSessionSend command,
        ZLinkServiceM6AWireCodec.ApplicationPayload payload) {
        List<? extends BoundSessionSendOwner> owners = inStateLane(() ->
            sessions.values().stream()
                .map(SessionState::actorRuntime)
                .filter(Objects::nonNull)
                .map(SessionBoundSessionSendOwner::new)
                .toList());
        return dispatchBoundSessionSend(
            owners, sourceNodeRid, sourceNodeGeneration, command, payload);
    }

    static boolean dispatchBoundSessionSend(
        List<? extends BoundSessionSendOwner> owners,
        RoutingId sourceNodeRid,
        long sourceNodeGeneration,
        ZLinkServiceM6BWireCodec.BoundSessionSend command,
        ZLinkServiceM6AWireCodec.ApplicationPayload payload) {
        List<? extends BoundSessionSendOwner> matches = owners.stream()
            .filter(owner -> owner.matches(
                sourceNodeRid, sourceNodeGeneration, command))
            .toList();
        return matches.size() == 1
            && matches.getFirst().accept(
                sourceNodeRid, sourceNodeGeneration, command, payload);
    }

    interface BoundSessionSendOwner {
        boolean matches(
            RoutingId sourceNodeRid,
            long sourceNodeGeneration,
            ZLinkServiceM6BWireCodec.BoundSessionSend command);

        boolean accept(
            RoutingId sourceNodeRid,
            long sourceNodeGeneration,
            ZLinkServiceM6BWireCodec.BoundSessionSend command,
            ZLinkServiceM6AWireCodec.ApplicationPayload payload);
    }

    private record SessionBoundSessionSendOwner(
        ZLinkSessionActorsRuntime runtime) implements BoundSessionSendOwner {
        @Override
        public boolean matches(
            RoutingId sourceNodeRid,
            long sourceNodeGeneration,
            ZLinkServiceM6BWireCodec.BoundSessionSend command) {
            return runtime.matchesBoundSessionSend(
                sourceNodeRid, sourceNodeGeneration, command);
        }

        @Override
        public boolean accept(
            RoutingId sourceNodeRid,
            long sourceNodeGeneration,
            ZLinkServiceM6BWireCodec.BoundSessionSend command,
            ZLinkServiceM6AWireCodec.ApplicationPayload payload) {
            return runtime.acceptBoundSessionSend(
                sourceNodeRid, sourceNodeGeneration, command, payload);
        }
    }

    /**
     * Handles the one-way boundSessionReplaced notice without putting it in a
     * user application mailbox. The callback is started on the Session's
     * serial lane, while its completion and both close timers are observed by
     * the runtime scheduler so the lane is returned immediately.
     */
    public void handleBoundSessionReplaced(
        RoutingId transportSource,
        systems.zlink.framework.runtime.internal.service
            .ZLinkServiceM6BWireCodec.BoundSessionReplaced replacement) {
        Objects.requireNonNull(transportSource, "transportSource");
        Objects.requireNonNull(replacement, "replacement");
        var actor = replacement.actorAuthority().actor();
        var retired = replacement.retiredSession();
        if (!transportSource.equals(actor.nodeRid())) {
            return;
        }
        SessionState state = findSessionForReplacement(retired);
        if (state == null || state.actorRuntime() == null
            || state.actorRuntime().find(actor.actorId())
                .filter(bound -> bound.ref().objectGeneration()
                    == actor.generation()
                && bound.ref().nodeRid().equals(actor.nodeRid()))
                .isEmpty()) {
            return;
        }
        ReplacementIdentity identity = new ReplacementIdentity(
            actor.actorId(),
            retired.sessionRid(),
            retired.retiredBindingGeneration());
        if (!state.beginReplacement(identity)) {
            return;
        }
        try {
            // Replacement is lifecycle work, not application work.  A
            // barrier keeps it ahead of application turns that were already
            // admitted while still preserving the active turn's completion
            // boundary.  New application ingress is closed above, so this
            // is the only queued lifecycle callback for this exact identity.
            CompletionStage<Void> queued = state.serials().executeLifecycleNext(() -> {
                ScheduledFuture<?> deadline = scheduleReplacementClose(
                    state,
                    identity,
                    sessionReplacementCallbackTimeout);
                CompletionStage<Void> callback;
                try {
                    callback = executeHandler(() ->
                        ZLinkHandlerStages.fromStageSupplier(
                            () -> state.session().onActorBindingReplaced(
                                actor.actorId())));
                } catch (RuntimeException failure) {
                    callback = CompletableFuture.failedFuture(failure);
                }
                ScheduledFuture<?> callbackDeadline = deadline;
                callback.whenComplete((ignored, failure) -> {
                    if (callbackDeadline != null) {
                        callbackDeadline.cancel(false);
                    }
                    scheduleReplacementClose(
                        state,
                        identity,
                        BOUND_SESSION_REPLACEMENT_CLOSE_DELAY);
                });
                // Do not return callback. The callback's terminal result is
                // observed above and the Session queue turn is free now.
                return CompletableFuture.completedFuture(null);
            });
            queued.whenComplete((ignored, failure) -> {
                if (failure != null) {
                    scheduleReplacementClose(
                        state,
                        identity,
                        BOUND_SESSION_REPLACEMENT_CLOSE_DELAY);
                }
            });
        } catch (RuntimeException rejected) {
            scheduleReplacementClose(
                state,
                identity,
                BOUND_SESSION_REPLACEMENT_CLOSE_DELAY);
        }
    }

    private SessionState findSessionForReplacement(
        systems.zlink.framework.runtime.internal.service
            .ZLinkServiceM6BWireCodec.RetiredSessionRouteFence retired) {
        return inStateLane(() -> sessions.values().stream()
                .filter(state -> state.matchesOwner(retired)
                    && state.routingId().equals(retired.sessionRid()))
                .findFirst()
                .orElse(null));
    }

    private ScheduledFuture<?> scheduleReplacementClose(
        SessionState state,
        ReplacementIdentity identity,
        Duration delay) {
        try {
            return replyRetryExecutor.schedule(() -> {
                closeReplacedSessionIfExact(state, identity);
            },
                delay.toNanos(),
                TimeUnit.NANOSECONDS);
        } catch (RuntimeException rejected) {
            closeReplacedSessionIfExact(state, identity);
            return null;
        }
    }

    private void closeReplacedSessionIfExact(
        SessionState state,
        ReplacementIdentity identity) {
        if (!isCurrentSessionState(state)
            || !state.matchesReplacement(identity)
            || !state.closeScheduled().compareAndSet(false, true)) {
            return;
        }
        try {
            // Publish SERVER_DRAIN at the timer boundary. The connector closes
            // itself after consuming this control record; a short fallback
            // disconnect covers a failed admission without occupying the lane.
            try {
                sendSessionClosing(state.stream(), state.routingId());
            } catch (RuntimeException sendFailure) {
                LOGGER.log(Level.FINE,
                    "bound Session replacement close notice failed: "
                        + state.routingId(),
                    sendFailure);
            }
            try {
                replyRetryExecutor.schedule(() ->
                    disconnectReplacedSessionIfExact(state, identity),
                    25,
                    TimeUnit.MILLISECONDS);
            } catch (RuntimeException rejected) {
                disconnectReplacedSessionIfExact(state, identity);
            }
        } catch (RuntimeException failure) {
            LOGGER.log(Level.FINE,
                "bound Session replacement close failed: "
                    + state.routingId(),
                failure);
        }
    }

    private void disconnectReplacedSessionIfExact(
        SessionState state,
        ReplacementIdentity identity) {
        if (!isCurrentSessionState(state)
            || !state.matchesReplacement(identity)
            || !state.closeScheduled().get()) {
            return;
        }
        try {
            state.stream().disconnectPeer(state.routingId());
        } catch (RuntimeException failure) {
            LOGGER.log(Level.FINE,
                "bound Session replacement transport disconnect failed: "
                    + state.routingId(),
                failure);
        }
    }

    private boolean isCurrentSessionState(SessionState state) {
        return inStateLane(() ->
            sessions.values().stream().anyMatch(current -> current == state));
    }

    private final class StreamReceiveLoop implements AutoCloseable {
        private final StreamNodeRegistration streamNode;
        private final ZLinkBackendStreamSocket stream;
        private final ZLinkStateLane receiveStateLane = new ZLinkStateLane();
        private boolean closed;

        private StreamReceiveLoop(
            StreamNodeRegistration streamNode,
            ZLinkBackendStreamSocket stream) {
            this.streamNode = streamNode;
            this.stream = stream;
        }

        private void start() {
            receiveExecutor.execute(this::runLoop);
        }

        private StreamNodeRegistration streamNode() {
            return streamNode;
        }

        private void removePeer(RoutingId routingId) {
            // PACKET mode has no per-peer partial assembler state.
        }

        @Override
        public void close() {
            inReceiveStateLane(() -> {
                closed = true;
                return null;
            });
        }

        private void runLoop() {
            while (!isClosed()) {
                try {
                    if (!stream.waitForReadable(RECEIVE_POLL_TIMEOUT)) {
                        continue;
                    }
                    if (isClosed()) {
                        return;
                    }
                    ZLinkReceiveBatchBudget batch = new ZLinkReceiveBatchBudget();
                    boolean pulledPacket = false;
                    while (batch.canReceiveNext()) {
                        if (pulledPacket && !stream.waitForReadable(Duration.ZERO)) {
                            break;
                        }
                        systems.zlink.framework.runtime.internal.dispatch
                            .ZLinkApplicationJobQueue.Permit permit;
                        try {
                            permit = applicationJobQueue.acquireBlocking();
                        } catch (InterruptedException interrupted) {
                            Thread.currentThread().interrupt();
                            return;
                        }
                        ZLinkBackendStreamReceived received = stream.recv();
                        if (received == null) {
                            permit.abandonReservation();
                            break;
                        }
                        pulledPacket = true;
                        boolean transferred = false;
                        try {
                            transferred = processReceived(received, permit, batch);
                        } finally {
                            if (!transferred) {
                                received.close();
                                permit.abandonReservation();
                            }
                        }
                    }
                } catch (RuntimeException | Error failure) {
                    if (!isClosed()) {
                        LOGGER.log(Level.WARNING,
                            "STREAM receive loop failed: " + streamNode.name(),
                            failure);
                        return;
                    }
                }
            }
        }

        private boolean processReceived(
            ZLinkBackendStreamReceived received,
            systems.zlink.framework.runtime.internal.dispatch
                .ZLinkApplicationJobQueue.Permit permit,
            ZLinkReceiveBatchBudget batch) {
            RoutingId routingId = received.routingId().orElse(null);
            if (routingId == null) {
                LOGGER.warning("STREAM packet did not provide a source routing id: "
                    + streamNode.name());
                return false;
            }
            if (isClosed()) {
                return false;
            }
            try {
                ZLinkStreamHeader header = ZLinkStreamHeaderCodec.decodeOrPlain(
                    received.header().toByteArray());
                long messageBytes = received.header().size()
                    + (long) received.body().size();
                long maxMessageSize = streamNode.socketConfig().maxMessageSize();
                if (maxMessageSize > 0 && messageBytes > maxMessageSize) {
                    throw new ZLinkStreamMessageTooLargeException(
                        "STREAM packet exceeds MaxMessageSize");
                }
                try (var ignored = systems.zlink.framework.runtime.internal.dispatch
                        .ZLinkApplicationJobContext.enter(permit)) {
                    dispatchToSession(
                        streamNode,
                        routingId,
                        header,
                        received.body()).whenComplete(
                            (ignoredResult, error) -> received.close());
                } finally {
                    permit.abandonReservation();
                }
                batch.record(messageBytes);
                return true;
            } catch (RuntimeException | Error failure) {
                isolatePeer(routingId, failure);
                return false;
            }
        }

        private void isolatePeer(RoutingId routingId, Throwable failure) {
            boolean messageTooLarge = failure instanceof
                ZLinkStreamMessageTooLargeException;
            int nativeCode = messageTooLarge
                ? ZLinkStreamMessageTooLargeException.EMSGSIZE
                : 0;
            String reason = messageTooLarge
                ? "EMSGSIZE"
                : "malformed STREAM frame";
            SessionState session = removeSessionState(streamNode, routingId);
            try {
                sendSessionClosing(
                    stream,
                    routingId,
                    ZLinkSessionClosingControl.PROTOCOL_ERROR,
                    reason);
            } catch (RuntimeException sendFailure) {
                LOGGER.log(Level.FINE,
                    "STREAM protocol-error notification send failed: "
                        + streamNode.name() + ":" + routingId,
                    sendFailure);
            }
            if (messageTooLarge) {
                try {
                    stream.disconnectPeer(routingId);
                } catch (RuntimeException disconnectFailure) {
                    LOGGER.log(Level.FINE,
                        "STREAM EMSGSIZE disconnect failed: "
                            + streamNode.name() + ":" + routingId,
                        disconnectFailure);
                }
            }
            if (session != null) {
                recordSessionClosed(session, "protocol_error");
                try {
                    session.serials().executeInfrastructure(() -> executeHandler(() ->
                        transportErrorDisconnectSessionStage(
                            session,
                            nativeCode,
                            reason)));
                } catch (RuntimeException enqueueFailure) {
                    LOGGER.log(Level.FINE,
                        "STREAM protocol-error session cleanup failed: "
                            + streamNode.name() + ":" + routingId,
                        enqueueFailure);
                }
            }
            LOGGER.log(Level.WARNING,
                "STREAM peer isolated after " + reason + ": "
                    + streamNode.name() + ":" + routingId,
                failure);
        }

        private boolean isClosed() {
            return inReceiveStateLane(() -> closed);
        }

        private <T> T inReceiveStateLane(Supplier<T> work) {
            try {
                return receiveStateLane.runAsync(work).toCompletableFuture().join();
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

    }

    private CompletionStage<Void> dispatchToSession(
        StreamNodeRegistration streamNode,
        RoutingId routingId,
        ZLinkStreamHeader streamHeader,
        Message payload) {
        ZLinkBackendStreamSocket stream = streamsByName.get(streamNode.name());
        //  Spec 27 §4: at Off the inbound flow pair is neither read into a flow
        //  context nor copied forward; at every other level the ingress installs
        //  the inbound pair or starts a new flow.
        ZLinkFlowContext.State capturedFlow = null;
        if (streamHeader.kind() != ZLinkStreamMessageKind.CONTROL) {
            if (flow.captureEnabled()) {
                if (streamHeader.flowId().isPresent()
                    && !ZLinkFlowContext.isValidFlowId(
                        streamHeader.flowId().orElseThrow())) {
                    throw new IllegalArgumentException(
                        "STREAM header flow id must be UUIDv7");
                }
                capturedFlow = streamHeader.flowId().isPresent()
                    ? new ZLinkFlowContext.State(streamHeader.flowId().orElseThrow(),
                        streamHeader.flowOrigin().orElseThrow())
                    : ZLinkFlowContext.create(ZLinkFlowOrigin.INBOUND);
                if (streamHeader.flowId().isEmpty()) {
                    streamHeader =
                        streamHeader.withFlow(capturedFlow.flowId(), capturedFlow.origin());
                }
            } else if (streamHeader.flowId().isPresent()) {
                //  Off drops the inbound pair so no downstream consumer
                //  (dispatch state, reply headers, relays) copies it forward.
                streamHeader = streamHeader.withFlow(null, null);
            }
        }
        final ZLinkFlowContext.State incomingFlow = capturedFlow;
        final ZLinkStreamHeader dispatchHeader = streamHeader;
        trace(STREAM_TRACE ? "stream-node frame-received node=" + streamNode.name()
            + " routingId=" + routingId
            + " kind=" + streamHeader.kind()
            + " name=" + streamHeader.packetName()
            + " requestSeq=" + streamHeader.requestSequence().orElse(null)
            + " correlation=" + streamHeader.correlationId().orElse(null)
            + " payloadBytes=" + payload.size() : null);
        if (streamHeader.kind() == ZLinkStreamMessageKind.CONTROL) {
            dispatchControl(streamNode, stream, routingId, streamHeader, payload);
            return CompletableFuture.completedFuture(null);
        }
        if (draining) {
            sendSessionClosing(stream, routingId);
            return CompletableFuture.completedFuture(null);
        }
        SessionState state = getOrCreateSessionState(streamNode, stream, routingId);
        if (state.replacementClosing()) {
            sendSessionClosing(stream, routingId);
            return CompletableFuture.completedFuture(null);
        }
        state.markApplicationReceived();
        ZLinkMessageFlowTracer.TracePoint received =
            flow.begin(ZLinkMessageFlowOutcome.RECEIVED);
        if (received != null) {
            String corr = ZLinkStreamCorrelations.forTrace(dispatchHeader);
            received.trace(new ZLinkMessageFlowEvent(
                ZLinkMessageFlowOutcome.RECEIVED,
                ZLinkDispatchErrorSurface.STREAM_SESSION,
                dispatchHeader.requestSequence().isPresent()
                    ? ZLinkDispatchMessageKind.REQUEST
                    : ZLinkDispatchMessageKind.SEND,
                dispatchHeader.packetName(), null, null, corr, null, null, null, null,
                null, null, null, null,
                incomingFlow == null ? null : incomingFlow.flowId(),
                incomingFlow == null ? null : incomingFlow.origin()));
        }
        Message payloadCopy = Message.from(ZLinkStreamPayloadCodec.decode(
            dispatchHeader,
            payload,
            compressionCodec));
        ZLinkMessage sessionPayload = ZLinkMessage.fromEncoded(
            ZLinkMessagePayloads.encoded(payloadCopy),
            ZLinkCodecRegistration.serializerForReceivedStreamCodec(
                serializer, dispatchHeader.codec()));
        payloadCopy.close();
        trace(STREAM_TRACE ? "stream-node dispatch-enqueue node=" + streamNode.name()
            + " routingId=" + routingId
            + " name=" + dispatchHeader.packetName()
            + " requestSeq=" + dispatchHeader.requestSequence().orElse(null)
            + " correlation=" + dispatchHeader.correlationId().orElse(null) : null);
        CompletionStage<Void> completion = state.serials().executeApplication(
            () -> {
            traceStreamPhase(
                dispatchHeader, incomingFlow, ZLinkMessageFlowOutcome.ADMITTED);
            traceStreamPhase(
                dispatchHeader, incomingFlow, ZLinkMessageFlowOutcome.DISPATCHED);
            if (incomingFlow == null) {
                return executeHandler(() ->
                    state.context().dispatchStage(dispatchHeader, sessionPayload, state.session()));
            }
            try (ZLinkFlowContext.Scope ignored = ZLinkFlowContext.enter(incomingFlow)) {
                return executeHandler(() ->
                    state.context().dispatchStage(dispatchHeader, sessionPayload, state.session()));
            }
        });
        return completion;
    }

    private void traceStreamPhase(
        ZLinkStreamHeader header,
        ZLinkFlowContext.State incomingFlow,
        ZLinkMessageFlowOutcome phase) {
        ZLinkMessageFlowTracer.TracePoint tracePoint = flow.begin(phase);
        if (tracePoint == null) {
            return;
        }
        tracePoint.trace(new ZLinkMessageFlowEvent(
            phase,
            ZLinkDispatchErrorSurface.STREAM_SESSION,
            header.requestSequence().isPresent()
                ? ZLinkDispatchMessageKind.REQUEST
                : ZLinkDispatchMessageKind.SEND,
            header.packetName(), null, null,
            ZLinkStreamCorrelations.forTrace(header), null, null, null, null,
            null, null, null, null,
            incomingFlow == null ? null : incomingFlow.flowId(),
            incomingFlow == null ? null : incomingFlow.origin()));
    }

    private void dispatchControl(
        StreamNodeRegistration streamNode,
        ZLinkBackendStreamSocket stream,
        RoutingId routingId,
        ZLinkStreamHeader header,
        Message payload) {
        if (payload.size() != 0) {
            throw new IllegalArgumentException("STREAM control packet payload must be empty");
        }
        if (HEARTBEAT_PONG_NAME.equals(header.packetName())) {
            SessionState state;
            state = inStateLane(() -> sessions.get(sessionKey(streamNode, routingId)));
            if (state != null) {
                state.markHeartbeatPong();
            }
            return;
        }
        if (!HEARTBEAT_PING_NAME.equals(header.packetName())) {
            throw new IllegalArgumentException("unknown STREAM control packet: " + header.packetName());
        }
        Message empty = Message.from(new byte[0]);
            try {
                ZLinkStreamHeader pong = new ZLinkStreamHeader(
                ZLinkStreamMessageKind.CONTROL,
                ZLinkStreamCodec.RAW,
                EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
                Optional.empty(),
                HEARTBEAT_PONG_NAME,
                    Map.of(),
                    Optional.empty());
            if (!stream.send(routingId, pong, List.of(empty), SendFlags.DONT_WAIT)) {
                LOGGER.log(Level.FINE,
                    "STREAM heartbeat pong was not admitted by the transport: "
                        + routingId);
            }
        } finally {
            empty.close();
        }
    }

    private void dispatchStreamNotification(
        StreamNodeRegistration streamNode,
        ZLinkBackendStreamSocket stream,
        RoutingId routingId) {
        SessionState state = removeSessionState(streamNode, routingId);
        if (state == null) {
            if (draining) {
                sendSessionClosing(stream, routingId);
                return;
            }
            getOrCreateSessionState(streamNode, stream, routingId);
            return;
        }
        recordSessionClosed(state, "client_close");
        state.serials().executeInfrastructure(() -> executeHandler(() -> disconnectSessionStage(state)));
    }

    private SessionState removeSessionState(
        StreamNodeRegistration streamNode,
        RoutingId routingId) {
        return inStateLane(() -> sessions.remove(sessionKey(streamNode, routingId)));
    }

    private void reportTransportError(
        StreamNodeRegistration streamNode,
        RoutingId routingId,
        int nativeCode,
        String message) {
        receiveLoops.stream()
            .filter(loop -> loop.streamNode().equals(streamNode))
            .findFirst()
            .ifPresent(loop -> loop.removePeer(routingId));
        SessionState state = removeSessionState(streamNode, routingId);
        if (state == null) {
            return;
        }
        recordSessionClosed(
            state,
            nativeCode == 0 ? "transport_error" : "protocol_error");
        if (nativeCode == 0 && "DISCONNECTED".equals(message)) {
            state.serials().executeInfrastructure(() -> executeHandler(() -> disconnectSessionStage(state)));
            return;
        }
        state.serials().executeInfrastructure(() -> executeHandler(() ->
            transportErrorDisconnectSessionStage(state, nativeCode, message)));
    }

    private SessionState getOrCreateSessionState(
        StreamNodeRegistration streamNode,
        ZLinkBackendStreamSocket stream,
        RoutingId routingId) {
        String key = sessionKey(streamNode, routingId);
        SessionCreationClaim claim = inStateLane(() -> {
            SessionState existing = sessions.get(key);
            if (existing != null) {
                return SessionCreationClaim.existing(existing);
            }
            CompletableFuture<SessionState> pending = pendingSessionCreations.get(key);
            if (pending != null) {
                return SessionCreationClaim.pending(pending);
            }
            CompletableFuture<SessionState> created = new CompletableFuture<>();
            pendingSessionCreations.put(key, created);
            return SessionCreationClaim.creator(created);
        });
        if (claim.state() != null) {
            return claim.state();
        }
        if (!claim.creator()) {
            if (sessionCreationProducers.get().contains(claim.pending())) {
                throw new IllegalStateException(
                    "pending STREAM session creation reentered by its producer");
            }
            return claim.pending().join();
        }

        Set<CompletableFuture<SessionState>> producers =
            sessionCreationProducers.get();
        if (!producers.add(claim.pending())) {
            throw new IllegalStateException(
                "pending STREAM session creation producer was already active");
        }
        try {
            SessionState state;
            try {
                // User session construction runs outside the state turn. The claim
                // makes concurrent callers observe the same in-progress session,
                // just as they did while waiting for the former monitor.
                state = createSessionState(streamNode, stream, routingId);
            } catch (RuntimeException | Error failure) {
                inStateLane(() -> {
                    pendingSessionCreations.remove(key, claim.pending());
                    return null;
                });
                claim.pending().completeExceptionally(failure);
                throw failure;
            }
            SessionState completed = state;
            inStateLane(() -> {
                sessions.put(key, completed);
                pendingSessionCreations.remove(key, claim.pending());
                return null;
            });
            // CompletableFuture's dependents may be inline; signal after the lane
            // turn has returned so they cannot inherit its CURRENT ownership.
            claim.pending().complete(state);
            ZLinkRuntimeMetrics.add("zlink.stream.connections.active", 1, Map.of());
            ZLinkRuntimeMetrics.increment("zlink.stream.connections.opened", Map.of());
            dispatchConnected(state);
            return state;
        } finally {
            producers.remove(claim.pending());
            if (producers.isEmpty()) {
                sessionCreationProducers.remove();
            }
        }
    }

    private static void recordSessionClosed(SessionState state, String reason) {
        if (!state.closeMetricRecorded().compareAndSet(false, true)) {
            return;
        }
        ZLinkRuntimeMetrics.add("zlink.stream.connections.active", -1, Map.of());
        ZLinkRuntimeMetrics.increment("zlink.stream.connections.closed",
            Map.of("close_reason", reason));
    }

    private SessionState createSessionState(
        StreamNodeRegistration streamNode,
        ZLinkBackendStreamSocket stream,
        RoutingId routingId) {
        ZLinkSessionActorsRuntime sessionActors = actors == null
            && !streamSessionRelayAttached.getOrDefault(streamNode.name(), false)
                ? null
                : new ZLinkSessionActorsRuntime(
                    streamSessionRelaySpotNodes.get(streamNode.name()),
                    stream,
                    routingId,
                    actors,
                    serializer,
                    sessionRelayRouteReady,
                    localActorDispatcher,
                    streamSessionRelayAttached.getOrDefault(streamNode.name(), false),
                    defaultCodec,
                    flow,
                    sessionRelocationSealTimeout)
                    .metadataPolicy(
                        metadataPolicy.sessionToActorKeys(),
                        metadataPolicy.actorToSessionKeys());
        ZLinkInternalMeshNode ownerNode = actors == null
            ? null
            : meshNodes.get(actors.meshName());
        RoutingId ownerNodeRid = ownerNode == null
            ? null
            : ownerNode.routingId();
        long ownerNodeGeneration = ownerNode == null
            ? 0L
            : ownerNode.lifecycleGeneration();
        String ownerId = ownerNode == null
            ? null
            : ownerNode.localAuthorityOwnerId();
        long ownerLeaseGeneration = ownerNode == null
            ? 0L
            : ownerNode.localAuthorityLeaseGeneration();
        ZLinkStreamSessionContextState context = new ZLinkStreamSessionContextState(
            streamNode.name(),
            stream,
            routingId,
            sessionActors,
            serializer,
            defaultCodec,
            compressionCodec,
            flow,
            () -> {
                sendSessionClosing(stream, routingId);
                return CompletableFuture.completedFuture(null);
            },
            replyRetryExecutor);
        sessionContexts.add(context);
        ZLinkSessionPacketDispatcher<ZLinkSessionContext> dispatcher =
            new ZLinkSessionPacketDispatcherRuntime<>(
                streamNode.sessionPacketHandlers(),
                handlerFactory,
                serializer,
                handlerExecutor,
                suspendHandlerInvokers);
        ZLinkHandlerActivator.MutableServices sessionFactory =
            ZLinkHandlerActivator.services(handlerFactory)
                .add(ZLinkSessionContext.class, context)
                .add(ZLinkSessionPacketDispatcher.class, dispatcher);
        if (actors != null) {
            sessionFactory.add(ZLinkActorManager.class, actors);
        }
        Object createdSession = sessionFactory.create(streamNode.sessionType());
        if (!(createdSession instanceof ZLinkSession session)) {
            throw new ZLinkConfigurationException(
                "stream session type must implement ZLinkSession: "
                    + streamNode.sessionType().getName());
        }
        if (session.context() != context) {
            throw new ZLinkConfigurationException(
                "stream session must expose the context provided by the runtime: "
                    + streamNode.sessionType().getName());
        }
        ZLinkSessionSerialExecutor serials = new ZLinkSessionSerialExecutor(
            serialExecutor);
        return new SessionState(
            session,
            serials,
            context,
            stream,
            routingId,
            ownerNodeRid,
            ownerNodeGeneration,
            ownerId,
            ownerLeaseGeneration,
            sessionActors);
    }

    private void dispatchConnected(SessionState state) {
        state.serials().executeControl(() -> executeHandler(() ->
            ZLinkHandlerStages.fromStageSupplier(state.session()::onConnected)));
    }

    private static String sessionKey(StreamNodeRegistration streamNode, RoutingId routingId) {
        return streamNode.name() + ":" + routingId.toString();
    }

    @Override
    public void close() {
        closeAsync();
    }

    public CompletionStage<Void> closeAsync() {
        List<SessionState> activeSessions = inStateLane(
            () -> List.copyOf(sessions.values()));
        sessionContexts.forEach(ZLinkStreamSessionContextState::closeReplyRetries);
        return CompletableFuture.allOf(activeSessions.stream()
            .map(state -> state.serials().executeFinal(
                () -> executeHandler(() -> disconnectSessionStage(state)))
                .toCompletableFuture())
            .toArray(CompletableFuture[]::new))
            .handle((ignored, failure) -> {
                finishClose(activeSessions);
                return null;
            });
    }

    private void finishClose(List<SessionState> activeSessions) {
        inStateLane(() -> {
            sessions.clear();
            return null;
        });
        sessionContexts.forEach(ZLinkStreamSessionContextState::closeReplyRetries);
        String closeReason = draining ? "server_shutdown" : "transport_error";
        for (int index = 0; index < activeSessions.size(); index++) {
            recordSessionClosed(activeSessions.get(index), closeReason);
        }
        replyRetryExecutor.shutdownNow();
        boolean replyRetriesStopped = awaitExecutorTermination(
            replyRetryExecutor,
            "STREAM error reply retry executor");
        livenessExecutor.shutdownNow();
        boolean livenessStopped = awaitExecutorTermination(
            livenessExecutor,
            "STREAM liveness executor");
        receiveLoops.forEach(StreamReceiveLoop::close);
        receiveExecutor.shutdownNow();
        boolean receiveStopped = awaitExecutorTermination(
            receiveExecutor,
            "STREAM receive executor");
        if (!replyRetriesStopped || !livenessStopped || !receiveStopped) {
            LOGGER.severe(
                "STREAM runtime resources did not quiesce; native stream and context remain open");
            return;
        }
        for (ZLinkBackendStreamSocket stream : streams) {
            stream.close();
        }
        if (ownsContext) {
            context.close();
        }
    }

    private static boolean awaitExecutorTermination(
        ExecutorService executor,
        String description) {
        boolean interrupted = false;
        for (int attempt = 0; attempt < 2 && !executor.isTerminated(); attempt++) {
            try {
                if (executor.awaitTermination(5, TimeUnit.SECONDS)) {
                    break;
                }
            } catch (InterruptedException interruption) {
                interrupted = true;
                executor.shutdownNow();
            }
            executor.shutdownNow();
        }
        if (interrupted) {
            Thread.currentThread().interrupt();
        }
        if (!executor.isTerminated()) {
            LOGGER.warning(description + " did not terminate within the close deadline");
        }
        return executor.isTerminated();
    }

    public void beginDrain() {
        draining = true;
    }

    public CompletionStage<Void> awaitDrainBarrier() {
        List<SessionState> activeSessions = inStateLane(
            () -> List.copyOf(sessions.values()));
        CompletableFuture<?>[] barriers = activeSessions.stream()
            .map(state -> state.serials().executeFinal(
                () -> CompletableFuture.completedFuture(null)))
            .map(CompletionStage::toCompletableFuture)
            .toArray(CompletableFuture[]::new);
        return CompletableFuture.allOf(barriers);
    }

    public CompletionStage<Void> notifyServerDrain() {
        List<Map.Entry<String, SessionState>> active = inStateLane(
            () -> List.copyOf(sessions.entrySet()));
        for (Map.Entry<String, SessionState> entry : active) {
            SessionState state = entry.getValue();
            sendSessionClosing(state.stream(), state.routingId());
        }
        return CompletableFuture.completedFuture(null);
    }

    private static void sendSessionClosing(
        ZLinkBackendStreamSocket stream,
        RoutingId routingId) {
        sendSessionClosing(stream, routingId, ZLinkSessionClosingControl.SERVER_DRAIN, "server drain");
    }

    private static void sendSessionClosing(
        ZLinkBackendStreamSocket stream,
        RoutingId routingId,
        int reason,
        String diagnostic) {
        Message payload = Message.from(ZLinkSessionClosingControl.encode(reason, diagnostic));
        try {
            ZLinkStreamHeader header = new ZLinkStreamHeader(
                ZLinkStreamMessageKind.CONTROL,
                ZLinkStreamCodec.RAW,
                EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
                Optional.empty(),
                ZLinkSessionClosingControl.NAME,
                Map.of(),
                Optional.empty());
            if (!stream.send(routingId, header, List.of(payload), SendFlags.NONE)) {
                LOGGER.log(Level.FINE,
                    "STREAM session-closing control was not admitted by the transport: "
                        + routingId);
            }
        } catch (ZlinkSubmitException transportFailure) {
            LOGGER.log(Level.FINE,
                "STREAM session-closing control failed during transport teardown: "
                    + routingId,
                transportFailure);
        } finally {
            payload.close();
        }
    }

    private void checkSessionLiveness() {
        long now = System.nanoTime();
        List<Map.Entry<String, SessionState>> snapshot = inStateLane(
            () -> List.copyOf(sessions.entrySet()));
        for (Map.Entry<String, SessionState> entry : snapshot) {
            SessionState state = entry.getValue();
            int reason = now - state.lastHeartbeatPongNanos() >= HEARTBEAT_TIMEOUT_NANOS
                ? ZLinkSessionClosingControl.HEARTBEAT_TIMEOUT
                : now - state.lastApplicationNanos() >= IDLE_TIMEOUT_NANOS
                    ? ZLinkSessionClosingControl.IDLE_TIMEOUT
                    : 0;
            if (reason == 0) {
                sendHeartbeatPing(state);
                continue;
            }
            boolean removed = inStateLane(
                () -> sessions.remove(entry.getKey(), state));
            if (!removed) {
                continue;
            }
            sendSessionClosing(
                state.stream(), state.routingId(), reason,
                reason == ZLinkSessionClosingControl.HEARTBEAT_TIMEOUT
                    ? "heartbeat timeout"
                    : "idle timeout");
            recordSessionClosed(
                state,
                reason == ZLinkSessionClosingControl.HEARTBEAT_TIMEOUT
                    ? "heartbeat_timeout"
                    : "idle_timeout");
            state.serials().executeInfrastructure(() -> executeHandler(() -> disconnectSessionStage(state)));
        }
    }

    private static void sendHeartbeatPing(SessionState state) {
        Message empty = Message.from(new byte[0]);
        try {
            ZLinkStreamHeader ping = new ZLinkStreamHeader(
                ZLinkStreamMessageKind.CONTROL,
                ZLinkStreamCodec.RAW,
                EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
                Optional.empty(),
                HEARTBEAT_PING_NAME,
                Map.of(),
                Optional.empty());
            state.stream().send(
                state.routingId(), ping, List.of(empty), SendFlags.DONT_WAIT);
        } catch (ZlinkSubmitException transportFailure) {
            LOGGER.log(Level.FINE,
                "STREAM heartbeat ping failed during transport teardown: "
                    + state.routingId(),
                transportFailure);
        } finally {
            empty.close();
        }
    }

    private <T> CompletionStage<T> executeHandler(
        Supplier<CompletionStage<T>> operation) {
        CompletableFuture<CompletionStage<T>> entered = new CompletableFuture<>();
        ZLinkFlowContext.State capturedFlow = ZLinkFlowContext.current();
        var applicationJob = ZLinkApplicationJobContext.transferToQueuedJob();
        try {
            handlerExecutor.execute(() -> {
                try (ZLinkFlowContext.Scope ignored = capturedFlow == null
                    ? () -> { }
                    : ZLinkFlowContext.enter(capturedFlow);
                     var ignoredApplicationJob =
                         ZLinkApplicationJobContext.enterQueued(applicationJob)) {
                    ZLinkApplicationJobContext
                        .beforeFirstApplicationInstruction();
                    entered.complete(Objects.requireNonNull(
                        operation.get(), "handler result"));
                } catch (RuntimeException ex) {
                    entered.completeExceptionally(ex);
                } finally {
                    if (applicationJob != null) {
                        applicationJob.close();
                    }
                }
            });
        } catch (RuntimeException ex) {
            if (applicationJob != null) {
                applicationJob.close();
            }
            entered.completeExceptionally(ex);
        }
        // The session queue owns callback ordering. Keep its turn until the
        // handler stage reaches its terminal result so callbacks from one
        // STREAM session cannot overlap.
        return entered.thenCompose(Function.identity());
    }

    private CompletionStage<Void> disconnectSessionStage(SessionState state) {
        state.context().closeReplyRetries();
        return notifyBoundActorsDisconnectedBestEffort(state)
            .thenCompose(ignored -> ZLinkHandlerStages.fromStageSupplier(state.session()::onDisconnected))
            .whenComplete((ignored, failure) -> sessionContexts.remove(state.context()));
    }

    private CompletionStage<Void> transportErrorDisconnectSessionStage(
        SessionState state,
        int nativeCode,
        String message) {
        state.context().closeReplyRetries();
        return ZLinkHandlerStages.fromStageSupplier(() -> state.session().onError(new ZLinkStreamError(
            ZLinkStreamSessionError.TRANSPORT_ERROR,
            message)))
            .thenCompose(ignored -> notifyBoundActorsDisconnectedBestEffort(state))
            .thenCompose(ignored -> ZLinkHandlerStages.fromStageSupplier(state.session()::onDisconnected))
            .whenComplete((ignored, failure) -> sessionContexts.remove(state.context()));
    }

    private CompletionStage<Void> notifyBoundActorsDisconnectedBestEffort(SessionState state) {
        return state.context().notifyBoundActorsDisconnected(PHYSICAL_DISCONNECT_TIMEOUT)
            .handle((ignored, error) -> (Void) null);
    }

    private static final class SessionState {
        private final ZLinkSession session;
        private final ZLinkSessionSerialExecutor serials;
        private final ZLinkStreamSessionContextState context;
        private final ZLinkBackendStreamSocket stream;
        private final RoutingId routingId;
        private final RoutingId ownerNodeRid;
        private final long ownerNodeGeneration;
        private final String ownerId;
        private final long ownerLeaseGeneration;
        private final ZLinkSessionActorsRuntime actorRuntime;
        private final Set<ReplacementIdentity> replacements =
            ConcurrentHashMap.newKeySet();
        private final AtomicBoolean replacementClosing = new AtomicBoolean();
        private final AtomicBoolean closeScheduled = new AtomicBoolean();
        private final AtomicBoolean closeMetricRecorded = new AtomicBoolean();
        private volatile long lastApplicationNanos = System.nanoTime();
        private volatile long lastHeartbeatPongNanos = System.nanoTime();

        SessionState(
            ZLinkSession session,
            ZLinkSessionSerialExecutor serials,
            ZLinkStreamSessionContextState context,
            ZLinkBackendStreamSocket stream,
            RoutingId routingId,
            RoutingId ownerNodeRid,
            long ownerNodeGeneration,
            String ownerId,
            long ownerLeaseGeneration,
            ZLinkSessionActorsRuntime actorRuntime) {
            this.session = session;
            this.serials = serials;
            this.context = context;
            this.stream = stream;
            this.routingId = routingId;
            this.ownerNodeRid = ownerNodeRid;
            this.ownerNodeGeneration = ownerNodeGeneration;
            this.ownerId = ownerId;
            this.ownerLeaseGeneration = ownerLeaseGeneration;
            this.actorRuntime = actorRuntime;
        }

        ZLinkSession session() { return session; }
        ZLinkSessionSerialExecutor serials() { return serials; }
        ZLinkStreamSessionContextState context() { return context; }
        ZLinkBackendStreamSocket stream() { return stream; }
        RoutingId routingId() { return routingId; }
        ZLinkSessionActorsRuntime actorRuntime() { return actorRuntime; }
        boolean replacementClosing() { return replacementClosing.get(); }
        AtomicBoolean closeScheduled() { return closeScheduled; }

        boolean matchesOwner(
            systems.zlink.framework.runtime.internal.service
                .ZLinkServiceM6BWireCodec.RetiredSessionRouteFence retired) {
            // ownerNodeGeneration is a node lifecycle-generation opaque
            // equality token (.NET ulong, spec 01-glossary "Lifecycle
            // generation"): full range, only zero is unassigned. `> 0`
            // wrongly treats a legitimate negative-as-long value as unset.
            // ownerLeaseGeneration is spec-bounded to a positive `long`
            // ("OwnerLeaseGeneration"), so `> 0` is correct for it.
            return ownerNodeRid != null
                && ownerNodeRid.equals(retired.sessionOwnerNodeRid())
                && ownerNodeGeneration != 0
                && ownerNodeGeneration == retired.sessionOwnerNodeGeneration()
                && ownerId != null
                && ownerId.equals(retired.sessionOwnerId())
                && ownerLeaseGeneration > 0
                && ownerLeaseGeneration == retired.sessionOwnerLeaseGeneration();
        }

        boolean beginReplacement(ReplacementIdentity identity) {
            if (!replacements.add(identity)) {
                return false;
            }
            replacementClosing.set(true);
            return true;
        }

        boolean matchesReplacement(ReplacementIdentity identity) {
            return replacementClosing.get() && replacements.contains(identity);
        }
        long lastApplicationNanos() { return lastApplicationNanos; }
        long lastHeartbeatPongNanos() { return lastHeartbeatPongNanos; }
        AtomicBoolean closeMetricRecorded() { return closeMetricRecorded; }
        void markApplicationReceived() { lastApplicationNanos = System.nanoTime(); }
        void markHeartbeatPong() { lastHeartbeatPongNanos = System.nanoTime(); }
    }

    private record ReplacementIdentity(
        String actorId,
        RoutingId sessionRid,
        long retiredBindingGeneration) {
    }

    private record SessionCreationClaim(
        SessionState state,
        CompletableFuture<SessionState> pending,
        boolean creator) {
        static SessionCreationClaim existing(SessionState state) {
            return new SessionCreationClaim(state, null, false);
        }

        static SessionCreationClaim pending(CompletableFuture<SessionState> pending) {
            return new SessionCreationClaim(null, pending, false);
        }

        static SessionCreationClaim creator(CompletableFuture<SessionState> pending) {
            return new SessionCreationClaim(null, pending, true);
        }
    }

    private static ZLinkStreamCodec defaultCodec(ZLinkFrameworkRegistration registration) {
        return registration.codecs().streamCodecForCustomSerializer()
            .orElse(ZLinkStreamCodec.JSON);
    }

    static void trace(String message) {
        if (STREAM_TRACE) {
            LOGGER.fine("[zlink-java-stream-trace] " + message);
        }
    }

    static boolean traceEnabled() {
        return STREAM_TRACE;
    }
}
