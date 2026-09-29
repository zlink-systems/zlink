package systems.zlink.framework.runtime.streams;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;
import systems.zlink.framework.runtime.actors.ZLinkActorRuntime;
import systems.zlink.framework.runtime.actors.ZLinkSessionActorsRuntime;
import systems.zlink.framework.runtime.configuration.ZLinkFrameworkRegistration;
import systems.zlink.framework.runtime.configuration.ZLinkMetadataPolicyRegistration;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerStages;
import systems.zlink.framework.runtime.internal.backend.*;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.configuration.ZLinkCodecRegistration;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorReason;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorSurface;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchMessageKind;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowEvent;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowOutcome;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkReceiveBatchBudget;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationAdapter;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkRuntimeEventDispatcher;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.messaging.ZLinkMessagePayloads;
import systems.zlink.framework.runtime.spots.ZLinkSpotRuntime;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionPacketDispatcher;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamCompressionCodec;
import systems.zlink.framework.streams.ZLinkStreamError;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;
import systems.zlink.framework.streams.ZLinkStreamSessionError;

import java.time.Duration;
import java.util.ArrayList;
import java.util.EnumSet;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.Set;
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
import java.util.concurrent.atomic.AtomicLong;
import java.util.function.BiFunction;
import java.util.function.Function;
import java.util.function.Predicate;
import java.util.function.Supplier;
import java.util.logging.Level;
import java.util.logging.Logger;

public final class ZLinkStreamRuntime implements AutoCloseable {
    private static final Duration PHYSICAL_DISCONNECT_TIMEOUT = Duration.ofSeconds(5);
    private static final Logger LOGGER = Logger.getLogger(ZLinkStreamRuntime.class.getName());
    private static final String HEARTBEAT_PING_NAME = "$zlink.heartbeat.ping";
    private static final String HEARTBEAT_PONG_NAME = "$zlink.heartbeat.pong";
    private static final long HEARTBEAT_TIMEOUT_NANOS = TimeUnit.SECONDS.toNanos(5);
    private static final long IDLE_TIMEOUT_NANOS = TimeUnit.SECONDS.toNanos(30);
    private static final Duration BOUND_SESSION_REPLACEMENT_CLOSE_DELAY = Duration.ofMillis(100);
    private static final Duration RECEIVE_POLL_TIMEOUT = Duration.ofMillis(250);
    private final ZLinkBackendContext context;
    private final boolean ownsContext;
    private final ZLinkFrameworkRegistration registration;
    private final systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue
            applicationJobQueue;
    private final ZLinkMessageSerializer serializer;
    private final ZLinkActorRuntime actors;
    private final Map<String, ZLinkInternalMeshNode> meshNodes;
    private final Map<String, ZLinkInternalSpotNode> spotNodes;
    private final ZLinkStreamBackendAdapter streamAdapter;
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
    // One binding generation counter for every Session this node owns when no
    // native Session relay allocates it (Session-Actor binding §4).
    private final AtomicLong sessionBindingGenerations = new AtomicLong();
    private final Map<String, ZLinkInternalSpotNode> streamSessionRelaySpotNodes = new HashMap<>();
    private final Map<String, SessionState> sessions = new HashMap<>();
    // A Session whose constructor has not finished. Its queue already exists, so
    // packets admitted while it is pending wait behind the constructor in order.
    private final Map<String, PendingSession> pendingSessionCreations = new HashMap<>();
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
            var turn = stateLane.runNowOrQueue(work).toCompletableFuture();
            // An idle lane ran the turn on this thread; only a pending turn is a wait.
            assert turn.isDone() || ZLinkStateLane.assertMayBlock();
            return turn.join();
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
                (ignoredBackend, ignoredKey) ->
                        (ignoredSubmission, ignoredCleanup) ->
                                CompletableFuture.failedFuture(
                                        new IllegalStateException(
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
                            BiFunction<Supplier<Boolean>, Runnable, CompletionStage<Void>>>
                    admission) {
        if (registration.streamNodes().isEmpty()) {
            throw new ZLinkConfigurationException("at least one stream node is required");
        }
        this.registration = Objects.requireNonNull(registration, "registration");
        this.sessionRelocationSealTimeout =
                registration.locations().options().sessionRelocationSealTimeout();
        this.sessionReplacementCallbackTimeout = registration.sessionReplacementCallbackTimeout();
        this.applicationJobQueue = registration.applicationJobQueue();
        this.serializer = serializer;
        this.actors = actors;
        this.meshNodes = Map.copyOf(meshNodes);
        this.handlerFactory = handlerFactory;
        this.handlerExecutor =
                ZLinkFlowContext.propagating(
                        Objects.requireNonNull(registration.handlerExecutor(), "handlerExecutor"));
        this.serialExecutor = registration.serialExecutor();
        this.flow =
                new ZLinkMessageFlowTracer(
                        registration.dispatchOptions(),
                        handlerFactory,
                        this.handlerExecutor,
                        eventDispatcher);
        this.suspendHandlerInvokers = registration.suspendHandlerInvokers();
        this.defaultCodec = defaultCodec(registration);
        this.compressionCodec = registration.streamCompressionCodec();
        this.sessionRelayRouteReady =
                sessionRelayRouteReady == null ? ignored -> true : sessionRelayRouteReady;
        this.localActorDispatcher = spots == null ? null : spots::dispatchLocalSessionActor;
        this.metadataPolicy = registration.metadataPolicy();
        this.livenessExecutor =
                Executors.newSingleThreadScheduledExecutor(
                        task -> {
                            Thread thread = new Thread(task, "zlink-stream-liveness");
                            thread.setDaemon(true);
                            return thread;
                        });
        this.replyRetryExecutor =
                Executors.newSingleThreadScheduledExecutor(
                        task -> {
                            Thread thread = new Thread(task, "zlink-stream-reply-retry");
                            thread.setDaemon(true);
                            return thread;
                        });
        this.receiveExecutor =
                Executors.newFixedThreadPool(
                        Math.max(1, registration.streamNodes().size()),
                        task -> {
                            Thread thread = new Thread(task, "zlink-stream-recv");
                            thread.setDaemon(true);
                            return thread;
                        });
        this.streamAdapter = backendFactory.createStreamAdapter(adapterOptions);
        this.spotNodes = spotNodes;
        this.context = Objects.requireNonNull(context, "context");
        this.ownsContext = ownsContext;
    }

    /**
     * Opens each STREAM node and starts receiving.
     *
     * <p>The runtime is already recorded by its owner when this runs, and each socket is recorded
     * in {@code streams} the moment it exists. A failure here leaves a partial set of sockets that
     * the owner's close ({@link #closeAsync()}) releases like any other.
     */
    public ZLinkStreamRuntime start() {
        for (StreamNodeRegistration streamNode : registration.streamNodes()) {
            String actorMeshName =
                    streamNode.actorDispatchEnabled() ? resolveActorDispatchMeshName() : null;
            ZLinkInternalMeshNode meshNode =
                    actorMeshName == null ? null : meshNodes.get(actorMeshName);
            ZLinkBackendStreamSocket stream = streamAdapter.createStreamSocket(context, meshNode);
            streams.add(stream);
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
                stream.bind(bindEndpoint);
            }
            stream.onTransportError(
                    (routingId, nativeCode, message) ->
                            reportTransportError(streamNode, routingId, nativeCode, message));
            stream.startSessionService();
            ZLinkInternalSpotNode spotNode = resolveSessionRelayNode(spotNodes);
            streamsByName.put(streamNode.name(), stream);
            streamSessionRelayAttached.put(streamNode.name(), spotNode != null);
            if (spotNode != null) {
                streamSessionRelaySpotNodes.put(streamNode.name(), spotNode);
            }
            receiveLoops.add(new StreamReceiveLoop(streamNode, stream));
        }
        receiveLoops.forEach(StreamReceiveLoop::start);
        livenessExecutor.scheduleAtFixedRate(this::checkSessionLiveness, 1L, 1L, TimeUnit.SECONDS);
        return this;
    }

    private String resolveActorDispatchMeshName() {
        if (actors == null || actors.meshName() == null || actors.meshName().isBlank()) {
            throw new ZLinkConfigurationException(
                    "stream actor dispatch requires a configured actor authority mesh");
        }
        if (!meshNodes.containsKey(actors.meshName())) {
            throw new ZLinkConfigurationException(
                    "stream actor dispatch authority mesh is not configured: " + actors.meshName());
        }
        return actors.meshName();
    }

    private static ZLinkInternalSpotNode resolveSessionRelayNode(
            Map<String, ZLinkInternalSpotNode> spotNodes) {
        return spotNodes.values().stream().findFirst().orElse(null);
    }

    public ZLinkSessionActorsRuntime sessionActors(
            String streamNodeName, RoutingId sessionRid, ZLinkActorRuntime actors) {
        ZLinkBackendStreamSocket stream = streamsByName.get(streamNodeName);
        if (stream == null) {
            throw new ZLinkConfigurationException("stream node is not running: " + streamNodeName);
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
                        sessionRelocationSealTimeout,
                        sessionBindingGenerations::incrementAndGet)
                .metadataPolicy(
                        metadataPolicy.sessionToActorKeys(), metadataPolicy.actorToSessionKeys());
    }

    /** Returns the advertised endpoint of a bound STREAM listener, or null when it has none. */
    public String boundListenerEndpoint(StreamNodeRegistration registration) {
        ZLinkBackendStreamSocket stream = streamsByName.get(registration.name());
        String actual = stream == null ? null : stream.lastEndpoint();
        if (actual == null || actual.isBlank()) {
            actual = registration.bindEndpoint();
        }
        if (actual == null || actual.isBlank() || actual.endsWith(":0")) {
            return null;
        }
        return registration.advertisedEndpoint(actual);
    }

    public CompletionStage<Void> handleSessionRelocationRoute(
            RoutingId transportSource, byte[] command44) {
        var codec = new systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec();
        var command = codec.decodeSessionRelocationRoute(command44);
        RoutingId expectedSource =
                command.action()
                                == systems.zlink.framework.runtime.internal.service
                                        .ZLinkServiceM6BWireCodec.SessionRelocationRouteAction
                                        .COMMIT
                        ? command.targetNodeRid()
                        : command.coordinator().nodeRid();
        if (!expectedSource.equals(transportSource)) {
            return CompletableFuture.failedFuture(
                    new ZLinkConfigurationException(
                            "command 44 transport source differs from its sender fence"));
        }
        List<SessionState> matches =
                inStateLane(
                        () ->
                                sessions.values().stream()
                                        .filter(
                                                state ->
                                                        state.routingId()
                                                                .equals(
                                                                        command.session()
                                                                                .sessionRid()))
                                        .toList());
        if (matches.size() != 1) {
            return CompletableFuture.failedFuture(
                    new ZLinkConfigurationException("command 44 requires one exact local Session"));
        }
        return matches.getFirst().context().applyRelocationRouteCommand(command);
    }

    /**
     * Command 42 endpoint. The relocation source addresses the seal to this Session owner; the
     * sender fence is the Actor owner node the binding still points at. The exact command 42
     * contract permits only the source role; command 43 echoes the seal fields without adding
     * completion state.
     */
    public CompletionStage<byte[]> handleSessionRelocationSeal(
            RoutingId transportSource, byte[] command42) {
        var codec = new systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec();
        var command = codec.decodeSessionRelocationSeal(command42);
        RoutingId expectedSource =
                command.senderRole()
                                == systems.zlink.framework.runtime.internal.service
                                        .ZLinkServiceM6BWireCodec.RelocationRole.SOURCE
                        ? command.actor().actor().nodeRid()
                        : command.coordinator().nodeRid();
        if (!expectedSource.equals(transportSource)) {
            return CompletableFuture.failedFuture(
                    new ZLinkConfigurationException(
                            "command 42 transport source differs from the sender fence"));
        }
        List<SessionState> matches =
                inStateLane(
                        () ->
                                sessions.values().stream()
                                        .filter(
                                                state ->
                                                        state.routingId()
                                                                .equals(
                                                                        command.session()
                                                                                .sessionRid()))
                                        .toList());
        if (matches.size() != 1) {
            return CompletableFuture.failedFuture(
                    new ZLinkConfigurationException("command 42 requires one exact local Session"));
        }
        return matches.getFirst()
                .context()
                .applyRelocationSealCommand(command)
                .thenApply(codec::encodeSessionRelocationSealed);
    }

    /**
     * Routes command 36 to the one Session whose current binding it names. The transport boundary
     * has already authenticated the source peer; the Session owner decides admission from the
     * binding identity alone (Session-Actor binding §3 item 3, §8.1).
     */
    public CompletionStage<Boolean> handleBoundSessionSend(
            RoutingId sourceNodeRid,
            long sourceNodeGeneration,
            ZLinkServiceM6BWireCodec.BoundSessionSend command,
            ZLinkServiceM6AWireCodec.ApplicationPayload payload) {
        return stateLane
                .<CompletionStage<Boolean>>runAsync(
                        () -> {
                            List<? extends BoundSessionSendOwner> owners =
                                    sessions.values().stream()
                                            .map(SessionState::actorRuntime)
                                            .filter(Objects::nonNull)
                                            .map(SessionBoundSessionSendOwner::new)
                                            .toList();
                            return dispatchBoundSessionSend(
                                    owners, flow, sourceNodeRid, command, payload);
                        })
                .thenCompose(Function.identity());
    }

    static CompletionStage<Boolean> dispatchBoundSessionSend(
            List<? extends BoundSessionSendOwner> owners,
            ZLinkMessageFlowTracer flow,
            RoutingId sourceNodeRid,
            ZLinkServiceM6BWireCodec.BoundSessionSend command,
            ZLinkServiceM6AWireCodec.ApplicationPayload payload) {
        List<? extends BoundSessionSendOwner> matches =
                owners.stream().filter(owner -> owner.matches(command)).toList();
        CompletionStage<Boolean> accepted =
                matches.size() == 1
                        ? matches.getFirst().acceptAsync(command, payload)
                        : CompletableFuture.completedFuture(false);
        return accepted.thenApply(
                admitted -> {
                    if (!admitted) {
                        traceRejectedBoundSessionSend(flow, sourceNodeRid, command, payload);
                    }
                    return admitted;
                });
    }

    //  Session-Actor binding §3 item 4: a push refused as not current is recorded with the
    //  closed message-flow vocabulary instead of disappearing.
    private static void traceRejectedBoundSessionSend(
            ZLinkMessageFlowTracer flow,
            RoutingId sourceNodeRid,
            ZLinkServiceM6BWireCodec.BoundSessionSend command,
            ZLinkServiceM6AWireCodec.ApplicationPayload payload) {
        ZLinkMessageFlowTracer.TracePoint tracePoint =
                flow == null ? null : flow.begin(ZLinkMessageFlowOutcome.DROPPED);
        if (tracePoint == null) {
            return;
        }
        tracePoint.trace(
                new ZLinkMessageFlowEvent(
                        ZLinkMessageFlowOutcome.DROPPED,
                        ZLinkDispatchErrorSurface.STREAM_SESSION,
                        ZLinkDispatchMessageKind.SEND,
                        payload.packetName(),
                        null,
                        null,
                        null,
                        sourceNodeRid.toString(),
                        null,
                        command.actor().actor().actorId(),
                        null,
                        ZLinkDispatchErrorReason.STALE_TARGET,
                        null,
                        null,
                        null,
                        null,
                        null));
    }

    interface BoundSessionSendOwner {
        boolean matches(ZLinkServiceM6BWireCodec.BoundSessionSend command);

        CompletionStage<Boolean> acceptAsync(
                ZLinkServiceM6BWireCodec.BoundSessionSend command,
                ZLinkServiceM6AWireCodec.ApplicationPayload payload);
    }

    private record SessionBoundSessionSendOwner(ZLinkSessionActorsRuntime runtime)
            implements BoundSessionSendOwner {
        @Override
        public boolean matches(ZLinkServiceM6BWireCodec.BoundSessionSend command) {
            return runtime.matchesBoundSessionSend(command);
        }

        @Override
        public CompletionStage<Boolean> acceptAsync(
                ZLinkServiceM6BWireCodec.BoundSessionSend command,
                ZLinkServiceM6AWireCodec.ApplicationPayload payload) {
            return runtime.acceptBoundSessionSendAsync(command, payload);
        }
    }

    /**
     * Handles the one-way boundSessionReplaced notice without putting it in a user application
     * mailbox. The callback is started on the Session's serial lane, while its completion and both
     * close timers are observed by the runtime scheduler so the lane is returned immediately.
     */
    public void handleBoundSessionReplaced(
            RoutingId transportSource,
            systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec
                            .BoundSessionReplaced
                    replacement) {
        Objects.requireNonNull(transportSource, "transportSource");
        Objects.requireNonNull(replacement, "replacement");
        var actor = replacement.actorAuthority().actor();
        var retired = replacement.retiredSession();
        if (!transportSource.equals(actor.nodeRid())) {
            return;
        }
        SessionState state = findSessionForReplacement(retired);
        if (state == null
                || state.actorRuntime() == null
                || state.actorRuntime()
                        .find(actor.actorId())
                        .filter(
                                bound ->
                                        bound.ref().objectGeneration() == actor.generation()
                                                && bound.ref().nodeRid().equals(actor.nodeRid()))
                        .isEmpty()) {
            return;
        }
        ReplacementIdentity identity =
                new ReplacementIdentity(
                        actor.actorId(), retired.sessionRid(), retired.retiredBindingGeneration());
        if (!state.beginReplacement(identity)) {
            return;
        }
        try {
            // Replacement is lifecycle work, not application work.  A
            // barrier keeps it ahead of application turns that were already
            // admitted while still preserving the active turn's completion
            // boundary.  New application ingress is closed above, so this
            // is the only queued lifecycle callback for this exact identity.
            CompletionStage<Void> queued =
                    state.serials()
                            .executeLifecycleNext(
                                    () -> {
                                        ScheduledFuture<?> deadline =
                                                scheduleReplacementClose(
                                                        state,
                                                        identity,
                                                        sessionReplacementCallbackTimeout);
                                        CompletionStage<Void> callback;
                                        try {
                                            callback =
                                                    executeHandler(
                                                            () ->
                                                                    ZLinkHandlerStages
                                                                            .fromStageSupplier(
                                                                                    () ->
                                                                                            state.session()
                                                                                                    .onActorBindingReplaced(
                                                                                                            actor
                                                                                                                    .actorId())));
                                        } catch (RuntimeException failure) {
                                            callback = CompletableFuture.failedFuture(failure);
                                        }
                                        ScheduledFuture<?> callbackDeadline = deadline;
                                        callback.whenComplete(
                                                (ignored, failure) -> {
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
            queued.whenComplete(
                    (ignored, failure) -> {
                        if (failure != null) {
                            scheduleReplacementClose(
                                    state, identity, BOUND_SESSION_REPLACEMENT_CLOSE_DELAY);
                        }
                    });
        } catch (RuntimeException rejected) {
            scheduleReplacementClose(state, identity, BOUND_SESSION_REPLACEMENT_CLOSE_DELAY);
        }
    }

    private SessionState findSessionForReplacement(
            systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec
                            .RetiredSessionRouteFence
                    retired) {
        return inStateLane(
                () ->
                        sessions.values().stream()
                                .filter(
                                        state ->
                                                state.matchesOwner(retired)
                                                        && state.routingId()
                                                                .equals(retired.sessionRid()))
                                .findFirst()
                                .orElse(null));
    }

    private ScheduledFuture<?> scheduleReplacementClose(
            SessionState state, ReplacementIdentity identity, Duration delay) {
        try {
            return replyRetryExecutor.schedule(
                    () -> {
                        closeReplacedSessionIfExact(state, identity);
                    },
                    delay.toNanos(),
                    TimeUnit.NANOSECONDS);
        } catch (RuntimeException rejected) {
            closeReplacedSessionIfExact(state, identity);
            return null;
        }
    }

    private void closeReplacedSessionIfExact(SessionState state, ReplacementIdentity identity) {
        if (!isCurrentSessionState(state)
                || !state.matchesReplacement(identity)
                || !state.closeScheduled().compareAndSet(false, true)) {
            return;
        }
        sendSessionClosing(state.stream(), state.routingId());
        try {
            state.stream().disconnectPeer(state.routingId());
        } catch (RuntimeException failure) {
            LOGGER.log(
                    Level.FINE,
                    "bound Session replacement transport disconnect failed: " + state.routingId(),
                    failure);
        }
    }

    private boolean isCurrentSessionState(SessionState state) {
        return inStateLane(() -> sessions.values().stream().anyMatch(current -> current == state));
    }

    private final class StreamReceiveLoop implements AutoCloseable {
        private final StreamNodeRegistration streamNode;
        private final ZLinkBackendStreamSocket stream;
        private volatile boolean closed;

        private StreamReceiveLoop(
                StreamNodeRegistration streamNode, ZLinkBackendStreamSocket stream) {
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
            closed = true;
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
                        systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue
                                        .Permit
                                permit;
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
                        LOGGER.log(
                                Level.WARNING,
                                "STREAM receive loop failed: " + streamNode.name(),
                                failure);
                        return;
                    }
                }
            }
        }

        private boolean processReceived(
                ZLinkBackendStreamReceived received,
                systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue.Permit
                        permit,
                ZLinkReceiveBatchBudget batch) {
            RoutingId routingId = received.routingId().orElse(null);
            if (routingId == null) {
                LOGGER.warning(
                        "STREAM packet did not provide a source routing id: " + streamNode.name());
                return false;
            }
            if (isClosed()) {
                return false;
            }
            try {
                ZLinkStreamHeader header =
                        ZLinkStreamHeaderCodec.decodeOrPlain(received.header().toByteArray());
                long messageBytes = received.header().size() + (long) received.body().size();
                long maxMessageSize = streamNode.socketConfig().maxMessageSize();
                if (maxMessageSize > 0 && messageBytes > maxMessageSize) {
                    throw new ZLinkStreamMessageTooLargeException(
                            "STREAM packet exceeds MaxMessageSize");
                }
                try (var ignored =
                        systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext
                                .enter(permit)) {
                    dispatchToSession(streamNode, routingId, header, received.body())
                            .whenComplete((ignoredResult, error) -> received.close());
                } finally {
                    permit.abandonReservation();
                }
                batch.record(messageBytes);
                return true;
            } catch (RuntimeException | Error failure) {
                ZLinkStreamRuntime.this.isolatePeer(streamNode, stream, routingId, failure);
                return false;
            }
        }

        private boolean isClosed() {
            return closed;
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
                        && !ZLinkFlowContext.isValidFlowId(streamHeader.flowId().orElseThrow())) {
                    throw new IllegalArgumentException("STREAM header flow id must be UUIDv7");
                }
                ZLinkFlowContext.State receivedFlow =
                        streamHeader.flowId().isPresent()
                                ? new ZLinkFlowContext.State(
                                        streamHeader.flowId().orElseThrow(),
                                        streamHeader.flowOrigin().orElseThrow(),
                                        null)
                                : ZLinkFlowContext.create(ZLinkFlowOrigin.INBOUND);
                capturedFlow =
                        new ZLinkFlowContext.State(
                                receivedFlow.flowId(), receivedFlow.origin(), routingId.toHex());
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
        if (streamHeader.kind() == ZLinkStreamMessageKind.CONTROL) {
            dispatchControl(streamNode, stream, routingId, streamHeader, payload);
            return CompletableFuture.completedFuture(null);
        }
        if (draining) {
            sendSessionClosing(stream, routingId);
            return CompletableFuture.completedFuture(null);
        }
        Message payloadCopy =
                Message.from(
                        ZLinkStreamPayloadCodec.decode(dispatchHeader, payload, compressionCodec));
        ZLinkMessage sessionPayload =
                ZLinkMessage.fromEncoded(
                        ZLinkMessagePayloads.encoded(payloadCopy),
                        ZLinkCodecRegistration.serializerForReceivedStreamCodec(
                                serializer, dispatchHeader.codec()));
        payloadCopy.close();
        // One lane turn finds or starts the Session and enqueues this packet on its
        // queue, so the receive thread never waits and the packets of one Session
        // keep their receive order.
        return stateLane.admitAsync(
                () -> sessionSlotOnLane(streamNode, stream, routingId),
                slot -> {
                    SessionState published = slot.published();
                    if (published != null && published.replacementClosing()) {
                        return sendSessionClosing(stream, routingId);
                    }
                    if (published != null) {
                        published.markApplicationReceived();
                    }
                    traceStreamReceived(dispatchHeader, incomingFlow, routingId);
                    return slot.serials()
                            .executeApplication(
                                    () -> {
                                        SessionState state = slot.constructed();
                                        if (state == null) {
                                            // The constructor failed; the peer is isolated.
                                            return CompletableFuture.completedFuture(null);
                                        }
                                        traceStreamPhase(
                                                dispatchHeader,
                                                incomingFlow,
                                                routingId,
                                                ZLinkMessageFlowOutcome.ADMITTED);
                                        traceStreamPhase(
                                                dispatchHeader,
                                                incomingFlow,
                                                routingId,
                                                ZLinkMessageFlowOutcome.DISPATCHED);
                                        try (ZLinkFlowContext.Scope ignored =
                                                incomingFlow == null
                                                        ? () -> {}
                                                        : ZLinkFlowContext.enter(incomingFlow)) {
                                            return executeHandler(
                                                    () ->
                                                            state.context()
                                                                    .dispatchStage(
                                                                            dispatchHeader,
                                                                            sessionPayload,
                                                                            state.session()));
                                        }
                                    });
                });
    }

    private void traceStreamReceived(
            ZLinkStreamHeader dispatchHeader,
            ZLinkFlowContext.State incomingFlow,
            RoutingId routingId) {
        ZLinkMessageFlowTracer.TracePoint received = flow.begin(ZLinkMessageFlowOutcome.RECEIVED);
        if (received == null) {
            return;
        }
        received.trace(
                new ZLinkMessageFlowEvent(
                                ZLinkMessageFlowOutcome.RECEIVED,
                                ZLinkDispatchErrorSurface.STREAM_SESSION,
                                dispatchHeader.requestSequence().isPresent()
                                        ? ZLinkDispatchMessageKind.REQUEST
                                        : ZLinkDispatchMessageKind.SEND,
                                dispatchHeader.packetName(),
                                null,
                                null,
                                ZLinkStreamCorrelations.forTrace(dispatchHeader),
                                null,
                                null,
                                null,
                                null,
                                null,
                                null,
                                null,
                                null,
                                incomingFlow == null ? null : incomingFlow.flowId(),
                                incomingFlow == null ? null : incomingFlow.origin())
                        .withStreamSessionId(
                                incomingFlow == null
                                        ? routingId.toHex()
                                        : incomingFlow.streamSessionId()));
    }

    private void traceStreamPhase(
            ZLinkStreamHeader header,
            ZLinkFlowContext.State incomingFlow,
            RoutingId routingId,
            ZLinkMessageFlowOutcome phase) {
        ZLinkMessageFlowTracer.TracePoint tracePoint = flow.begin(phase);
        if (tracePoint == null) {
            return;
        }
        tracePoint.trace(
                new ZLinkMessageFlowEvent(
                                phase,
                                ZLinkDispatchErrorSurface.STREAM_SESSION,
                                header.requestSequence().isPresent()
                                        ? ZLinkDispatchMessageKind.REQUEST
                                        : ZLinkDispatchMessageKind.SEND,
                                header.packetName(),
                                null,
                                null,
                                ZLinkStreamCorrelations.forTrace(header),
                                null,
                                null,
                                null,
                                null,
                                null,
                                null,
                                null,
                                null,
                                incomingFlow == null ? null : incomingFlow.flowId(),
                                incomingFlow == null ? null : incomingFlow.origin())
                        .withStreamSessionId(routingId.toHex()));
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
            String key = sessionKey(streamNode, routingId);
            stateLane.runAsync(
                    () -> {
                        SessionState state = sessions.get(key);
                        if (state != null) {
                            state.markHeartbeatPong();
                        }
                    });
            return;
        }
        if (!HEARTBEAT_PING_NAME.equals(header.packetName())) {
            throw new IllegalArgumentException(
                    "unknown STREAM control packet: " + header.packetName());
        }
        sendControlAsync(
                stream,
                routingId,
                HEARTBEAT_PONG_NAME,
                Message.from(new byte[0]),
                "STREAM heartbeat pong was not admitted by the transport: ");
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
        boolean disconnected = nativeCode == 0 && "DISCONNECTED".equals(message);
        retireSession(
                streamNode,
                routingId,
                nativeCode == 0 ? "transport_error" : "protocol_error",
                state ->
                        disconnected
                                ? disconnectSessionStage(state)
                                : transportErrorDisconnectSessionStage(
                                        state, nativeCode, message));
    }

    private void isolatePeer(
            StreamNodeRegistration streamNode,
            ZLinkBackendStreamSocket stream,
            RoutingId routingId,
            Throwable failure) {
        boolean messageTooLarge = failure instanceof ZLinkStreamMessageTooLargeException;
        int nativeCode = messageTooLarge ? ZLinkStreamMessageTooLargeException.EMSGSIZE : 0;
        String reason = messageTooLarge ? "EMSGSIZE" : "malformed STREAM frame";
        retireSession(
                streamNode,
                routingId,
                "protocol_error",
                state -> transportErrorDisconnectSessionStage(state, nativeCode, reason));
        sendSessionClosing(stream, routingId, ZLinkSessionClosingControl.PROTOCOL_ERROR, reason);
        if (messageTooLarge) {
            try {
                stream.disconnectPeer(routingId);
            } catch (RuntimeException disconnectFailure) {
                LOGGER.log(
                        Level.FINE,
                        "STREAM EMSGSIZE disconnect failed: " + streamNode.name() + ":" + routingId,
                        disconnectFailure);
            }
        }
        LOGGER.log(
                Level.WARNING,
                "STREAM peer isolated after "
                        + reason
                        + ": "
                        + streamNode.name()
                        + ":"
                        + routingId,
                failure);
    }

    /**
     * Withdraws a peer's Session and queues {@code cleanup} behind the packets already admitted to
     * it. A Session still under construction is withdrawn before it is published, and its cleanup
     * runs after the constructor.
     */
    private void retireSession(
            StreamNodeRegistration streamNode,
            RoutingId routingId,
            String closeReason,
            Function<SessionState, CompletionStage<Void>> cleanup) {
        String key = sessionKey(streamNode, routingId);
        stateLane.admitAsync(
                () -> removeSessionSlotOnLane(key),
                slot ->
                        slot == null
                                ? CompletableFuture.<Void>completedFuture(null)
                                : slot.serials()
                                        .executeInfrastructure(
                                                () -> {
                                                    SessionState state = slot.constructed();
                                                    if (state == null) {
                                                        return CompletableFuture.completedFuture(
                                                                null);
                                                    }
                                                    recordSessionClosed(state, closeReason);
                                                    return executeHandler(
                                                            () -> cleanup.apply(state));
                                                }));
    }

    /** State-lane only: removes the published or constructing Session of {@code key}. */
    private SessionSlot removeSessionSlotOnLane(String key) {
        SessionState published = sessions.remove(key);
        if (published != null) {
            return SessionSlot.of(published);
        }
        PendingSession pending = pendingSessionCreations.remove(key);
        return pending == null ? null : SessionSlot.constructing(pending);
    }

    /**
     * State-lane only: the one decision on whether a peer's Session exists, is being constructed,
     * or must be constructed now. The constructor is the first turn of the new Session's queue, so
     * every packet admitted after this turn runs behind it.
     */
    private SessionSlot sessionSlotOnLane(
            StreamNodeRegistration streamNode,
            ZLinkBackendStreamSocket stream,
            RoutingId routingId) {
        String key = sessionKey(streamNode, routingId);
        SessionState published = sessions.get(key);
        if (published != null) {
            return SessionSlot.of(published);
        }
        PendingSession pending = pendingSessionCreations.get(key);
        if (pending == null) {
            PendingSession constructing =
                    new PendingSession(new ZLinkSessionSerialExecutor(serialExecutor));
            pendingSessionCreations.put(key, constructing);
            constructing
                    .serials()
                    .executeControl(
                            () ->
                                    constructSession(
                                            key, constructing, streamNode, stream, routingId));
            pending = constructing;
        }
        return SessionSlot.constructing(pending);
    }

    /** First turn of a new Session's queue: constructs, publishes and connects the Session. */
    private CompletionStage<Void> constructSession(
            String key,
            PendingSession pending,
            StreamNodeRegistration streamNode,
            ZLinkBackendStreamSocket stream,
            RoutingId routingId) {
        SessionState state;
        try {
            state = createSessionState(streamNode, stream, routingId, pending.serials());
        } catch (RuntimeException | Error failure) {
            pending.constructed().completeExceptionally(failure);
            isolatePeer(streamNode, stream, routingId, failure);
            return CompletableFuture.completedFuture(null);
        }
        pending.constructed().complete(state);
        ZLinkRuntimeMetrics.add("zlink.stream.connections.active", 1, Map.of());
        ZLinkRuntimeMetrics.increment("zlink.stream.connections.opened", Map.of());
        // Publication is withdrawn when the peer left while the constructor ran;
        // that retirement's cleanup is already queued behind this turn.
        stateLane.runAsync(
                () -> {
                    if (pendingSessionCreations.remove(key, pending)) {
                        sessions.put(key, state);
                    }
                });
        return executeHandler(
                () -> ZLinkHandlerStages.fromStageSupplier(state.session()::onConnected));
    }

    private static void recordSessionClosed(SessionState state, String reason) {
        if (!state.closeMetricRecorded().compareAndSet(false, true)) {
            return;
        }
        ZLinkRuntimeMetrics.add("zlink.stream.connections.active", -1, Map.of());
        ZLinkRuntimeMetrics.increment(
                "zlink.stream.connections.closed", Map.of("close_reason", reason));
    }

    private SessionState createSessionState(
            StreamNodeRegistration streamNode,
            ZLinkBackendStreamSocket stream,
            RoutingId routingId,
            ZLinkSessionSerialExecutor serials) {
        ZLinkSessionActorsRuntime sessionActors =
                actors == null && !streamSessionRelayAttached.getOrDefault(streamNode.name(), false)
                        ? null
                        : new ZLinkSessionActorsRuntime(
                                        streamSessionRelaySpotNodes.get(streamNode.name()),
                                        stream,
                                        routingId,
                                        actors,
                                        serializer,
                                        sessionRelayRouteReady,
                                        localActorDispatcher,
                                        streamSessionRelayAttached.getOrDefault(
                                                streamNode.name(), false),
                                        defaultCodec,
                                        flow,
                                        sessionRelocationSealTimeout,
                                        sessionBindingGenerations::incrementAndGet)
                                .metadataPolicy(
                                        metadataPolicy.sessionToActorKeys(),
                                        metadataPolicy.actorToSessionKeys());
        ZLinkInternalMeshNode ownerNode = actors == null ? null : meshNodes.get(actors.meshName());
        RoutingId ownerNodeRid = ownerNode == null ? null : ownerNode.routingId();
        long ownerNodeGeneration = ownerNode == null ? 0L : ownerNode.lifecycleGeneration();
        String ownerId = ownerNode == null ? null : ownerNode.localAuthorityOwnerId();
        long ownerLeaseGeneration =
                ownerNode == null ? 0L : ownerNode.localAuthorityLeaseGeneration();
        ZLinkStreamSessionContextState context =
                new ZLinkStreamSessionContextState(
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

    private static String sessionKey(StreamNodeRegistration streamNode, RoutingId routingId) {
        return streamNode.name() + ":" + routingId.toString();
    }

    @Override
    public void close() {
        closeAsync();
    }

    public CompletionStage<Void> closeAsync() {
        sessionContexts.forEach(ZLinkStreamSessionContextState::closeReplyRetries);
        return stateLane
                .runAsync(() -> List.copyOf(sessions.values()))
                .thenCompose(this::closeSessions);
    }

    private CompletionStage<Void> closeSessions(List<SessionState> activeSessions) {
        return CompletableFuture.allOf(
                        activeSessions.stream()
                                .map(
                                        state ->
                                                state.serials()
                                                        .executeFinal(
                                                                () ->
                                                                        executeHandler(
                                                                                () ->
                                                                                        disconnectSessionStage(
                                                                                                state)))
                                                        .toCompletableFuture())
                                .toArray(CompletableFuture[]::new))
                .handle(
                        (ignored, failure) -> {
                            finishClose(activeSessions);
                            return null;
                        });
    }

    private void finishClose(List<SessionState> activeSessions) {
        stateLane.runAsync(sessions::clear);
        sessionContexts.forEach(ZLinkStreamSessionContextState::closeReplyRetries);
        String closeReason = draining ? "server_drain" : "transport_error";
        for (int index = 0; index < activeSessions.size(); index++) {
            recordSessionClosed(activeSessions.get(index), closeReason);
        }
        replyRetryExecutor.shutdownNow();
        boolean replyRetriesStopped =
                awaitExecutorTermination(replyRetryExecutor, "STREAM error reply retry executor");
        livenessExecutor.shutdownNow();
        boolean livenessStopped =
                awaitExecutorTermination(livenessExecutor, "STREAM liveness executor");
        receiveLoops.forEach(StreamReceiveLoop::close);
        receiveExecutor.shutdownNow();
        boolean receiveStopped =
                awaitExecutorTermination(receiveExecutor, "STREAM receive executor");
        if (!replyRetriesStopped || !livenessStopped || !receiveStopped) {
            LOGGER.severe(
                    "STREAM runtime resources did not quiesce; native stream and context remain"
                            + " open");
            return;
        }
        for (ZLinkBackendStreamSocket stream : streams) {
            stream.close();
        }
        if (ownsContext) {
            context.close();
        }
    }

    private static boolean awaitExecutorTermination(ExecutorService executor, String description) {
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
        List<SessionState> activeSessions = inStateLane(() -> List.copyOf(sessions.values()));
        CompletableFuture<?>[] barriers =
                activeSessions.stream()
                        .map(
                                state ->
                                        state.serials()
                                                .executeFinal(
                                                        () ->
                                                                CompletableFuture.completedFuture(
                                                                        null)))
                        .map(CompletionStage::toCompletableFuture)
                        .toArray(CompletableFuture[]::new);
        return CompletableFuture.allOf(barriers);
    }

    public CompletionStage<Void> notifyServerDrain() {
        List<Map.Entry<String, SessionState>> active =
                inStateLane(() -> List.copyOf(sessions.entrySet()));
        List<CompletableFuture<Void>> notifications = new ArrayList<>();
        for (Map.Entry<String, SessionState> entry : active) {
            SessionState state = entry.getValue();
            notifications.add(
                    sendSessionClosing(state.stream(), state.routingId()).toCompletableFuture());
        }
        return CompletableFuture.allOf(notifications.toArray(CompletableFuture[]::new));
    }

    private static CompletionStage<Void> sendSessionClosing(
            ZLinkBackendStreamSocket stream, RoutingId routingId) {
        return sendSessionClosing(
                stream, routingId, ZLinkSessionClosingControl.SERVER_DRAIN, "server drain");
    }

    private static CompletionStage<Void> sendSessionClosing(
            ZLinkBackendStreamSocket stream, RoutingId routingId, int reason, String diagnostic) {
        return sendControlAsync(
                stream,
                routingId,
                ZLinkSessionClosingControl.NAME,
                Message.from(ZLinkSessionClosingControl.encode(reason, diagnostic)),
                "STREAM session-closing control failed during transport teardown: ");
    }

    private void checkSessionLiveness() {
        long now = System.nanoTime();
        List<Map.Entry<String, SessionState>> snapshot =
                inStateLane(() -> List.copyOf(sessions.entrySet()));
        for (Map.Entry<String, SessionState> entry : snapshot) {
            SessionState state = entry.getValue();
            int reason =
                    now - state.lastHeartbeatPongNanos() >= HEARTBEAT_TIMEOUT_NANOS
                            ? ZLinkSessionClosingControl.HEARTBEAT_TIMEOUT
                            : now - state.lastApplicationNanos() >= IDLE_TIMEOUT_NANOS
                                    ? ZLinkSessionClosingControl.IDLE_TIMEOUT
                                    : 0;
            if (reason == 0) {
                sendHeartbeatPing(state);
                continue;
            }
            boolean removed = inStateLane(() -> sessions.remove(entry.getKey(), state));
            if (!removed) {
                continue;
            }
            sendSessionClosing(
                    state.stream(),
                    state.routingId(),
                    reason,
                    reason == ZLinkSessionClosingControl.HEARTBEAT_TIMEOUT
                            ? "heartbeat timeout"
                            : "idle timeout");
            recordSessionClosed(
                    state,
                    reason == ZLinkSessionClosingControl.HEARTBEAT_TIMEOUT
                            ? "heartbeat_timeout"
                            : "idle_timeout");
            state.serials()
                    .executeInfrastructure(
                            () -> executeHandler(() -> disconnectSessionStage(state)));
        }
    }

    private static void sendHeartbeatPing(SessionState state) {
        sendControlAsync(
                state.stream(),
                state.routingId(),
                HEARTBEAT_PING_NAME,
                Message.from(new byte[0]),
                "STREAM heartbeat ping failed during transport teardown: ");
    }

    private static CompletionStage<Void> sendControlAsync(
            ZLinkBackendStreamSocket stream,
            RoutingId routingId,
            String packetName,
            Message payload,
            String failureMessage) {
        CompletionStage<Void> submission;
        try {
            ZLinkStreamHeader header =
                    new ZLinkStreamHeader(
                            ZLinkStreamMessageKind.CONTROL,
                            ZLinkStreamCodec.RAW,
                            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
                            Optional.empty(),
                            packetName,
                            Map.of(),
                            Optional.empty());
            submission = stream.sendAsync(routingId, header, List.of(payload));
        } catch (RuntimeException failure) {
            payload.close();
            LOGGER.log(Level.FINE, failureMessage + routingId, failure);
            return CompletableFuture.completedFuture(null);
        }
        return submission.handle(
                (ignored, failure) -> {
                    payload.close();
                    if (failure != null) {
                        LOGGER.log(Level.FINE, failureMessage + routingId, failure);
                    }
                    return null;
                });
    }

    private <T> CompletionStage<T> executeHandler(Supplier<CompletionStage<T>> operation) {
        CompletableFuture<CompletionStage<T>> entered = new CompletableFuture<>();
        ZLinkFlowContext.State capturedFlow = ZLinkFlowContext.current();
        var applicationJob = ZLinkApplicationJobContext.transferToQueuedJob();
        try {
            handlerExecutor.execute(
                    () -> {
                        try (ZLinkFlowContext.Scope ignored =
                                        capturedFlow == null
                                                ? () -> {}
                                                : ZLinkFlowContext.enter(capturedFlow);
                                var ignoredApplicationJob =
                                        ZLinkApplicationJobContext.enterQueued(applicationJob)) {
                            ZLinkApplicationJobContext.beforeFirstApplicationInstruction();
                            entered.complete(
                                    Objects.requireNonNull(operation.get(), "handler result"));
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
                .thenCompose(
                        ignored ->
                                ZLinkHandlerStages.fromStageSupplier(
                                        state.session()::onDisconnected))
                .whenComplete((ignored, failure) -> sessionContexts.remove(state.context()));
    }

    private CompletionStage<Void> transportErrorDisconnectSessionStage(
            SessionState state, int nativeCode, String message) {
        state.context().closeReplyRetries();
        return ZLinkHandlerStages.fromStageSupplier(
                        () ->
                                state.session()
                                        .onError(
                                                new ZLinkStreamError(
                                                        ZLinkStreamSessionError.TRANSPORT_ERROR,
                                                        message)))
                .thenCompose(ignored -> notifyBoundActorsDisconnectedBestEffort(state))
                .thenCompose(
                        ignored ->
                                ZLinkHandlerStages.fromStageSupplier(
                                        state.session()::onDisconnected))
                .whenComplete((ignored, failure) -> sessionContexts.remove(state.context()));
    }

    private CompletionStage<Void> notifyBoundActorsDisconnectedBestEffort(SessionState state) {
        return state.context()
                .notifyBoundActorsDisconnected(PHYSICAL_DISCONNECT_TIMEOUT)
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
        private final Set<ReplacementIdentity> replacements = ConcurrentHashMap.newKeySet();
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

        ZLinkSession session() {
            return session;
        }

        ZLinkSessionSerialExecutor serials() {
            return serials;
        }

        ZLinkStreamSessionContextState context() {
            return context;
        }

        ZLinkBackendStreamSocket stream() {
            return stream;
        }

        RoutingId routingId() {
            return routingId;
        }

        ZLinkSessionActorsRuntime actorRuntime() {
            return actorRuntime;
        }

        boolean replacementClosing() {
            return replacementClosing.get();
        }

        AtomicBoolean closeScheduled() {
            return closeScheduled;
        }

        boolean matchesOwner(
                systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec
                                .RetiredSessionRouteFence
                        retired) {
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

        long lastApplicationNanos() {
            return lastApplicationNanos;
        }

        long lastHeartbeatPongNanos() {
            return lastHeartbeatPongNanos;
        }

        AtomicBoolean closeMetricRecorded() {
            return closeMetricRecorded;
        }

        void markApplicationReceived() {
            lastApplicationNanos = System.nanoTime();
        }

        void markHeartbeatPong() {
            lastHeartbeatPongNanos = System.nanoTime();
        }
    }

    private record ReplacementIdentity(
            String actorId, RoutingId sessionRid, long retiredBindingGeneration) {}

    /** A Session whose constructor is the first turn of {@code serials}. */
    private record PendingSession(
            ZLinkSessionSerialExecutor serials, CompletableFuture<SessionState> constructed) {
        PendingSession(ZLinkSessionSerialExecutor serials) {
            this(serials, new CompletableFuture<>());
        }
    }

    /** A published Session, or one under construction, with the queue both share. */
    private record SessionSlot(SessionState published, PendingSession pending) {
        static SessionSlot of(SessionState state) {
            return new SessionSlot(state, null);
        }

        static SessionSlot constructing(PendingSession pending) {
            return new SessionSlot(null, pending);
        }

        ZLinkSessionSerialExecutor serials() {
            return published != null ? published.serials() : pending.serials();
        }

        /**
         * The Session in a turn of {@link #serials()}; the constructor's turn has run by then, so
         * null means construction failed.
         */
        SessionState constructed() {
            if (published != null) {
                return published;
            }
            CompletableFuture<SessionState> constructed = pending.constructed();
            return constructed.isCompletedExceptionally() ? null : constructed.getNow(null);
        }
    }

    private static ZLinkStreamCodec defaultCodec(ZLinkFrameworkRegistration registration) {
        return registration.codecs().streamCodecForCustomSerializer().orElse(ZLinkStreamCodec.JSON);
    }
}
