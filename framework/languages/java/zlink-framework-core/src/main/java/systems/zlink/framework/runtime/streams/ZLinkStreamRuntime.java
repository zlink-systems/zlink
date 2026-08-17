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
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue;
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
    private static final Duration BOUND_SESSION_REPLACEMENT_DEADLINE =
        Duration.ofSeconds(5);
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
    private final List<ZLinkBackendStreamSocket> streams = new ArrayList<>();
    private final Map<String, ZLinkBackendStreamSocket> streamsByName = new HashMap<>();
    private final Map<String, Boolean> streamSessionRelayAttached = new HashMap<>();
    private final Map<String, ZLinkInternalSpotNode> streamSessionRelaySpotNodes = new HashMap<>();
    private final Map<String, SessionState> sessions = new HashMap<>();
    private final ScheduledExecutorService livenessExecutor;
    private final ScheduledExecutorService replyRetryExecutor;
    private final ExecutorService receiveExecutor;
    private final List<StreamReceiveLoop> receiveLoops = new ArrayList<>();
    private final Set<ZLinkStreamSessionContextState> sessionContexts =
        ConcurrentHashMap.newKeySet();
    private volatile boolean draining;

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
            stream.enableNotifications();
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
        List<SessionState> matches;
        synchronized (sessions) {
            matches = sessions.values().stream()
                .filter(state -> state.routingId().equals(
                    command.session().sessionRid()))
                .toList();
        }
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
        List<SessionState> matches;
        synchronized (sessions) {
            matches = sessions.values().stream()
                .filter(state -> state.routingId().equals(
                    command.session().sessionRid()))
                .toList();
        }
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
        List<? extends BoundSessionSendOwner> owners;
        synchronized (sessions) {
            owners = sessions.values().stream()
                .map(SessionState::actorRuntime)
                .filter(Objects::nonNull)
                .map(SessionBoundSessionSendOwner::new)
                .toList();
        }
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
            CompletionStage<Void> queued = state.queue().enqueueBarrierNext(() -> {
                ScheduledFuture<?> deadline = scheduleReplacementClose(
                    state,
                    identity,
                    BOUND_SESSION_REPLACEMENT_DEADLINE);
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
        synchronized (sessions) {
            return sessions.values().stream()
                .filter(state -> state.matchesOwner(retired)
                    && state.routingId().equals(retired.sessionRid()))
                .findFirst()
                .orElse(null);
        }
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
        synchronized (sessions) {
            return sessions.values().stream().anyMatch(current -> current == state);
        }
    }

    private final class StreamReceiveLoop implements AutoCloseable {
        private final StreamNodeRegistration streamNode;
        private final ZLinkBackendStreamSocket stream;
        private final Map<RoutingId, StreamReceiveState> receiveStates =
            new ConcurrentHashMap<>();
        private final Set<RoutingId> ignoredPeers =
            ConcurrentHashMap.newKeySet();
        private final Object receiveLock = new Object();
        private long receiveStateCursor;
        private boolean closed;
        private boolean capacitySignal;
        private AutoCloseable capacityRegistration;

        private StreamReceiveLoop(
            StreamNodeRegistration streamNode,
            ZLinkBackendStreamSocket stream) {
            this.streamNode = streamNode;
            this.stream = stream;
        }

        private void start() {
            try {
                receiveExecutor.execute(this::runLoop);
            } catch (RuntimeException rejected) {
                try {
                    capacityRegistration.close();
                } catch (Exception ignored) {
                }
                capacityRegistration = null;
                throw rejected;
            }
        }

        private StreamNodeRegistration streamNode() {
            return streamNode;
        }

        private void removePeer(RoutingId routingId) {
            ignoredPeers.add(routingId);
            StreamReceiveState state = receiveStates.remove(routingId);
            if (state != null) {
                state.close();
            }
        }

        @Override
        public void close() {
            AutoCloseable registration;
            List<StreamReceiveState> states;
            synchronized (receiveLock) {
                if (closed) {
                    return;
                }
                closed = true;
                capacitySignal = true;
                receiveLock.notifyAll();
                registration = capacityRegistration;
                capacityRegistration = null;
                states = List.copyOf(receiveStates.values());
                receiveStates.clear();
                ignoredPeers.clear();
            }
            if (registration != null) {
                try {
                    registration.close();
                } catch (Exception ignored) {
                }
            }
            states.forEach(StreamReceiveState::close);
        }

        private void runLoop() {
            while (!isClosed()) {
                try {
                    ZLinkReceiveBatchBudget dispatchBatch =
                        new ZLinkReceiveBatchBudget();
                    if (!flushReceiveStates(dispatchBatch)) {
                        if (!awaitCapacity()) {
                            return;
                        }
                        continue;
                    }
                    if (!dispatchBatch.canReceiveNext()) {
                        continue;
                    }
                    if (!stream.waitForReadable(RECEIVE_POLL_TIMEOUT)) {
                        continue;
                    }
                    if (isClosed()) {
                        return;
                    }
                    ZLinkReceiveBatchBudget transportBatch =
                        new ZLinkReceiveBatchBudget();
                    while (transportBatch.canReceiveNext()
                        && dispatchBatch.canReceiveNext()) {
                        if (transportBatch.messageCount() > 0
                            && !stream.waitForReadable(Duration.ZERO)) {
                            break;
                        }
                        ZLinkBackendStreamReceived received = stream.recv();
                        if (received == null) {
                            break;
                        }
                        transportBatch.record(ZLinkReceiveBatchBudget.bytesOf(
                            received.parts()));
                        boolean transferred = false;
                        try {
                            transferred = processReceived(received, dispatchBatch);
                        } finally {
                            if (!transferred) {
                                received.close();
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

        private boolean awaitCapacity() {
            return !isClosed();
        }

        private boolean flushReceiveStates(ZLinkReceiveBatchBudget batch) {
            List<StreamReceiveState> snapshot =
                new ArrayList<>(receiveStates.values());
            snapshot.sort((left, right) ->
                left.routingId().toString().compareTo(
                    right.routingId().toString()));
            if (snapshot.isEmpty()) {
                return true;
            }
            int cursor = (int) Math.floorMod(
                receiveStateCursor, snapshot.size());
            for (int offset = 0;
                offset < snapshot.size() && batch.canReceiveNext();
                offset++) {
                StreamReceiveState state = snapshot.get(cursor);
                cursor = (cursor + 1) % snapshot.size();
                receiveStateCursor = cursor;
                try {
                    if (state.closed()) {
                        continue;
                    }
                    if (!drainReceiveState(state, batch)) {
                        return false;
                    }
                } catch (RuntimeException | Error failure) {
                    isolatePeer(state.routingId(), failure);
                }
            }
            return true;
        }

        private boolean processReceived(
            ZLinkBackendStreamReceived received,
            ZLinkReceiveBatchBudget batch) {
            RoutingId routingId = received.routingId().orElse(null);
            if (routingId == null) {
                LOGGER.warning("STREAM receive did not provide a source routing id or part: "
                    + streamNode.name());
                return false;
            }
            List<Message> parts = received.parts();
            if (isClosed()) {
                return false;
            }
            if (parts.isEmpty()) {
                isolatePeer(
                    routingId,
                    new IllegalArgumentException(
                        "STREAM receive did not provide a message part"));
                return false;
            }
            if (parts.size() == 1
                && parts.getFirst().size() == 0
                && !parts.getFirst().more()) {
                if (ignoredPeers.remove(routingId)) {
                    trace(STREAM_TRACE ? "stream-node ignored quarantined peer notification node="
                        + streamNode.name() + " routingId=" + routingId : null);
                    return false;
                }
                try {
                    handleNotification(routingId);
                } catch (RuntimeException | Error failure) {
                    isolatePeer(routingId, failure);
                }
                return false;
            }
            if (ignoredPeers.contains(routingId)) {
                return false;
            }
            StreamReceiveState state = receiveStates.computeIfAbsent(
                routingId,
                ignored -> new StreamReceiveState(routingId));
            boolean transferred = false;
            try {
                if (!state.append(parts, received)) {
                    return false;
                }
                transferred = true;
                drainReceiveState(state, batch);
            } catch (RuntimeException | Error failure) {
                isolatePeer(routingId, failure);
            }
            return transferred;
        }

        private boolean drainReceiveState(
            StreamReceiveState state,
            ZLinkReceiveBatchBudget batch) {
            while (true) {
                if (!batch.canReceiveNext()) {
                    return true;
                }
                ZLinkStreamInboundFrame frame = state.claimFrame();
                if (frame == null) {
                    return true;
                }
                long bytes = frame.header().size()
                    + (long) frame.payload().size();
                if (!tryAdmitFrame(state.routingId(), frame)) {
                    state.retain(frame);
                    return false;
                }
                batch.record(bytes);
            }
        }

        private boolean tryAdmitFrame(
            RoutingId routingId,
            ZLinkStreamInboundFrame frame) {
            try {
                if (isClosed()) {
                    frame.close();
                    return true;
                }
                ZLinkStreamHeader header = ZLinkStreamHeaderCodec.decodeOrPlain(
                    frame.header().toByteArray());
                if (header.kind() != ZLinkStreamMessageKind.CONTROL) {
                    if (draining) {
                        sendSessionClosing(stream, routingId);
                        frame.close();
                        return true;
                    }
                }
                boolean ordinaryApplication =
                    header.kind() == ZLinkStreamMessageKind.SEND
                        || header.kind() == ZLinkStreamMessageKind.REQUEST;
                systems.zlink.framework.runtime.internal.dispatch
                    .ZLinkApplicationJobQueue.Permit permit = null;
                if (ordinaryApplication) {
                    try {
                        permit = applicationJobQueue.acquireBlocking();
                    } catch (InterruptedException interrupted) {
                        Thread.currentThread().interrupt();
                        frame.close();
                        return true;
                    }
                }
                try (var ignored = permit == null
                    ? (systems.zlink.framework.runtime.internal.dispatch
                        .ZLinkApplicationJobContext.Scope) () -> { }
                    : systems.zlink.framework.runtime.internal.dispatch
                        .ZLinkApplicationJobContext.enter(permit)) {
                    dispatchToSession(
                        streamNode,
                        routingId,
                        header,
                        frame.payload()).whenComplete(
                            (ignoredResult, error) -> frame.close());
                } finally {
                    if (permit != null) {
                        permit.abandonReservation();
                    }
                }
                return true;
            } catch (RuntimeException | Error failure) {
                frame.close();
                throw failure;
            }
        }

        private void handleNotification(RoutingId routingId) {
            StreamReceiveState state = receiveStates.remove(routingId);
            if (state != null) {
                state.close();
            }
            ZLinkBackendStreamSocket currentStream = stream;
            trace(STREAM_TRACE ? "stream-node notification node=" + streamNode.name()
                + " routingId=" + routingId : null);
            dispatchStreamNotification(streamNode, currentStream, routingId);
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
            ignoredPeers.add(routingId);
            StreamReceiveState state = receiveStates.remove(routingId);
            if (state != null) {
                state.close();
            }
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
                recordSessionClosed("protocol_error");
                try {
                    session.queue().enqueue(() -> executeHandler(() ->
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
            synchronized (receiveLock) {
                return closed;
            }
        }

        private final class StreamReceiveState implements AutoCloseable {
            private final RoutingId routingId;
            private final ZLinkStreamReceiveBuffer buffer =
                new ZLinkStreamReceiveBuffer(
                    streamNode.socketConfig().maxMessageSize());
            private ZLinkStreamInboundFrame pending;
            private boolean closed;

            private StreamReceiveState(RoutingId routingId) {
                this.routingId = routingId;
            }

            private RoutingId routingId() {
                return routingId;
            }

            private synchronized boolean closed() {
                return closed;
            }

            private synchronized boolean append(
                List<Message> parts,
                ZLinkBackendStreamReceived received) {
                if (closed) {
                    return false;
                }
                buffer.append(parts, received);
                return true;
            }

            private synchronized ZLinkStreamInboundFrame claimFrame() {
                if (closed) {
                    return null;
                }
                if (pending != null) {
                    ZLinkStreamInboundFrame claimed = pending;
                    pending = null;
                    return claimed;
                }
                return buffer.tryTakeFrame();
            }

            private synchronized void retain(ZLinkStreamInboundFrame frame) {
                if (closed) {
                    frame.close();
                    return;
                }
                if (pending != null) {
                    frame.close();
                    throw new IllegalStateException(
                        "STREAM peer already has a pending frame");
                }
                pending = frame;
            }

            @Override
            public synchronized void close() {
                if (closed) {
                    return;
                }
                closed = true;
                if (pending != null) {
                    pending.close();
                    pending = null;
                }
                buffer.close();
            }
        }

    }

    private CompletionStage<Void> dispatchToSession(
        StreamNodeRegistration streamNode,
        RoutingId routingId,
        ZLinkStreamHeader streamHeader,
        Message payload) {
        ZLinkBackendStreamSocket stream = streamsByName.get(streamNode.name());
        ZLinkFlowContext.State incomingFlow = streamHeader.flowId().isPresent()
            ? new ZLinkFlowContext.State(streamHeader.flowId().orElseThrow(),
                streamHeader.flowOrigin().orElseThrow())
            : (flow.enabled(ZLinkMessageFlowOutcome.RECEIVED)
                ? ZLinkFlowContext.create(ZLinkFlowOrigin.INBOUND)
                : null);
        if (streamHeader.kind() != ZLinkStreamMessageKind.CONTROL
            && incomingFlow != null
            && streamHeader.flowId().isEmpty()) {
            streamHeader = streamHeader.withFlow(incomingFlow.flowId(), incomingFlow.origin());
        }
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
        CompletionStage<Void> completion = state.queue().enqueue(
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
            synchronized (sessions) {
                state = sessions.get(sessionKey(streamNode, routingId));
            }
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
        recordSessionClosed("client_close");
        state.queue().enqueue(() -> executeHandler(() -> disconnectSessionStage(state)));
    }

    private SessionState removeSessionState(
        StreamNodeRegistration streamNode,
        RoutingId routingId) {
        synchronized (sessions) {
            return sessions.remove(sessionKey(streamNode, routingId));
        }
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
        recordSessionClosed(nativeCode == 0 ? "transport_error" : "protocol_error");
        if (nativeCode == 0 && "DISCONNECTED".equals(message)) {
            state.queue().enqueue(() -> executeHandler(() -> disconnectSessionStage(state)));
            return;
        }
        state.queue().enqueue(() -> executeHandler(() ->
            transportErrorDisconnectSessionStage(state, nativeCode, message)));
    }

    private SessionState getOrCreateSessionState(
        StreamNodeRegistration streamNode,
        ZLinkBackendStreamSocket stream,
        RoutingId routingId) {
        String key = sessionKey(streamNode, routingId);
        SessionState state;
        boolean created = false;
        synchronized (sessions) {
            state = sessions.get(key);
            if (state == null) {
                state = createSessionState(streamNode, stream, routingId);
                sessions.put(key, state);
                created = true;
            }
        }
        if (created) {
            ZLinkRuntimeMetrics.add("zlink.stream.connections.active", 1, Map.of());
            ZLinkRuntimeMetrics.increment("zlink.stream.connections.opened", Map.of());
            dispatchConnected(state);
        }
        return state;
    }

    private static void recordSessionClosed(String reason) {
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
        ZLinkAsyncSerialQueue queue = new ZLinkAsyncSerialQueue(
            serialExecutor, false);
        return new SessionState(
            session,
            queue,
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
        state.queue().enqueue(() -> executeHandler(() ->
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
        List<SessionState> activeSessions;
        synchronized (sessions) {
            activeSessions = List.copyOf(sessions.values());
        }
        sessionContexts.forEach(ZLinkStreamSessionContextState::closeReplyRetries);
        return CompletableFuture.allOf(activeSessions.stream()
            .map(state -> state.queue().enqueue(
                () -> executeHandler(() -> disconnectSessionStage(state)))
                .toCompletableFuture())
            .toArray(CompletableFuture[]::new))
            .handle((ignored, failure) -> {
                finishClose(activeSessions);
                return null;
            });
    }

    private void finishClose(List<SessionState> activeSessions) {
        synchronized (sessions) {
            sessions.clear();
        }
        sessionContexts.forEach(ZLinkStreamSessionContextState::closeReplyRetries);
        String closeReason = draining ? "server_drain" : "transport_error";
        for (int index = 0; index < activeSessions.size(); index++) {
            recordSessionClosed(closeReason);
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
        List<SessionState> activeSessions;
        synchronized (sessions) {
            activeSessions = List.copyOf(sessions.values());
        }
        CompletableFuture<?>[] barriers = activeSessions.stream()
            .map(state -> state.queue().enqueue(
                () -> CompletableFuture.completedFuture(null)))
            .map(CompletionStage::toCompletableFuture)
            .toArray(CompletableFuture[]::new);
        return CompletableFuture.allOf(barriers);
    }

    public CompletionStage<Void> notifyServerDrain() {
        List<Map.Entry<String, SessionState>> active;
        synchronized (sessions) {
            active = List.copyOf(sessions.entrySet());
        }
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
        } finally {
            payload.close();
        }
    }

    private void checkSessionLiveness() {
        long now = System.nanoTime();
        List<Map.Entry<String, SessionState>> snapshot;
        synchronized (sessions) {
            snapshot = List.copyOf(sessions.entrySet());
        }
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
            synchronized (sessions) {
                if (!sessions.remove(entry.getKey(), state)) {
                    continue;
                }
            }
            sendSessionClosing(
                state.stream(), state.routingId(), reason,
                reason == ZLinkSessionClosingControl.HEARTBEAT_TIMEOUT
                    ? "heartbeat timeout"
                    : "idle timeout");
            state.queue().enqueue(() -> executeHandler(() -> disconnectSessionStage(state)));
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
            state.stream().send(state.routingId(), ping, List.of(empty), SendFlags.DONT_WAIT);
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
        private final ZLinkAsyncSerialQueue queue;
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
        private volatile long lastApplicationNanos = System.nanoTime();
        private volatile long lastHeartbeatPongNanos = System.nanoTime();

        SessionState(
            ZLinkSession session,
            ZLinkAsyncSerialQueue queue,
            ZLinkStreamSessionContextState context,
            ZLinkBackendStreamSocket stream,
            RoutingId routingId,
            RoutingId ownerNodeRid,
            long ownerNodeGeneration,
            String ownerId,
            long ownerLeaseGeneration,
            ZLinkSessionActorsRuntime actorRuntime) {
            this.session = session;
            this.queue = queue;
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
        ZLinkAsyncSerialQueue queue() { return queue; }
        ZLinkStreamSessionContextState context() { return context; }
        ZLinkBackendStreamSocket stream() { return stream; }
        RoutingId routingId() { return routingId; }
        ZLinkSessionActorsRuntime actorRuntime() { return actorRuntime; }
        boolean replacementClosing() { return replacementClosing.get(); }
        AtomicBoolean closeScheduled() { return closeScheduled; }

        boolean matchesOwner(
            systems.zlink.framework.runtime.internal.service
                .ZLinkServiceM6BWireCodec.RetiredSessionRouteFence retired) {
            return ownerNodeRid != null
                && ownerNodeRid.equals(retired.sessionOwnerNodeRid())
                && ownerNodeGeneration > 0
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
        void markApplicationReceived() { lastApplicationNanos = System.nanoTime(); }
        void markHeartbeatPong() { lastHeartbeatPongNanos = System.nanoTime(); }
    }

    private record ReplacementIdentity(
        String actorId,
        RoutingId sessionRid,
        long retiredBindingGeneration) {
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
