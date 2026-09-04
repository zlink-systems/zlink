package systems.zlink.framework.runtime.actors;
import java.util.Objects;
import java.util.concurrent.CompletionException;
import java.util.concurrent.ConcurrentHashMap;

import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;

import systems.zlink.framework.runtime.internal.backend.*;

import java.time.Duration;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.Optional;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.TimeoutException;
import java.util.function.Predicate;
import java.util.function.Supplier;
import java.util.function.LongFunction;
import java.util.logging.Logger;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locations.ZLinkLocationOptions;
import systems.zlink.framework.streams.ZLinkSessionActor;
import systems.zlink.framework.streams.ZLinkSessionActors;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

public final class ZLinkSessionActorsRuntime implements ZLinkSessionActors {
    private static final Logger LOGGER = Logger.getLogger(ZLinkSessionActorsRuntime.class.getName());
    static final Duration RELAY_SUBMIT_TIMEOUT = Duration.ofSeconds(30);
    private static final boolean STREAM_TRACE =
        "1".equals(System.getenv("ZLINK_JAVA_STREAM_TRACE"));
    private static final ZLinkSessionRelayHeaders RELAY_HEADERS = new ZLinkSessionRelayHeaders();
    static final int TARGET_OUTBOUND_CAPACITY = 4_096;
    private final ZLinkBackendStreamSocket stream;
    private final ZLinkInternalSpotNode spotNode;
    private final RoutingId sessionRid;
    private final ZLinkActorRuntime actors;
    private final ZLinkMessageSerializer serializer;
    private final Predicate<RoutingId> routeReady;
    private final LocalActorDispatcher localActorDispatcher;
    private final boolean nativeSessionRelayAttached;
    private final ZLinkStreamCodec defaultCodec;
    private final ZLinkMessageFlowTracer flow;
    private final int targetOutboundCapacity;
    private final Duration sessionRelocationSealTimeout;
    // This runtime is one Session owner's C2 state boundary.  Keep the concurrent
    // binding views below for the synchronous ZLinkSessionActors query surface;
    // all cross-collection state transitions run through this lane.
    private final ZLinkStateLane stateLane = new ZLinkStateLane();
    private final List<ZLinkSessionActor> bound = new CopyOnWriteArrayList<>();
    private final ConcurrentHashMap<String, StoredBindingRoute>
        bindingRoutes = new ConcurrentHashMap<>();
    private final ConcurrentHashMap<String, CompletableFuture<Void>>
        bindingTransitions = new ConcurrentHashMap<>();
    //  Command 42 terminals keyed by relocation id. The bound and the
    //  bounded eviction mirror the C++ `_session_seal_terminals` store
    //  (public_host_runtime.cpp:4005-4020).
    private static final int SEAL_TERMINAL_CAPACITY = 65_536;
    //  How many spent terminals stay behind to answer a command 42 that was
    //  still in flight when its route reached a terminal.
    private static final int SPENT_SEAL_TERMINAL_RETENTION = 64;
    private final java.util.LinkedHashMap<
        SessionRelocationKey, SealTerminal>
        sealTerminals = new java.util.LinkedHashMap<>();
    private final java.util.LinkedHashMap<
        SessionRelocationKey, RouteTerminal>
        routeTerminals = new java.util.LinkedHashMap<>();
    private final java.util.HashMap<SessionRelocationKey, RouteFlight>
        routeFlights = new java.util.HashMap<>();
    private final java.util.HashMap<String, TargetOutboundBinding>
        targetOutboundBindings = new java.util.HashMap<>();
    private final AtomicLong bindingGenerations = new AtomicLong();
    private final java.util.HashMap<String, IngressGate> ingressGates =
        new java.util.HashMap<>();
    private long nextFallbackIngressSequence = 1;
    private ZLinkRelayMetadataPolicy metadataPolicy = ZLinkRelayMetadataPolicy.EMPTY;
    private boolean relocationStopped;
    private boolean relocationSealTimedOut;

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

    private <T> CompletionStage<T> onStateLane(Supplier<T> work) {
        return stateLane.runAsync(work);
    }

    public ZLinkSessionActorsRuntime metadataPolicy(
        Set<String> sessionToActorKeys,
        Set<String> actorToSessionKeys) {
        metadataPolicy =
            new ZLinkRelayMetadataPolicy(sessionToActorKeys, actorToSessionKeys);
        return this;
    }

    @FunctionalInterface
    public interface LocalActorDispatcher {
        CompletionStage<Optional<LocalActorReply>> dispatch(
            ZLinkBackendActorRef actor,
            long sourceSessionSequence,
            ZLinkStreamHeader header,
            Message payload);
    }

    public record LocalActorReply(
        Message payload,
        ZLinkStreamCodec codec) {
        public LocalActorReply {
            Objects.requireNonNull(payload, "payload");
            Objects.requireNonNull(codec, "codec");
        }
    }

    @FunctionalInterface
    interface IngressAdmission {
        CompletionStage<Void> submit(
            LongFunction<CompletionStage<Void>> operation);
    }

    public static void enterRelayDispatch(ZLinkStreamHeader header) {
        RELAY_HEADERS.enter(header);
    }

    public static void enterRelayDispatch(
        ZLinkSessionDispatchContext dispatch,
        ZLinkStreamHeader header) {
        RELAY_HEADERS.enter(dispatch, header);
    }

    public static void exitRelayDispatch() {
        RELAY_HEADERS.exit();
    }

    public static void exitRelayDispatch(ZLinkSessionDispatchContext dispatch) {
        RELAY_HEADERS.exit(dispatch);
    }

    public ZLinkSessionActorsRuntime(
        ZLinkBackendStreamSocket stream,
        RoutingId sessionRid,
        ZLinkActorRuntime actors,
        ZLinkMessageSerializer serializer) {
        this(
            null,
            stream,
            sessionRid,
            actors,
            serializer,
            ignored -> true,
            null,
            true,
            ZLinkStreamCodec.JSON,
            null);
    }

    public ZLinkSessionActorsRuntime(
        ZLinkBackendStreamSocket stream,
        RoutingId sessionRid,
        ZLinkActorRuntime actors,
        ZLinkMessageSerializer serializer,
        Predicate<RoutingId> routeReady) {
        this(
            null,
            stream,
            sessionRid,
            actors,
            serializer,
            routeReady,
            null,
            true,
            ZLinkStreamCodec.JSON,
            null);
    }

    public ZLinkSessionActorsRuntime(
        ZLinkBackendStreamSocket stream,
        RoutingId sessionRid,
        ZLinkActorRuntime actors,
        ZLinkMessageSerializer serializer,
        Predicate<RoutingId> routeReady,
        LocalActorDispatcher localActorDispatcher,
        boolean nativeSessionRelayAttached,
        ZLinkStreamCodec defaultCodec) {
        this(null, stream, sessionRid, actors, serializer, routeReady,
            localActorDispatcher, nativeSessionRelayAttached, defaultCodec, null);
    }

    public ZLinkSessionActorsRuntime(
        ZLinkInternalSpotNode spotNode,
        ZLinkBackendStreamSocket stream,
        RoutingId sessionRid,
        ZLinkActorRuntime actors,
        ZLinkMessageSerializer serializer,
        Predicate<RoutingId> routeReady,
        LocalActorDispatcher localActorDispatcher,
        boolean nativeSessionRelayAttached,
        ZLinkStreamCodec defaultCodec) {
        this(spotNode, stream, sessionRid, actors, serializer, routeReady,
            localActorDispatcher, nativeSessionRelayAttached, defaultCodec, null);
    }

    public ZLinkSessionActorsRuntime(
        ZLinkInternalSpotNode spotNode,
        ZLinkBackendStreamSocket stream,
        RoutingId sessionRid,
        ZLinkActorRuntime actors,
        ZLinkMessageSerializer serializer,
        Predicate<RoutingId> routeReady,
        LocalActorDispatcher localActorDispatcher,
        boolean nativeSessionRelayAttached,
        ZLinkStreamCodec defaultCodec,
        ZLinkMessageFlowTracer flow) {
        this(
            spotNode, stream, sessionRid, actors, serializer, routeReady,
            localActorDispatcher, nativeSessionRelayAttached, defaultCodec,
            flow, TARGET_OUTBOUND_CAPACITY,
            defaultSessionRelocationSealTimeout());
    }

    public ZLinkSessionActorsRuntime(
        ZLinkInternalSpotNode spotNode,
        ZLinkBackendStreamSocket stream,
        RoutingId sessionRid,
        ZLinkActorRuntime actors,
        ZLinkMessageSerializer serializer,
        Predicate<RoutingId> routeReady,
        LocalActorDispatcher localActorDispatcher,
        boolean nativeSessionRelayAttached,
        ZLinkStreamCodec defaultCodec,
        ZLinkMessageFlowTracer flow,
        Duration sessionRelocationSealTimeout) {
        this(
            spotNode, stream, sessionRid, actors, serializer, routeReady,
            localActorDispatcher, nativeSessionRelayAttached, defaultCodec,
            flow, TARGET_OUTBOUND_CAPACITY, sessionRelocationSealTimeout);
    }

    ZLinkSessionActorsRuntime(
        ZLinkInternalSpotNode spotNode,
        ZLinkBackendStreamSocket stream,
        RoutingId sessionRid,
        ZLinkActorRuntime actors,
        ZLinkMessageSerializer serializer,
        Predicate<RoutingId> routeReady,
        LocalActorDispatcher localActorDispatcher,
        boolean nativeSessionRelayAttached,
        ZLinkStreamCodec defaultCodec,
        ZLinkMessageFlowTracer flow,
        int targetOutboundCapacity) {
        this(
            spotNode, stream, sessionRid, actors, serializer, routeReady,
            localActorDispatcher, nativeSessionRelayAttached, defaultCodec,
            flow, targetOutboundCapacity,
            defaultSessionRelocationSealTimeout());
    }

    ZLinkSessionActorsRuntime(
        ZLinkInternalSpotNode spotNode,
        ZLinkBackendStreamSocket stream,
        RoutingId sessionRid,
        ZLinkActorRuntime actors,
        ZLinkMessageSerializer serializer,
        Predicate<RoutingId> routeReady,
        LocalActorDispatcher localActorDispatcher,
        boolean nativeSessionRelayAttached,
        ZLinkStreamCodec defaultCodec,
        ZLinkMessageFlowTracer flow,
        int targetOutboundCapacity,
        Duration sessionRelocationSealTimeout) {
        if (targetOutboundCapacity <= 0) {
            throw new IllegalArgumentException(
                "targetOutboundCapacity must be positive");
        }
        this.spotNode = spotNode;
        this.stream = stream;
        this.sessionRid = sessionRid;
        this.actors = actors;
        this.serializer = serializer;
        this.routeReady = routeReady == null ? ignored -> true : routeReady;
        this.localActorDispatcher = localActorDispatcher;
        this.nativeSessionRelayAttached = nativeSessionRelayAttached;
        this.defaultCodec = defaultCodec == null ? ZLinkStreamCodec.JSON : defaultCodec;
        this.flow = flow;
        this.targetOutboundCapacity = targetOutboundCapacity;
        this.sessionRelocationSealTimeout = validateSessionRelocationSealTimeout(
            sessionRelocationSealTimeout);
    }

    private static Duration defaultSessionRelocationSealTimeout() {
        return new ZLinkLocationOptions().sessionRelocationSealTimeout();
    }

    private static Duration validateSessionRelocationSealTimeout(
        Duration value) {
        ZLinkLocationOptions options = new ZLinkLocationOptions();
        options.setSessionRelocationSealTimeout(value);
        return options.sessionRelocationSealTimeout();
    }

    @Override
    public List<ZLinkSessionActor> bound() {
        return List.copyOf(bound);
    }

    @Override
    public CompletionStage<ZLinkSessionActor> bind(ZLinkActor actor) {
        return bindManagedAsyncCore(actor);
    }

    @Override
    public CompletionStage<ZLinkSessionActor> bind(ActorRef actor) {
        ZLinkBackendActorRef ref = new ZLinkBackendActorRef(
            actor.nodeRid(),
            actor.actorId(),
            actor.objectGeneration());
        return bindBackendRef(ref, actor.meshName());
    }

    @Override
    public CompletionStage<ZLinkSessionActor> bindOrGet(ActorRef actor) {
        ZLinkBackendActorRef ref = new ZLinkBackendActorRef(
            actor.nodeRid(),
            actor.actorId(),
            actor.objectGeneration());
        Optional<ZLinkSessionActor> existing = bound.stream()
            .filter(boundActor -> sameRef(boundActor.ref(), actor))
            .findFirst();
        return existing
            .<CompletionStage<ZLinkSessionActor>>map(CompletableFuture::completedFuture)
            .orElseGet(() -> bindBackendRef(ref, actor.meshName()));
    }

    @Override
    public Optional<ZLinkSessionActor> find(String actorId) {
        return bound.stream()
            .filter(actor -> actor.actorId().equals(actorId))
            .findFirst();
    }

    public CompletionStage<Void> notifyDisconnectedAll() {
        return notifyDisconnectedAllCore(RELAY_SUBMIT_TIMEOUT);
    }

    public CompletionStage<Void> notifyDisconnectedAll(Duration timeout) {
        return notifyDisconnectedAllCore(timeout);
    }

    private CompletionStage<Void> notifyDisconnectedAllCore(Duration timeout) {
        Objects.requireNonNull(timeout, "timeout");
        List<ZLinkSessionActor> current = List.copyOf(bound);
        return CompletableFuture.allOf(current.stream()
            .map(actor -> notifyDisconnectedSafely(actor, timeout))
            .toArray(CompletableFuture[]::new))
            .whenComplete((ignored, error) -> {
                current.forEach(this::removeBinding);
                stopRelocationOwner(new ZLinkConfigurationException(
                    "Session disconnected while relocation state was pending"));
            });
    }

    private void stopRelocationOwner(RuntimeException failure) {
        stopRelocationOwnerCore(failure, null);
    }

    private boolean stopRelocationOwner(
        RuntimeException failure,
        SealTerminal timeoutTerminal) {
        return stopRelocationOwnerCore(failure, timeoutTerminal);
    }

    private boolean stopRelocationOwnerCore(
        RuntimeException failure,
        SealTerminal timeoutTerminal) {
        StopRelocationState stopped = inStateLane(() -> {
            List<HeldIngress> held = new ArrayList<>();
            if (relocationStopped) {
                return null;
            }
            if (timeoutTerminal != null
                && (timeoutTerminal.consumed()
                    || sealTerminals.get(
                        relocationKey(timeoutTerminal.seal()))
                        != timeoutTerminal)) {
                return null;
            }
            relocationStopped = true;
            relocationSealTimedOut = timeoutTerminal != null;
            if (timeoutTerminal != null) {
                timeoutTerminal.consume();
            }
            ingressGates.values().forEach(
                gate -> held.addAll(gate.detachHeld()));
            ingressGates.clear();
            bindingRoutes.clear();
            bindingTransitions.clear();
            List<SealTerminal> seals = List.copyOf(sealTerminals.values());
            targetOutboundBindings.values().forEach(
                binding -> binding.stop(TargetOutboundSettlement.SHUTDOWN));
            targetOutboundBindings.clear();
            List<RouteFlight> routes = List.copyOf(routeFlights.values());
            routeFlights.clear();
            sealTerminals.clear();
            routeTerminals.clear();
            return new StopRelocationState(held, seals, routes);
        });
        if (stopped == null) {
            return false;
        }
        failHeld(stopped.held(), failure);
        stopped.seals().forEach(seal -> seal.fail(failure));
        stopped.routes().forEach(route ->
            route.completion().completeExceptionally(failure));
        return true;
    }

    private record StopRelocationState(
        List<HeldIngress> held,
        List<SealTerminal> seals,
        List<RouteFlight> routes) {
    }

    private static CompletableFuture<Void> notifyDisconnectedSafely(
        ZLinkSessionActor actor,
        Duration timeout) {
        try {
            return actor instanceof ZLinkBoundActor boundActor
                ? boundActor.notifyDisconnected(timeout).toCompletableFuture()
                : actor.notifyDisconnected().toCompletableFuture();
        } catch (RuntimeException error) {
            return CompletableFuture.failedFuture(error);
        }
    }

    private CompletionStage<ZLinkSessionActor> bindBackendRef(
        ZLinkBackendActorRef ref,
        String meshName) {
        trace(STREAM_TRACE ? "session-actor bind-start sessionRid=" + sessionRid
            + " actorNode=" + ref.nodeRid()
            + " actorId=" + ref.actorId()
            + " generation=" + ref.generation() : null);
        if (actors != null) {
            Optional<ZLinkActor> localActor = actors.localActor(ref.actorId());
            if (localActor.isPresent()) {
                ZLinkBackendActorRef localRef = actors.refFor(localActor.get());
                if (localRef.nodeRid().equals(ref.nodeRid())
                    && localRef.actorId().equals(ref.actorId())
                    && localRef.generation() == ref.generation()) {
                    trace(STREAM_TRACE ? "session-actor bind-local sessionRid=" + sessionRid
                        + " actorNode=" + ref.nodeRid()
                        + " actorId=" + ref.actorId()
                        + " generation=" + ref.generation() : null);
                    return bindManagedAsyncCore(localActor.get());
                }
            }
        }
        CompletionStage<Void> authorityReady = actors == null
            ? CompletableFuture.completedFuture(null)
            : actors.prepareRemoteSessionBinding(ref);
        return authorityReady.thenCompose(ignored -> replaceBinding(
            ref.actorId(),
            () -> awaitRouteReady(ref).thenCompose(routeReadyIgnored -> {
                trace(STREAM_TRACE ? "session-actor bind-native-submit sessionRid=" + sessionRid
                    + " actorNode=" + ref.nodeRid()
                    + " actorId=" + ref.actorId()
                    + " generation=" + ref.generation() : null);
                return ZLinkBoundSessionRuntime.bindActorWithRetry(
                    stream, sessionRid, ref, RELAY_SUBMIT_TIMEOUT);
            })
            .thenApply(bindIgnored -> {
                trace(STREAM_TRACE ? "session-actor bind-native-ok sessionRid=" + sessionRid
                    + " actorNode=" + ref.nodeRid()
                    + " actorId=" + ref.actorId()
                    + " generation=" + ref.generation() : null);
                AtomicReference<ZLinkBoundActor> binding = new AtomicReference<>();
                long bindingGeneration = currentBindingGeneration(ref.actorId());
                ZLinkBoundActor actor = new ZLinkBoundActor(
                    stream,
                    sessionRid,
                    ref,
                    meshName,
                    Optional.empty(),
                    actors,
                    serializer,
                    0,
                    bindingGeneration,
                    routeReady,
                    null,
                    true,
                    defaultCodec,
                    RELAY_HEADERS,
                    flow,
                    () -> isCurrentBinding(binding.get()),
                    operation -> admitIngress(binding.get(), operation),
                    metadataPolicy);
                binding.set(actor);
                actor.setUnbindListener(() -> removeBinding(actor));
                return actor;
            })))
            .thenCompose(actor -> actor.notifyRemoteBoundSession()
                .thenApply(notificationIgnored -> actor))
            .whenComplete((actor, error) -> {
                if (error != null && actor != null) {
                    removeBinding(actor);
                }
                if (error != null) {
                    trace(STREAM_TRACE ? "session-actor bind-native-error sessionRid=" + sessionRid
                        + " actorNode=" + ref.nodeRid()
                        + " actorId=" + ref.actorId()
                        + " generation=" + ref.generation()
                        + " error=" + errorSummary(error) : null);
                }
            })
            .thenApply(actor -> (ZLinkSessionActor) actor);
    }

    private static boolean sameRef(ActorRef left, ActorRef right) {
        return left != null
            && right != null
            && left.actorId().equals(right.actorId())
            && left.nodeRid().equals(right.nodeRid())
            && left.objectGeneration() == right.objectGeneration()
            && left.meshName().equals(right.meshName());
    }

    private CompletionStage<ZLinkSessionActor> bindManagedAsyncCore(ZLinkActor actor) {
        if (actors == null) {
            return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                "managed actor binding requires an actor runtime"));
        }
        ZLinkBackendActorRef ref = actors.refFor(actor);
        return replaceBinding(actor.context().actorId(), () -> {
            CompletionStage<Void> nativeBinding = nativeSessionRelayAttached
                ? awaitRouteReady(ref)
                    .thenCompose(ignored -> ZLinkBoundSessionRuntime.bindActorWithRetry(
                        stream, sessionRid, ref, RELAY_SUBMIT_TIMEOUT))
                : CompletableFuture.completedFuture(null);
            return nativeBinding.thenApply(ignored -> {
                long bindingGeneration = currentBindingGeneration(ref.actorId());
                ZLinkBoundSessionRuntime boundSession =
                    new ZLinkBoundSessionRuntime(
                        stream,
                        spotNode,
                        sessionRid,
                        ref.actorId(),
                        serializer,
                        actors,
                        actor,
                        defaultCodec,
                        routeReady,
                        metadataPolicy);
                RoutingId sourceNodeRid =
                    nativeSessionRelayAttached && spotNode != null ? spotNode.routingId() : null;
                RoutingId sourceSessionRid =
                    nativeSessionRelayAttached ? sessionRid : null;
                long bindingToken = actors.bindSession(
                    actor,
                    boundSession,
                    sourceNodeRid,
                    sourceSessionRid,
                    bindingGeneration,
                    0);
                boundSession.setBindingToken(bindingToken);
                AtomicReference<ZLinkBoundActor> binding = new AtomicReference<>();
                ZLinkBoundActor boundActor = new ZLinkBoundActor(
                    stream,
                    sessionRid,
                    ref,
                    actors.meshName(),
                    Optional.of(actor),
                    actors,
                    serializer,
                    bindingToken,
                    bindingGeneration,
                    routeReady,
                    localActorDispatcher,
                    nativeSessionRelayAttached,
                    defaultCodec,
                    RELAY_HEADERS,
                    flow,
                    () -> isCurrentBinding(binding.get()),
                    operation -> admitIngress(binding.get(), operation),
                    metadataPolicy);
                binding.set(boundActor);
                boundSession.setUnbindListener(() -> removeBinding(boundActor));
                boundActor.setUnbindListener(() -> removeBinding(boundActor));
                boundSession.setRebindListener(target ->
                    recordNativeRebind(boundActor, target));
                return boundActor;
            });
        })
            .thenApply(value -> (ZLinkSessionActor) value);
    }

    void recordNativeRebind(
        ZLinkBoundActor actor,
        ZLinkBackendActorRef targetActor) {
        actor.rebindNativeActor(targetActor);
        inStateLane(() -> bindingRoutes.computeIfPresent(
            actor.actorId(),
            (ignored, current) -> current.toNativeTarget(targetActor)));
    }

    private CompletionStage<ZLinkBoundActor> replaceBinding(
        String actorId,
        Supplier<CompletionStage<ZLinkBoundActor>> createBinding) {
        CompletableFuture<Void> tail = new CompletableFuture<>();
        CompletableFuture<Void> previousTransition = inStateLane(() -> {
            CompletableFuture<Void> previous = bindingTransitions.get(actorId);
            bindingTransitions.put(actorId, tail);
            return previous;
        });
        CompletionStage<Void> ready = previousTransition == null
            ? CompletableFuture.completedFuture(null)
            : previousTransition.handle((ignored, failure) -> null);
        CompletionStage<ZLinkBoundActor> installation = ready
            .thenCompose(ignored -> installReplacement(
                actorId, createBinding));
        installation.whenComplete((ignored, failure) -> {
            // Complete off the lane so an inline dependent cannot re-enter it.
            tail.completeAsync(() -> null).whenComplete((completed, ignoredError) ->
                inStateLane(() -> {
                    bindingTransitions.remove(actorId, tail);
                    return null;
                }));
        });
        return installation;
    }

    private CompletionStage<ZLinkBoundActor> installReplacement(
        String actorId,
        Supplier<CompletionStage<ZLinkBoundActor>> createBinding) {
        List<ZLinkBoundActor> previous = bound.stream()
            .filter(existing -> existing.actorId().equals(actorId))
            .map(ZLinkBoundActor.class::cast)
            .toList();
        CompletionStage<ZLinkBoundActor> created;
        try {
            created = createBinding.get();
        } catch (RuntimeException failure) {
            throw failure;
        }
        return created.thenCompose(actor -> {
            installBinding(actor);
            if (!previous.isEmpty()) {
                previous.forEach(bound::remove);
            }
            // The old binding cleanup is deliberately not part of bind
            // completion. The session owner receives command 51 and owns the
            // callback/close lifecycle for the retired physical session.
            return CompletableFuture.completedFuture(actor);
        });
    }

    private long currentBindingGeneration(String actorId) {
        long generation = nativeSessionRelayAttached
            ? stream.boundActorBindingGeneration(sessionRid, actorId)
            : 0;
        if (generation > 0) {
            return generation;
        }
        generation = bindingGenerations.incrementAndGet();
        if (generation <= 0) {
            throw new ZLinkConfigurationException(
                "Session binding generation is exhausted");
        }
        return generation;
    }

    private ZLinkBoundActor installBinding(ZLinkBoundActor actor) {
        ActorRef current = actor.ref();
        ZLinkBackendActorRef backendRef = new ZLinkBackendActorRef(
            current.nodeRid(), current.actorId(), current.objectGeneration());
        long nodeGeneration = spotNode == null
            ? 0L
            : spotNode.actorNodeGeneration(backendRef);
        long authorityGeneration = spotNode == null
            ? 0L
            : spotNode.actorAuthorityOwnerGeneration(backendRef);
        long ownerLeaseGeneration = spotNode == null
            ? 0L
            : spotNode.actorAuthorityOwnerLeaseGeneration(backendRef);
        long bindingGeneration = actor.bindingGeneration();
        BindingCleanup cleanup = inStateLane(() -> {
            List<HeldIngress> abandoned = List.of();
            SealTerminal abandonedSeal = null;
            if (relocationStopped) {
                throw new ZLinkConfigurationException(
                    "Session is closed while installing an Actor binding");
            }
            bound.add(actor);
            TargetOutboundBinding previousOutbound =
                targetOutboundBindings.remove(actor.actorId());
            if (previousOutbound != null) {
                previousOutbound.stop(TargetOutboundSettlement.SHUTDOWN);
            }
            bindingRoutes.put(actor.actorId(), new StoredBindingRoute(
                current.actorId(),
                current.objectGeneration(),
                current.meshName(),
                current.nodeRid(),
                nodeGeneration,
                authorityGeneration,
                ownerLeaseGeneration,
                bindingGeneration,
                0));
            IngressGate previous = ingressGates.put(
                actor.actorId(), new IngressGate(
                    current.objectGeneration(), bindingGeneration));
            if (previous != null) {
                abandoned = previous.detachHeld();
                if (previous.seal != null) {
                    SessionRelocationKey previousKey =
                        new SessionRelocationKey(
                            previous.seal,
                            actor.actorId(),
                            previous.objectGeneration,
                            sessionRid,
                            previous.bindingGeneration);
                    SealTerminal terminal = sealTerminals.get(previousKey);
                    if (terminal != null && !terminal.consumed()) {
                        terminal.consume();
                        abandonedSeal = terminal;
                    }
                }
            }
            return new BindingCleanup(abandoned, abandonedSeal);
        });
        failHeld(cleanup.held(), new ZLinkConfigurationException(
            "Session binding changed while relocation ingress was held: "
                + actor.actorId()));
        if (cleanup.seal() != null) {
            cleanup.seal().fail(new ZLinkConfigurationException(
                "Session binding changed before relocation seal drained: "
                    + actor.actorId()));
        }
        return actor;
    }

    private void removeBinding(ZLinkSessionActor actor) {
        BindingCleanup cleanup = inStateLane(() -> {
            if (!bound.remove(actor)) {
                return null;
            }
                List<HeldIngress> abandoned = List.of();
                SealTerminal abandonedSeal = null;
                boolean replacementExists = bound.stream().anyMatch(
                    candidate -> candidate.actorId().equals(actor.actorId()));
                if (!replacementExists) {
                    bindingRoutes.remove(actor.actorId());
                    TargetOutboundBinding removedOutbound =
                        targetOutboundBindings.remove(actor.actorId());
                    if (removedOutbound != null) {
                        removedOutbound.stop(
                            TargetOutboundSettlement.SHUTDOWN);
                    }
                    IngressGate removed = ingressGates.remove(actor.actorId());
                    if (removed != null) {
                        abandoned = removed.detachHeld();
                        if (removed.seal != null) {
                            SealTerminal terminal = sealTerminals.get(
                                new SessionRelocationKey(
                                    removed.seal,
                                    actor.actorId(),
                                    removed.objectGeneration,
                                    sessionRid,
                                    removed.bindingGeneration));
                            if (terminal != null && !terminal.consumed()) {
                                terminal.consume();
                                abandonedSeal = terminal;
                            }
                        }
                    }
                }
                return new BindingCleanup(abandoned, abandonedSeal);
        });
        if (cleanup != null) {
            failHeld(cleanup.held(), new ZLinkConfigurationException(
                "bound Actor was removed while relocation ingress was held: "
                    + actor.actorId()));
            if (cleanup.seal() != null) {
                cleanup.seal().fail(new ZLinkConfigurationException(
                    "bound Actor was removed before relocation seal drained: "
                        + actor.actorId()));
            }
        }
    }

    private record BindingCleanup(
        List<HeldIngress> held,
        SealTerminal seal) {
    }

    /**
     * Accepts one Session-to-Actor ingress record or holds it behind the
     * relocation seal. This method and command 42 use {@link #sealTerminals}
     * as their shared linearization lock, so the ACK high-water cannot race a
     * post-seal relay into the captured prefix.
     */
    private CompletionStage<Void> admitIngress(
        ZLinkBoundActor actor,
        LongFunction<CompletionStage<Void>> operation) {
        Objects.requireNonNull(operation, "operation");
        IngressAdmissionState admission = inStateLane(() -> {
            CompletableFuture<Void> held = null;
            IngressGate admittedGate = null;
            long acceptedSequence = 0;
            if (!isCurrentBinding(actor)) {
                return IngressAdmissionState.failed(
                    new ZLinkConfigurationException(
                        "bound Actor is no longer the current Session binding: "
                            + actor.actorId()));
            }
            StoredBindingRoute route = bindingRoutes.get(actor.actorId());
            IngressGate gate = ingressGates.get(actor.actorId());
            if (route == null || gate == null) {
                return IngressAdmissionState.failed(
                    new ZLinkConfigurationException(
                        "Session ingress binding state is unavailable: "
                            + actor.actorId()));
            }
            if (gate.seal != null) {
                held = new CompletableFuture<>();
                gate.held.addLast(new HeldIngress(actor, operation, held));
            } else {
                long accepted = allocateIngressSequence(gate);
                gate.activeIngress++;
                admittedGate = gate;
                acceptedSequence = accepted;
                bindingRoutes.put(actor.actorId(), route.withAcceptedHighWater(
                    accepted));
            }
            return new IngressAdmissionState(held, admittedGate, acceptedSequence,
                null);
        });
        if (admission.failure() != null) {
            return CompletableFuture.failedFuture(admission.failure());
        }
        if (admission.held() != null) {
            return admission.held();
        }
        IngressGate completionGate = admission.gate();
        CompletionStage<Void> submission =
            invokeIngress(operation, admission.sequence());
        submission.whenComplete((ignored, failure) ->
            completeIngress(actor, completionGate));
        return submission;
    }

    private record IngressAdmissionState(
        CompletableFuture<Void> held,
        IngressGate gate,
        long sequence,
        RuntimeException failure) {
        private static IngressAdmissionState failed(RuntimeException failure) {
            return new IngressAdmissionState(null, null, 0, failure);
        }
    }

    private long allocateIngressSequence(IngressGate gate) {
        long sequence = stream.allocateBoundSessionIngressSequence();
        if (sequence == 0) {
            if (nextFallbackIngressSequence <= 0
                || nextFallbackIngressSequence == Long.MAX_VALUE) {
                throw new ZLinkConfigurationException(
                    "Session ingress sequence is exhausted");
            }
            sequence = nextFallbackIngressSequence++;
        }
        return gate.accept(sequence);
    }

    private void completeIngress(
        ZLinkBoundActor actor,
        IngressGate admittedGate) {
        SealTerminal drained = inStateLane(() -> {
            SealTerminal candidateDrained = null;
            if (ingressGates.get(actor.actorId()) == admittedGate
                && admittedGate.activeIngress > 0) {
                admittedGate.activeIngress--;
                if (admittedGate.activeIngress == 0
                    && admittedGate.seal != null) {
                    SealTerminal terminal = sealTerminals.get(
                        new SessionRelocationKey(
                            admittedGate.seal,
                            actor.actorId(),
                            admittedGate.objectGeneration,
                            sessionRid,
                            admittedGate.bindingGeneration));
                    if (terminal != null
                        && !terminal.completion().isDone()
                        && !terminal.consumed()) {
                        candidateDrained = terminal;
                    }
                }
            }
            return candidateDrained;
        });
        if (drained != null) {
            drained.completeSealed();
        }
    }

    private static CompletionStage<Void> invokeIngress(
        LongFunction<CompletionStage<Void>> operation,
        long sourceSessionSequence) {
        try {
            return Objects.requireNonNull(operation.apply(sourceSessionSequence),
                "Session ingress operation stage");
        } catch (RuntimeException failure) {
            return CompletableFuture.failedFuture(failure);
        }
    }

    private void resumeHeld(List<HeldIngress> held) {
        CompletionStage<Void> tail = CompletableFuture.completedFuture(null);
        for (HeldIngress pending : held) {
            tail = tail.handle((ignored, failure) -> null)
                .thenComposeAsync(ignored ->
                    admitIngress(pending.actor(), pending.operation()))
                .whenComplete((ignored, failure) -> {
                    if (failure == null) {
                        pending.result().complete(null);
                    } else {
                        pending.result().completeExceptionally(failure);
                    }
                });
        }
    }

    private static void failHeld(
        List<HeldIngress> held,
        RuntimeException failure) {
        held.forEach(pending -> pending.result().completeExceptionally(failure));
    }

    private ZLinkBoundActor currentBoundActor(String actorId) {
        return bound.stream()
            .filter(candidate -> candidate.actorId().equals(actorId))
            .map(ZLinkBoundActor.class::cast)
            .findFirst()
            .orElseThrow(() -> new ZLinkConfigurationException(
                "bound Actor is unavailable: " + actorId));
    }

    private record TargetAuthorityFence(
        long nodeGeneration,
        long authorityOwnerGeneration) {
    }

    /**
     * Applies relocation command 42 at this Session owner and answers with
     * command 43. Ported from the C++ session-owner handler
     * (`framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp:3974-4078`)
     * in the same order: cached-terminal lookup keyed by relocation id
     * (identical retransmit re-sends the cached ACK, a conflicting seal is
     * refused), cache bound, the binding fence check that C++ performs in
     * `stream_session_registry_t::seal_remote_route`
     * (`stream_session_registry.cpp:330`), then the ACK that echoes every
     * command 42 field plus the owner's accepted high-water.
     *
     * <p>A refusal completes exceptionally so the seal stays unanswered and
     * the source retransmits on the spec 20 §5 step 8 schedule, mirroring the
     * C++ handler's `continue`.</p>
     */
    public CompletionStage<ZLinkServiceM6BWireCodec.SessionRelocationSealed>
        applyRelocationSealCommand(
            ZLinkServiceM6BWireCodec.SessionRelocationSeal command) {
        return onStateLane(() -> applyRelocationSealCommandCore(command))
            .thenCompose(stage -> stage);
    }

    private CompletionStage<ZLinkServiceM6BWireCodec.SessionRelocationSealed>
        applyRelocationSealCommandCore(
            ZLinkServiceM6BWireCodec.SessionRelocationSeal command) {
        Objects.requireNonNull(command, "command");
        if (!sessionRid.equals(command.session().sessionRid())) {
            return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                "Session relocation seal command does not target this Session"));
        }
        if (relocationStopped) {
            return CompletableFuture.failedFuture(
                new ZLinkConfigurationException(
                    "Session relocation owner is stopped"));
        }
        ZLinkServiceM6BWireCodec.SessionRelocationSealed sealed;
        SealTerminal installed;
        SessionRelocationKey key = relocationKey(command);
            SealTerminal cached = sealTerminals.get(key);
            if (cached != null) {
                if (!cached.seal().equals(command)) {
                    return CompletableFuture.failedFuture(
                        new ZLinkConfigurationException(
                            ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                            "Session relocation seal conflicts with the "
                                + "recorded seal for this relocation"));
                }
                return cached.completion();
            }
            if (sealTerminals.size() >= SEAL_TERMINAL_CAPACITY
                && !evictOneSealTerminal()) {
                return CompletableFuture.failedFuture(
                    new ZLinkConfigurationException(
                        "Session relocation seal terminal store is full"));
            }
            StoredBindingRoute observed =
                bindingRoutes.get(command.actor().actor().actorId());
            IngressGate gate = ingressGates.get(
                command.actor().actor().actorId());
            if (!sealFenceMatches(command, observed)
                || gate == null
                || gate.objectGeneration != command.actor().actor().generation()
                || gate.bindingGeneration
                    != command.session().bindingGeneration()) {
                LOGGER.warning("[zlink-java-stream-trace] session-seal stale-fence"
                    + " refused actor=" + command.actor().actor().actorId()
                    + " commandBinding=" + command.session().bindingGeneration()
                    + " commandGeneration=" + command.actor().actor().generation()
                    + " commandNode=" + command.actor().actor().nodeRid()
                    + " commandAuthority="
                    + command.actor().authorityOwnerGeneration()
                    + (observed == null
                        ? " observed=none"
                        : " observedBinding=" + observed.bindingGeneration()
                            + " observedGeneration=" + observed.objectGeneration()
                            + " observedNode=" + observed.nodeRid()
                            + " observedAuthority="
                            + observed.authorityOwnerGeneration()));
                return CompletableFuture.failedFuture(
                    new ZLinkConfigurationException(
                        "Session relocation seal fence differs from the current "
                            + "binding: " + command.actor().actor().actorId()));
            }
            if (gate.seal != null) {
                return CompletableFuture.failedFuture(
                    new ZLinkConfigurationException(
                        ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                        "Session binding is already sealed by another relocation: "
                            + command.actor().actor().actorId()));
            }
            observed = observed.withSealFence(command.actor());
            bindingRoutes.put(command.actor().actor().actorId(), observed);
            sealed = new ZLinkServiceM6BWireCodec.SessionRelocationSealed(
                command.relocation(),
                command.coordinator(),
                command.actor(),
                command.session());
            gate.seal = command.relocation();
            String actorId = command.actor().actor().actorId();
            TargetOutboundBinding outbound =
                targetOutboundBindings.get(actorId);
            if (outbound == null) {
                outbound = new TargetOutboundBinding(
                    command, targetOutboundCapacity);
                targetOutboundBindings.put(actorId, outbound);
            } else if (!outbound.matchesBinding(command)) {
                return CompletableFuture.failedFuture(
                    new ZLinkConfigurationException(
                        "Session relocation seal differs from the active "
                            + "outbound binding: " + actorId));
            }
            installed = new SealTerminal(command, sealed, outbound);
            sealTerminals.put(key, installed);
            installed.armDeadline(
                () -> expireRelocationSeal(installed),
                sessionRelocationSealTimeout);
        pruneSpentSealTerminals();
        if (STREAM_TRACE) {
            LOGGER.warning("[zlink-java-stream-trace] session-seal recorded"
                + " actor=" + command.actor().actor().actorId()
                + " binding=" + command.session().bindingGeneration());
        }
        if (ingressDrainedCore(command)) {
            installed.completeSealed();
        }
        return installed.completion();
    }

    private void expireRelocationSeal(SealTerminal terminal) {
        //  Spec 32-framework-error-model:38 — a live operation past its deadline
        //  is DeadlineExceeded, not a configuration error.
        RuntimeException failure = new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
            "Session relocation route update timed out: "
                + terminal.seal().actor().actor().actorId());
        if (!stopRelocationOwner(failure, terminal)) {
            return;
        }

        List<ZLinkSessionActor> current = inStateLane(() -> {
            List<ZLinkSessionActor> snapshot = List.copyOf(bound);
            bound.clear();
            return snapshot;
        });
        try {
            stream.disconnectPeer(sessionRid);
        } catch (RuntimeException disconnectFailure) {
            LOGGER.warning("[zlink-java-stream-trace] session relocation "
                + "timeout transport disconnect failed session=" + sessionRid
                + " error=" + disconnectFailure.getMessage());
        }
        current.forEach(actor ->
            notifyDisconnectedSafely(actor, RELAY_SUBMIT_TIMEOUT));
    }

    private boolean ingressDrained(
        ZLinkServiceM6BWireCodec.SessionRelocationSeal command) {
        return inStateLane(() -> ingressDrainedCore(command));
    }

    private boolean ingressDrainedCore(
        ZLinkServiceM6BWireCodec.SessionRelocationSeal command) {
        IngressGate gate = ingressGates.get(
            command.actor().actor().actorId());
        return gate != null
            && command.relocation().equals(gate.seal)
            && gate.activeIngress == 0;
    }

    /** Returns whether this Session binding is the unique owner for command 36. */
    public boolean matchesBoundSessionSend(
        RoutingId sourceNodeRid,
        long sourceNodeGeneration,
        ZLinkServiceM6BWireCodec.BoundSessionSend command) {
        Objects.requireNonNull(sourceNodeRid, "sourceNodeRid");
        Objects.requireNonNull(command, "command");
        return inStateLane(() ->
            outboundTargetLocked(
                    sourceNodeRid, sourceNodeGeneration, command) != null
                || matchesCurrentBoundSessionSendLocked(
                    sourceNodeRid, sourceNodeGeneration, command));
    }

    /** Admits one immutable command-36 payload into the Session-owned FIFO. */
    public boolean acceptBoundSessionSend(
        RoutingId sourceNodeRid,
        long sourceNodeGeneration,
        ZLinkServiceM6BWireCodec.BoundSessionSend command,
        ZLinkServiceM6AWireCodec.ApplicationPayload payload) {
        Objects.requireNonNull(payload, "payload");
        CurrentOutboundAdmission admission = inStateLane(() -> {
            TargetOutboundTarget target = outboundTargetLocked(
                sourceNodeRid, sourceNodeGeneration, command);
            if (target != null) {
                return new CurrentOutboundAdmission(target, target.binding().admit(
                    target.epoch(), command, payload), false);
            } else if (matchesCurrentBoundSessionSendLocked(
                    sourceNodeRid, sourceNodeGeneration, command)) {
                return new CurrentOutboundAdmission(null, null, true);
            }
            return new CurrentOutboundAdmission(null, null, false);
        });
        if (admission.current()) {
            return deliverCurrentBoundSessionSendLocked(payload);
        }
        if (admission.admission() == null || !admission.admission().admitted()) {
            return false;
        }
        startTargetOutboundDrain(admission.target().binding());
        return true;
    }

    /** Admits command 36 without blocking its infrastructure receive owner. */
    public CompletionStage<Boolean> acceptBoundSessionSendAsync(
        RoutingId sourceNodeRid,
        long sourceNodeGeneration,
        ZLinkServiceM6BWireCodec.BoundSessionSend command,
        ZLinkServiceM6AWireCodec.ApplicationPayload payload) {
        Objects.requireNonNull(payload, "payload");
        return stateLane.<AsyncBoundSessionAdmission>runAsync(() -> {
            TargetOutboundTarget target = outboundTargetLocked(
                sourceNodeRid, sourceNodeGeneration, command);
            if (target != null) {
                TargetOutboundAdmission admission = target.binding().admit(
                    target.epoch(), command, payload);
                return new AsyncBoundSessionAdmission(
                    CompletableFuture.completedFuture(
                        admission.admitted()),
                    admission.admitted() ? target.binding() : null);
            } else if (matchesCurrentBoundSessionSendLocked(
                    sourceNodeRid, sourceNodeGeneration, command)) {
                return new AsyncBoundSessionAdmission(
                    deliverCurrentBoundSessionSendAsync(payload), null);
            }
            return new AsyncBoundSessionAdmission(
                CompletableFuture.completedFuture(false), null);
        }).thenCompose(admission -> {
            if (admission.drain() != null) {
                startTargetOutboundDrain(admission.drain());
            }
            return admission.completion();
        });
    }

    TargetOutboundAdmission admitBoundSessionSend(
        RoutingId sourceNodeRid,
        long sourceNodeGeneration,
        ZLinkServiceM6BWireCodec.BoundSessionSend command,
        ZLinkServiceM6AWireCodec.ApplicationPayload payload) {
        Objects.requireNonNull(payload, "payload");
        CurrentOutboundAdmission state = inStateLane(() -> {
            TargetOutboundTarget target = outboundTargetLocked(
                sourceNodeRid, sourceNodeGeneration, command);
            if (target == null) {
                return new CurrentOutboundAdmission(null,
                    TargetOutboundAdmission.rejected(
                        TargetOutboundSettlement.REJECTED), false);
            }
            return new CurrentOutboundAdmission(target, target.binding().admit(
                target.epoch(), command, payload), false);
        });
        TargetOutboundAdmission admission = state.admission();
        if (admission.admitted()) {
            startTargetOutboundDrain(state.target().binding());
        }
        return admission;
    }

    private record CurrentOutboundAdmission(
        TargetOutboundTarget target,
        TargetOutboundAdmission admission,
        boolean current) {
    }

    private record AsyncBoundSessionAdmission(
        CompletionStage<Boolean> completion,
        TargetOutboundBinding drain) {
    }

    private TargetOutboundTarget outboundTargetLocked(
        RoutingId sourceNodeRid,
        long sourceNodeGeneration,
        ZLinkServiceM6BWireCodec.BoundSessionSend command) {
        var target = command.actor();
        // sourceNodeGeneration is a node lifecycle-generation opaque
        // equality token (.NET ulong, spec 01-glossary "Lifecycle
        // generation"): full range, only zero is unassigned. A signed
        // `<= 0` sentinel wrongly rejects a legitimate negative-as-long
        // value.
        if (!sourceNodeRid.equals(target.actor().nodeRid())
            || sourceNodeGeneration == 0
            || sourceNodeGeneration != target.targetNodeGeneration()) {
            return null;
        }
        TargetOutboundBinding binding = targetOutboundBindings.get(
            target.actor().actorId());
        if (binding == null || binding.stopped) {
            return null;
        }
        TargetOutboundTarget owner = binding.matchesApplied(
                sourceNodeRid, sourceNodeGeneration, command)
            ? new TargetOutboundTarget(binding, binding.currentEpoch)
            : null;
        for (SealTerminal candidate : sealTerminals.values()) {
            var seal = candidate.seal();
            boolean sameIdentity = seal.actor().actor().actorId().equals(
                    target.actor().actorId())
                && seal.actor().actor().generation()
                    == target.actor().generation()
                && seal.session().bindingGeneration()
                    == command.expectedBindingGeneration();
            TargetOutboundEpoch epoch = candidate.epoch();
            boolean producerPending = !candidate.consumed()
                && candidate.completion().isDone()
                && epoch.matchesProducerPending(
                    sourceNodeRid, sourceNodeGeneration, command);
            if (!sameIdentity || !producerPending
                || epoch.binding != binding || epoch.aborted) {
                continue;
            }
            TargetOutboundTarget candidateOwner =
                new TargetOutboundTarget(binding, epoch);
            if (owner != null && owner.epoch() != epoch) {
                return null;
            }
            owner = candidateOwner;
        }
        return owner;
    }

    private boolean matchesCurrentBoundSessionSendLocked(
        RoutingId sourceNodeRid,
        long sourceNodeGeneration,
        ZLinkServiceM6BWireCodec.BoundSessionSend command) {
        if (relocationStopped || sourceNodeGeneration == 0) {
            return false;
        }
        var target = command.actor();
        String actorId = target.actor().actorId();
        StoredBindingRoute route = bindingRoutes.get(actorId);
        return route != null
            && route.actorId().equals(actorId)
            && route.objectGeneration() == target.actor().generation()
            && route.nodeRid().equals(sourceNodeRid)
            && route.nodeRid().equals(target.actor().nodeRid())
            && route.nodeGeneration() == sourceNodeGeneration
            && route.nodeGeneration() == target.targetNodeGeneration()
            && route.authorityOwnerGeneration()
                == target.authorityOwnerGeneration()
            && route.bindingGeneration()
                == command.expectedBindingGeneration();
    }

    private boolean deliverCurrentBoundSessionSendLocked(
        ZLinkServiceM6AWireCodec.ApplicationPayload payload) {
        List<Message> parts =
            ZLinkServiceM6AWireCodec.decodeFrameworkMultipart(payload);
        try {
            return stream.sendBoundSessionPush(
                sessionRid, parts, SendFlags.DONT_WAIT);
        } finally {
            parts.forEach(Message::close);
        }
    }

    private CompletionStage<Boolean> deliverCurrentBoundSessionSendAsync(
        ZLinkServiceM6AWireCodec.ApplicationPayload payload) {
        List<Message> parts =
            ZLinkServiceM6AWireCodec.decodeFrameworkMultipart(payload);
        try {
            return stream.sendBoundSessionPushAsync(sessionRid, parts)
                .thenApply(ignored -> true);
        } catch (RuntimeException failure) {
            return CompletableFuture.failedFuture(failure);
        } finally {
            parts.forEach(Message::close);
        }
    }

    private void startTargetOutboundDrain(TargetOutboundBinding owner) {
        TargetOutboundEntry pending = inStateLane(() -> {
            if (owner == null || owner.stopped
                || owner.drainRunning || !owner.headIsDeliverable()) {
                return null;
            }
            owner.drainRunning = true;
            return owner.queue.peekFirst();
        });
        if (pending == null) {
            return;
        }
        CompletionStage<Void> submission;
        List<Message> parts = List.of();
        try {
            parts = ZLinkServiceM6AWireCodec.decodeFrameworkMultipart(
                pending.payload());
            submission = stream.sendBoundSessionPushAsync(
                sessionRid, parts);
        } catch (RuntimeException failure) {
            submission = CompletableFuture.failedFuture(failure);
        } finally {
            parts.forEach(Message::close);
        }
        CompletableFuture<Void> resolvedPhysical;
        try {
            resolvedPhysical = Objects.requireNonNull(
                submission.toCompletableFuture(),
                "physical STREAM admission future");
        } catch (RuntimeException failure) {
            resolvedPhysical = CompletableFuture.failedFuture(failure);
        }
        CompletableFuture<Void> physical = resolvedPhysical;
        boolean cancel = inStateLane(() -> {
            boolean cancelled = owner.stopped || owner.queue.peekFirst() != pending;
            if (cancelled) {
                owner.drainRunning = false;
            } else {
                owner.physicalDrain = physical;
                owner.physicalEntry = pending;
            }
            return cancelled;
        });
        if (cancel) {
            physical.cancel(false);
            return;
        }
        physical.whenComplete((ignored, failure) -> {
            DrainCompletion completion = inStateLane(() -> {
                if (owner.physicalDrain != physical) {
                    return null;
                }
                owner.physicalDrain = null;
                owner.physicalEntry = null;
                owner.drainRunning = false;
                TargetOutboundSettlement settlement = null;
                if (owner.queue.peekFirst() == pending) {
                    owner.queue.removeFirst();
                    settlement = failure == null
                        ? TargetOutboundSettlement.DELIVERED
                        : TargetOutboundSettlement.REJECTED;
                }
                return new DrainCompletion(settlement,
                    !owner.stopped && owner.headIsDeliverable());
            });
            if (completion == null) {
                return;
            }
            if (completion.settlement() != null) {
                pending.settle(completion.settlement());
            }
            if (completion.continueDrain()) {
                CompletableFuture.runAsync(
                    () -> startTargetOutboundDrain(owner));
            }
        });
    }

    private record DrainCompletion(
        TargetOutboundSettlement settlement,
        boolean continueDrain) {
    }

    private boolean sealFenceMatches(
        ZLinkServiceM6BWireCodec.SessionRelocationSeal command,
        StoredBindingRoute observed) {
        if (observed == null) {
            return false;
        }
        var actor = command.actor();
        boolean storedRouteMatches = observed.bindingGeneration()
                == command.session().bindingGeneration()
            && observed.objectGeneration() == actor.actor().generation()
            && observed.nodeRid().equals(actor.actor().nodeRid());
        return storedRouteMatches;
    }

    //  Evicts one terminal so a new seal can be recorded: a spent one first,
    //  mirroring C++ (public_host_runtime.cpp:4008-4019); if every terminal is
    //  still in flight the store is genuinely full and the seal is refused.
    private boolean evictOneSealTerminal() {
        var iterator = sealTerminals.entrySet().iterator();
        while (iterator.hasNext()) {
            var entry = iterator.next();
            if (entry.getValue().consumed()) {
                iterator.remove();
                return true;
            }
        }
        return false;
    }

    //  A spent terminal only exists to answer a command 42 that was still in
    //  flight when its route committed, so a small insertion-ordered tail is
    //  enough. C++ can keep every spent terminal because its store dies with
    //  the host process; this one lives inside a bound Session that can
    //  outlive thousands of relocations.
    private void pruneSpentSealTerminals() {
        int spent = 0;
        for (SealTerminal terminal : sealTerminals.values()) {
            if (terminal.consumed()) {
                spent++;
            }
        }
        var iterator = sealTerminals.entrySet().iterator();
        while (spent > SPENT_SEAL_TERMINAL_RETENTION && iterator.hasNext()) {
            var entry = iterator.next();
            if (entry.getValue().consumed()) {
                iterator.remove();
                spent--;
            }
        }
    }

    //  Session owner가 seal을 대조할 때 쓰는 값만 본다 — Session과 Actor binding 8.1
    //  "Session owner는 다음 값만 검증한다": 현재 Session identity와 SessionRid, 현재
    //  binding generation과 ActorId/ObjectGeneration, 같은 relocation인지 구분하는
    //  relocation identity. coordinator identity는 transport가 검증하는 wire fence이므로
    //  여기서 다시 대조하지 않는다.
    private static boolean sealMatchesRoute(
        ZLinkServiceM6BWireCodec.SessionRelocationSeal seal,
        ZLinkServiceM6BWireCodec.SessionRelocationRoute route) {
        return seal.relocation().equals(route.relocation())
            && seal.session().equals(route.session())
            && seal.actor().actor().actorId().equals(route.actor().actorId())
            && seal.actor().actor().generation() == route.actor().generation();
    }

    private static final class SealTerminal {
        private final ZLinkServiceM6BWireCodec.SessionRelocationSeal seal;
        private final ZLinkServiceM6BWireCodec.SessionRelocationSealed sealed;
        private final CompletableFuture<
            ZLinkServiceM6BWireCodec.SessionRelocationSealed> completion =
                new CompletableFuture<>();
        private final TargetOutboundEpoch outbound;
        private boolean consumed;
        private CompletableFuture<Void> deadline;

        private SealTerminal(
            ZLinkServiceM6BWireCodec.SessionRelocationSeal seal,
            ZLinkServiceM6BWireCodec.SessionRelocationSealed sealed,
            TargetOutboundBinding binding) {
            this.seal = seal;
            this.sealed = sealed;
            this.outbound = new TargetOutboundEpoch(seal, binding);
        }

        private ZLinkServiceM6BWireCodec.SessionRelocationSeal seal() {
            return seal;
        }

        private ZLinkServiceM6BWireCodec.SessionRelocationSealed sealed() {
            return sealed;
        }

        private CompletableFuture<
            ZLinkServiceM6BWireCodec.SessionRelocationSealed> completion() {
            return completion;
        }

        private boolean consumed() {
            return consumed;
        }

        private TargetOutboundEpoch epoch() {
            return outbound;
        }

        private void consume() {
            consumed = true;
            cancelDeadline();
        }

        private void completeSealed() {
            completion.completeAsync(() -> sealed);
        }

        private void armDeadline(Runnable expiration, Duration timeout) {
            if (deadline == null && !consumed) {
                deadline = ZLinkActorRetryScheduler.scheduleAfter(
                    expiration, timeout);
            }
        }

        private void cancelDeadline() {
            if (deadline != null) {
                deadline.cancel(false);
                deadline = null;
            }
        }

        private void fail(Throwable failure) {
            cancelDeadline();
            rejectOutbound();
            completion.completeExceptionally(failure);
        }

        private void rejectOutbound() {
            outbound.binding.reject(outbound);
        }
    }

    enum TargetOutboundSettlement {
        DELIVERED,
        REJECTED,
        SHUTDOWN,
        BACKPRESSURED
    }

    record TargetOutboundAdmission(
        boolean admitted,
        CompletionStage<TargetOutboundSettlement> settlement) {
        private static TargetOutboundAdmission rejected(
            TargetOutboundSettlement result) {
            return new TargetOutboundAdmission(
                false, CompletableFuture.completedFuture(result));
        }
    }

    private enum TargetOutboundEntryState {
        PENDING,
        ACCEPTED,
        SETTLED
    }

    private static final class TargetOutboundEntry {
        private final ZLinkServiceM6BWireCodec.BoundSessionSend command;
        private final ZLinkServiceM6AWireCodec.ApplicationPayload payload;
        private final TargetOutboundEpoch epoch;
        private final CompletableFuture<TargetOutboundSettlement> settlement =
            new CompletableFuture<>();
        private final AtomicBoolean settled = new AtomicBoolean();
        private TargetOutboundEntryState state;

        private TargetOutboundEntry(
            ZLinkServiceM6BWireCodec.BoundSessionSend command,
            ZLinkServiceM6AWireCodec.ApplicationPayload payload,
            TargetOutboundEpoch epoch,
            TargetOutboundEntryState state) {
            this.command = Objects.requireNonNull(command, "command");
            this.payload = Objects.requireNonNull(payload, "payload");
            this.epoch = Objects.requireNonNull(epoch, "epoch");
            this.state = Objects.requireNonNull(state, "state");
        }

        private ZLinkServiceM6AWireCodec.ApplicationPayload payload() {
            return payload;
        }

        private boolean deliverable() {
            return state == TargetOutboundEntryState.ACCEPTED
                && epoch.applied && !epoch.aborted;
        }

        private boolean accept(
            ZLinkServiceM6BWireCodec.SessionRelocationRoute route,
            TargetAuthorityFence fence) {
            if (!epoch.matchesAccepted(command, route, fence)) {
                settle(TargetOutboundSettlement.REJECTED);
                return false;
            }
            state = TargetOutboundEntryState.ACCEPTED;
            return true;
        }

        private void settle(TargetOutboundSettlement result) {
            if (settled.compareAndSet(false, true)) {
                state = TargetOutboundEntryState.SETTLED;
                settlement.completeAsync(() -> result);
            }
        }
    }

    private static final class TargetOutboundEpoch {
        private final ZLinkServiceM6BWireCodec.SessionRelocationSeal seal;
        private final TargetOutboundBinding binding;
        private ZLinkServiceM6BWireCodec.SessionRelocationRoute appliedRoute;
        private TargetAuthorityFence acceptedFence;
        private boolean applied;
        private boolean aborted;

        private TargetOutboundEpoch(
            ZLinkServiceM6BWireCodec.SessionRelocationSeal seal,
            TargetOutboundBinding binding) {
            this.seal = Objects.requireNonNull(seal, "seal");
            this.binding = Objects.requireNonNull(binding, "binding");
        }

        private boolean matchesProducerPending(
            RoutingId sourceNodeRid,
            long sourceNodeGeneration,
            ZLinkServiceM6BWireCodec.BoundSessionSend command) {
            if (aborted) {
                return false;
            }
            var actor = command.actor();
            return sourceNodeRid.equals(actor.actor().nodeRid())
                && sourceNodeGeneration == actor.targetNodeGeneration()
                && actor.authorityOwnerGeneration()
                    > seal.actor().authorityOwnerGeneration()
                && actor.ownerLeaseGeneration() > 0;
        }

        private boolean matchesAccepted(
            ZLinkServiceM6BWireCodec.BoundSessionSend command,
            ZLinkServiceM6BWireCodec.SessionRelocationRoute route,
            TargetAuthorityFence fence) {
            if (route == null || fence == null) {
                return false;
            }
            var actor = command.actor();
            return actor.actor().nodeRid().equals(route.targetNodeRid())
                && actor.targetNodeGeneration()
                    == route.targetNodeGeneration()
                && actor.actor().actorId().equals(
                    route.actor().actorId())
                && actor.actor().generation()
                    == route.actor().generation()
                && actor.targetNodeGeneration()
                    == fence.nodeGeneration()
                && actor.authorityOwnerGeneration()
                    == fence.authorityOwnerGeneration()
                && command.expectedBindingGeneration()
                    == route.session().bindingGeneration();
        }
    }

    private static final class TargetOutboundBinding {
        private final String actorId;
        private final long objectGeneration;
        private final RoutingId sessionRid;
        private final long bindingGeneration;
        private final int capacity;
        private final ArrayDeque<TargetOutboundEntry> queue =
            new ArrayDeque<>();
        private TargetOutboundEpoch currentEpoch;
        private CompletableFuture<Void> physicalDrain;
        private TargetOutboundEntry physicalEntry;
        private boolean drainRunning;
        private boolean stopped;

        private TargetOutboundBinding(
            ZLinkServiceM6BWireCodec.SessionRelocationSeal seal,
            int capacity) {
            actorId = seal.actor().actor().actorId();
            objectGeneration = seal.actor().actor().generation();
            sessionRid = seal.session().sessionRid();
            bindingGeneration = seal.session().bindingGeneration();
            this.capacity = capacity;
        }

        private boolean matchesBinding(
            ZLinkServiceM6BWireCodec.SessionRelocationSeal seal) {
            return actorId.equals(seal.actor().actor().actorId())
                && objectGeneration == seal.actor().actor().generation()
                && sessionRid.equals(seal.session().sessionRid())
                && bindingGeneration == seal.session().bindingGeneration();
        }

        private boolean matchesApplied(
            RoutingId sourceNodeRid,
            long sourceNodeGeneration,
            ZLinkServiceM6BWireCodec.BoundSessionSend command) {
            return currentEpoch != null && currentEpoch.applied
                && !currentEpoch.aborted
                && sourceNodeRid.equals(command.actor().actor().nodeRid())
                && sourceNodeGeneration
                    == command.actor().targetNodeGeneration()
                && currentEpoch.matchesAccepted(
                    command,
                    currentEpoch.appliedRoute,
                    currentEpoch.acceptedFence);
        }

        private TargetOutboundAdmission admit(
            TargetOutboundEpoch epoch,
            ZLinkServiceM6BWireCodec.BoundSessionSend command,
            ZLinkServiceM6AWireCodec.ApplicationPayload payload) {
            if (stopped) {
                return TargetOutboundAdmission.rejected(
                    TargetOutboundSettlement.SHUTDOWN);
            }
            if (queue.size() >= capacity) {
                return TargetOutboundAdmission.rejected(
                    TargetOutboundSettlement.BACKPRESSURED);
            }
            TargetOutboundEntryState state = epoch.acceptedFence == null
                ? TargetOutboundEntryState.PENDING
                : TargetOutboundEntryState.ACCEPTED;
            TargetOutboundEntry entry = new TargetOutboundEntry(
                command, payload, epoch, state);
            if (state == TargetOutboundEntryState.ACCEPTED) {
                boolean accepted =
                    entry.accept(epoch.appliedRoute, epoch.acceptedFence);
                if (!accepted) {
                    return new TargetOutboundAdmission(
                        false, entry.settlement);
                }
            }
            queue.addLast(entry);
            return new TargetOutboundAdmission(true, entry.settlement);
        }

        private void acceptProof(
            TargetOutboundEpoch epoch,
            ZLinkServiceM6BWireCodec.SessionRelocationRoute route,
            TargetAuthorityFence fence) {
            if (epoch.binding != this || epoch.aborted
                || !sealMatchesRoute(epoch.seal, route)) {
                throw new ZLinkConfigurationException(
                    "target outbound route differs from its Session seal");
            }
            if (epoch.acceptedFence != null
                && (!epoch.appliedRoute.equals(route)
                    || !epoch.acceptedFence.equals(fence))) {
                throw new ZLinkConfigurationException(
                    "target outbound proof changed during route apply");
            }
            epoch.appliedRoute = Objects.requireNonNull(route, "route");
            epoch.acceptedFence = Objects.requireNonNull(fence, "fence");
            var iterator = queue.iterator();
            while (iterator.hasNext()) {
                TargetOutboundEntry entry = iterator.next();
                if (entry.epoch == epoch
                    && entry.state == TargetOutboundEntryState.PENDING
                    && !entry.accept(route, fence)) {
                    iterator.remove();
                }
            }
        }

        private void apply(TargetOutboundEpoch epoch) {
            if (epoch.acceptedFence == null || epoch.aborted) {
                throw new ZLinkConfigurationException(
                    "target outbound proof was not accepted");
            }
            epoch.applied = true;
            currentEpoch = epoch;
        }

        private boolean headIsDeliverable() {
            return !stopped && !queue.isEmpty()
                && queue.peekFirst().deliverable();
        }

        private void reject(TargetOutboundEpoch epoch) {
            epoch.aborted = true;
            CompletableFuture<Void> active =
                physicalEntry != null && physicalEntry.epoch == epoch
                    ? physicalDrain
                    : null;
            if (active != null) {
                physicalDrain = null;
                physicalEntry = null;
                drainRunning = false;
            }
            var iterator = queue.iterator();
            while (iterator.hasNext()) {
                TargetOutboundEntry entry = iterator.next();
                if (entry.epoch == epoch) {
                    iterator.remove();
                    entry.settle(TargetOutboundSettlement.REJECTED);
                }
            }
            if (active != null) {
                active.cancel(false);
            }
        }

        private void stop(TargetOutboundSettlement result) {
            if (stopped) {
                return;
            }
            stopped = true;
            CompletableFuture<Void> active = physicalDrain;
            physicalDrain = null;
            physicalEntry = null;
            drainRunning = false;
            for (TargetOutboundEntry entry : queue) {
                entry.settle(result);
            }
            queue.clear();
            if (active != null) {
                active.cancel(false);
            }
        }
    }

    private record TargetOutboundTarget(
        TargetOutboundBinding binding,
        TargetOutboundEpoch epoch) {
    }

    private record SessionRelocationKey(
        ZLinkServiceM6BWireCodec.RelocationIdentity relocation,
        String actorId,
        long objectGeneration,
        RoutingId sessionRid,
        long bindingGeneration) {
    }

    private static SessionRelocationKey relocationKey(
        ZLinkServiceM6BWireCodec.SessionRelocationSeal command) {
        return new SessionRelocationKey(
            command.relocation(),
            command.actor().actor().actorId(),
            command.actor().actor().generation(),
            command.session().sessionRid(),
            command.session().bindingGeneration());
    }

    private static SessionRelocationKey relocationKey(
        ZLinkServiceM6BWireCodec.SessionRelocationRoute command) {
        return new SessionRelocationKey(
            command.relocation(),
            command.actor().actorId(),
            command.actor().generation(),
            command.session().sessionRid(),
            command.session().bindingGeneration());
    }

    private record RouteTerminal(
        ZLinkServiceM6BWireCodec.SessionRelocationRoute command) {
    }

    private record RouteFlight(
        ZLinkServiceM6BWireCodec.SessionRelocationRoute command,
        StoredBindingRoute sourceRoute,
        RelocationRouteUpdate update,
        TargetAuthorityFence targetFence,
        ZLinkBoundActor actor,
        ZLinkBackendActorRef sourceActor,
        ZLinkBackendActorRef targetActor,
        CompletableFuture<Void> completion) {
    }

    private CompletionStage<Void> cachedRouteTerminal(
        ZLinkServiceM6BWireCodec.SessionRelocationRoute command) {
        return inStateLane(() -> cachedRouteTerminalCore(command));
    }

    private CompletionStage<Void> cachedRouteTerminalCore(
        ZLinkServiceM6BWireCodec.SessionRelocationRoute command) {
        RouteTerminal terminal = routeTerminals.get(relocationKey(command));
        if (terminal == null) {
            return null;
        }
        if (!terminal.command().equals(command)) {
            return protocolRouteConflict(
                "Session relocation route conflicts with the recorded "
                    + "command 44 terminal");
        }
        return CompletableFuture.completedFuture(null);
    }

    private void recordRouteTerminalLocked(
        ZLinkServiceM6BWireCodec.SessionRelocationRoute command) {
        SessionRelocationKey key = relocationKey(command);
        RouteTerminal existing = routeTerminals.get(key);
        if (existing != null) {
            if (!existing.command().equals(command)) {
                throw new ZLinkConfigurationException(
                    ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                    "Session relocation route raced a conflicting command 44");
            }
            return;
        }
        if (routeTerminals.size() >= SEAL_TERMINAL_CAPACITY) {
            var iterator = routeTerminals.entrySet().iterator();
            if (iterator.hasNext()) {
                iterator.next();
                iterator.remove();
            }
        }
        routeTerminals.put(key, new RouteTerminal(command));
    }

    public CompletionStage<Void> applyRelocationRouteCommand(
        ZLinkServiceM6BWireCodec.SessionRelocationRoute command) {
        return onStateLane(() -> applyRelocationRouteCommandCore(command))
            .thenCompose(stage -> stage);
    }

    private CompletionStage<Void> applyRelocationRouteCommandCore(
        ZLinkServiceM6BWireCodec.SessionRelocationRoute command) {
        Objects.requireNonNull(command, "command");
        if (!sessionRid.equals(command.session().sessionRid())) {
            return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                "Session relocation route command does not target this Session"));
        }
        if (relocationStopped) {
            if (relocationSealTimedOut) {
                LOGGER.warning("late_session_route_update session="
                    + command.session().sessionRid());
                return CompletableFuture.completedFuture(null);
            }
            return CompletableFuture.failedFuture(
                new ZLinkConfigurationException(
                    "Session relocation owner is stopped"));
        }
        CompletionStage<Void> cached = cachedRouteTerminalCore(command);
        if (cached != null) {
            return cached;
        }
        if (command.action()
                != ZLinkServiceM6BWireCodec.SessionRelocationRouteAction.COMMIT) {
            return applyRelocationAbortCore(command);
        }
        final RouteFlight flight;
        if (relocationStopped) {
                if (relocationSealTimedOut) {
                    LOGGER.warning("late_session_route_update session="
                        + command.session().sessionRid());
                    return CompletableFuture.completedFuture(null);
                }
                return CompletableFuture.failedFuture(
                    new ZLinkConfigurationException(
                        "Session relocation owner is stopped"));
            }
            SessionRelocationKey key = relocationKey(command);
            RouteTerminal terminal = routeTerminals.get(key);
            if (terminal != null) {
                if (!terminal.command().equals(command)) {
                    return protocolRouteConflict(
                        "command 44 conflicts with its recorded terminal");
                }
                return CompletableFuture.completedFuture(null);
            }
            RouteFlight active = routeFlights.get(key);
            if (active != null) {
                if (!active.command().equals(command)) {
                    return protocolRouteConflict(
                        "command 44 conflicts with an in-flight route update");
                }
                return active.completion();
            }
            StoredBindingRoute observed =
                bindingRoutes.get(command.actor().actorId());
            SealTerminal seal = sealTerminals.get(key);
            if (observed == null || seal == null || seal.consumed()
                || !seal.completion().isDone()
                || !sealMatchesRoute(seal.seal(), command)
                || observed.bindingGeneration()
                    != command.session().bindingGeneration()) {
                LOGGER.warning(staleFenceDiagnostic(command, observed));
                return CompletableFuture.completedFuture(null);
            }
            RelocationRouteUpdate update;
            try {
                update = new RelocationRouteUpdate(
                    command.actor().actorId(),
                    command.actor().generation(),
                    observed.nodeRid(),
                    command.targetNodeRid(),
                    command.targetNodeGeneration(),
                    command.previousAuthorityOwnerGeneration(),
                    command.currentAuthorityOwnerGeneration(),
                    command.session().bindingGeneration());
            } catch (RuntimeException invalid) {
                LOGGER.warning(staleFenceDiagnostic(command, observed));
                return CompletableFuture.completedFuture(null);
            }
            if (!observed.matchesSource(update)) {
                LOGGER.warning(staleFenceDiagnostic(command, observed));
                return CompletableFuture.completedFuture(null);
            }
            ZLinkBackendActorRef target = new ZLinkBackendActorRef(
                update.targetNodeRid(), update.actorId(), update.objectGeneration());
            TargetAuthorityFence targetFence = new TargetAuthorityFence(
                update.targetNodeGeneration(),
                update.targetAuthorityOwnerGeneration());
            ZLinkBoundActor actor = currentBoundActor(update.actorId());
            flight = new RouteFlight(
                command,
                observed,
                update,
                targetFence,
                actor,
                new ZLinkBackendActorRef(
                    observed.nodeRid(), observed.actorId(),
                    observed.objectGeneration()),
                target,
                new CompletableFuture<>());
            routeFlights.put(key, flight);
        CompletableFuture.runAsync(() -> startRoutePreparation(flight));
        return flight.completion();
    }

    private CompletionStage<Void> applyRelocationAbort(
            ZLinkServiceM6BWireCodec.SessionRelocationRoute command) {
        return onStateLane(() -> applyRelocationAbortCore(command))
            .thenCompose(stage -> stage);
    }

    private CompletionStage<Void> applyRelocationAbortCore(
            ZLinkServiceM6BWireCodec.SessionRelocationRoute command) {
        List<HeldIngress> held;
            SessionRelocationKey key = relocationKey(command);
            RouteTerminal recorded = routeTerminals.get(key);
            if (recorded != null) {
                if (!recorded.command().equals(command)) {
                    return protocolRouteConflict(
                        "abort conflicts with its recorded route terminal");
                }
                return CompletableFuture.completedFuture(null);
            }
            RouteFlight flight = routeFlights.get(key);
            if (flight != null) {
                if (!flight.command().equals(command)) {
                    return protocolRouteConflict(
                        "abort conflicts with an in-flight route update");
                }
                return flight.completion();
            }
            SealTerminal terminal = sealTerminals.get(key);
            IngressGate gate = ingressGates.get(command.actor().actorId());
            if (terminal == null || terminal.consumed()
                || !terminal.completion().isDone()
                || !sealMatchesRoute(terminal.seal(), command)
                || gate == null
                || !command.relocation().equals(gate.seal)) {
                LOGGER.warning(staleFenceDiagnostic(command, null));
                return CompletableFuture.completedFuture(null);
            }
            terminal.rejectOutbound();
            terminal.consume();
            gate.seal = null;
            held = gate.detachHeld();
            recordRouteTerminalLocked(command);
            pruneSpentSealTerminals();
        resumeHeld(held);
        return CompletableFuture.completedFuture(null);
    }

    private CompletionStage<Void> protocolRouteConflict(String message) {
        return CompletableFuture.failedFuture(
            new ZLinkConfigurationException(
                ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                message));
    }

    private void startRoutePreparation(RouteFlight flight) {
        CompletionStage<Void> preparation;
        try {
            preparation = flight.actor().prepareNativeActorRoute(
                flight.targetActor(), RELAY_SUBMIT_TIMEOUT);
        } catch (RuntimeException failure) {
            preparation = CompletableFuture.failedFuture(failure);
        }
        preparation.whenComplete((ignored, failure) -> {
            if (failure != null) {
                compensateRouteFlight(flight, unwrapRouteFailure(failure));
            } else {
                commitPreparedRouteFlight(flight);
            }
        });
    }

    private void commitPreparedRouteFlight(RouteFlight flight) {
        RouteCommitState state = inStateLane(() -> {
            List<HeldIngress> held = null;
            TargetOutboundBinding outboundOwner = null;
            Throwable failure = null;
            SessionRelocationKey key = relocationKey(flight.command());
            StoredBindingRoute current =
                bindingRoutes.get(flight.command().actor().actorId());
            SealTerminal seal = sealTerminals.get(key);
            IngressGate gate =
                ingressGates.get(flight.command().actor().actorId());
            ZLinkBoundActor liveActor = bound.stream()
                .filter(candidate -> candidate.actorId().equals(
                    flight.command().actor().actorId()))
                .map(ZLinkBoundActor.class::cast)
                .findFirst()
                .orElse(null);
            if (routeFlights.get(key) != flight
                || current != flight.sourceRoute()
                || !current.matchesSource(flight.update())
                || liveActor != flight.actor()
                || seal == null || seal.consumed()
                || !seal.completion().isDone()
                || !sealMatchesRoute(seal.seal(), flight.command())
                || gate == null
                || !flight.command().relocation().equals(gate.seal)) {
                failure = new ZLinkConfigurationException(
                    "Session route or authority changed during native preparation: "
                        + flight.command().actor().actorId());
            } else {
                outboundOwner = seal.epoch().binding;
                outboundOwner.acceptProof(
                    seal.epoch(), flight.command(), flight.targetFence());
                flight.actor().commitPreparedNativeActorRoute(
                    flight.targetActor());
                bindingRoutes.put(
                    flight.command().actor().actorId(),
                    current.toTarget(flight.update(), flight.targetFence()));
                outboundOwner.apply(seal.epoch());
                seal.consume();
                gate.seal = null;
                held = gate.detachHeld();
                recordRouteTerminalLocked(flight.command());
                routeFlights.remove(key, flight);
                pruneSpentSealTerminals();
            }
            return new RouteCommitState(held, outboundOwner, failure);
        });
        if (state.failure() != null) {
            compensateRouteFlight(flight, state.failure());
            return;
        }
        startTargetOutboundDrain(state.outboundOwner());
        resumeHeld(state.held());
        flight.completion().completeAsync(() -> null);
    }

    private record RouteCommitState(
        List<HeldIngress> held,
        TargetOutboundBinding outboundOwner,
        Throwable failure) {
    }

    private void compensateRouteFlight(
        RouteFlight flight,
        Throwable failure) {
        CompletionStage<Void> compensation;
        try {
            compensation = flight.actor().compensatePreparedNativeActorRoute(
                flight.sourceActor(), RELAY_SUBMIT_TIMEOUT);
        } catch (RuntimeException compensationFailure) {
            compensation = CompletableFuture.failedFuture(compensationFailure);
        }
        compensation.whenComplete((ignored, compensationFailure) -> {
            Throwable terminal = failure;
            if (compensationFailure != null) {
                terminal.addSuppressed(unwrapRouteFailure(compensationFailure));
            }
            inStateLane(() -> {
                routeFlights.remove(
                    relocationKey(flight.command()), flight);
                return null;
            });
            flight.completion().completeAsync(() -> {
                throw new CompletionException(terminal);
            });
        });
    }

    private static Throwable unwrapRouteFailure(Throwable failure) {
        Throwable current = failure;
        while (current instanceof CompletionException
            && current.getCause() != null) {
            current = current.getCause();
        }
        return current;
    }

    private static String staleFenceDiagnostic(
        ZLinkServiceM6BWireCodec.SessionRelocationRoute command,
        StoredBindingRoute observed) {
        return "[zlink-java-stream-trace] session-route stale-fence rejected"
            + " actor=" + command.actor().actorId()
            + " commandBinding=" + command.session().bindingGeneration()
            + " commandPrevAuthority=" + command.previousAuthorityOwnerGeneration()
            + " commandTargetAuthority=" + command.currentAuthorityOwnerGeneration()
            + " commandTarget=" + command.targetNodeRid()
            + " commandGeneration=" + command.actor().generation()
            + (observed == null
                ? " observed=none"
                : " observedBinding=" + observed.bindingGeneration()
                    + " observedAuthority=" + observed.authorityOwnerGeneration()
                    + " observedNode=" + observed.nodeRid()
                    + " observedGeneration=" + observed.objectGeneration());
    }

    private record RelocationRouteUpdate(
        String actorId,
        long objectGeneration,
        RoutingId sourceNodeRid,
        RoutingId targetNodeRid,
        long targetNodeGeneration,
        long sourceAuthorityOwnerGeneration,
        long targetAuthorityOwnerGeneration,
        long bindingGeneration) {
        RelocationRouteUpdate {
            // targetNodeGeneration is a node lifecycle-generation opaque
            // equality token (.NET ulong, spec 01-glossary "Lifecycle
            // generation"): full range, only zero is unassigned. A signed
            // `<= 0` sentinel wrongly rejects a legitimate negative-as-long
            // value. objectGeneration/sourceAuthorityOwnerGeneration/
            // bindingGeneration/targetAuthorityOwnerGeneration are
            // spec-bounded to `1..long.MaxValue`, so `<= 0` is valid presence
            // validation; it does not establish ordering between projections.
            if (actorId == null || actorId.isBlank()
                || objectGeneration <= 0
                || targetNodeGeneration == 0
                || sourceAuthorityOwnerGeneration <= 0
                || bindingGeneration <= 0
                || targetAuthorityOwnerGeneration <= 0) {
                throw new IllegalArgumentException(
                    "relocation binding-route generations are invalid");
            }
            Objects.requireNonNull(sourceNodeRid, "sourceNodeRid");
            Objects.requireNonNull(targetNodeRid, "targetNodeRid");
        }
    }

    private record StoredBindingRoute(
        String actorId,
        long objectGeneration,
        String meshName,
        RoutingId nodeRid,
        long nodeGeneration,
        long authorityOwnerGeneration,
        long ownerLeaseGeneration,
        long bindingGeneration,
        long lastAcceptedSessionSequence) {
        boolean matchesSource(RelocationRouteUpdate update) {
            return actorId.equals(update.actorId())
                && objectGeneration == update.objectGeneration()
                && nodeRid.equals(update.sourceNodeRid())
                && bindingGeneration == update.bindingGeneration();
        }

        StoredBindingRoute toTarget(
            RelocationRouteUpdate update,
            TargetAuthorityFence targetFence) {
            return new StoredBindingRoute(
                actorId,
                objectGeneration,
                meshName,
                update.targetNodeRid(),
                targetFence.nodeGeneration(),
                targetFence.authorityOwnerGeneration(),
                0,
                bindingGeneration,
                lastAcceptedSessionSequence);
        }

        StoredBindingRoute toNativeTarget(ZLinkBackendActorRef targetActor) {
            return new StoredBindingRoute(
                actorId,
                objectGeneration,
                meshName,
                targetActor.nodeRid(),
                nodeGeneration,
                authorityOwnerGeneration,
                ownerLeaseGeneration,
                bindingGeneration,
                lastAcceptedSessionSequence);
        }

        StoredBindingRoute withAcceptedHighWater(long highWater) {
            return new StoredBindingRoute(
                actorId,
                objectGeneration,
                meshName,
                nodeRid,
                nodeGeneration,
                authorityOwnerGeneration,
                ownerLeaseGeneration,
                bindingGeneration,
                highWater);
        }

        StoredBindingRoute withSealFence(
            ZLinkServiceM6BWireCodec.ActorRouteFence fence) {
            return new StoredBindingRoute(
                actorId,
                objectGeneration,
                meshName,
                nodeRid,
                fence.targetNodeGeneration(),
                fence.authorityOwnerGeneration(),
                fence.ownerLeaseGeneration(),
                bindingGeneration,
                lastAcceptedSessionSequence);
        }
    }

    private static final class IngressGate {
        private final long objectGeneration;
        private final long bindingGeneration;
        private long acceptedHighWater;
        private long activeIngress;
        private ZLinkServiceM6BWireCodec.RelocationIdentity seal;
        private final ArrayDeque<HeldIngress> held = new ArrayDeque<>();

        private IngressGate(
            long objectGeneration,
            long bindingGeneration) {
            this.objectGeneration = objectGeneration;
            this.bindingGeneration = bindingGeneration;
        }

        private long accept(long sequence) {
            if (sequence <= 0 || sequence <= acceptedHighWater) {
                throw new ZLinkConfigurationException(
                    "Session ingress sequence is not monotonic");
            }
            acceptedHighWater = sequence;
            return sequence;
        }

        private List<HeldIngress> detachHeld() {
            if (held.isEmpty()) {
                return List.of();
            }
            List<HeldIngress> pending = new ArrayList<>(held);
            held.clear();
            return pending;
        }
    }

    private record HeldIngress(
        ZLinkBoundActor actor,
        LongFunction<CompletionStage<Void>> operation,
        CompletableFuture<Void> result) {
    }

    private boolean isCurrentBinding(ZLinkBoundActor actor) {
        return actor != null && bound.contains(actor);
    }

    private CompletionStage<Void> awaitRouteReady(ZLinkBackendActorRef ref) {
        return ZLinkActorRetryScheduler.waitUntilRelay(
            RELAY_SUBMIT_TIMEOUT,
            () -> routeReady.test(ref.nodeRid()),
            () -> trace(STREAM_TRACE ? "session-actor route-ready sessionRid=" + sessionRid
                + " actorNode=" + ref.nodeRid()
                + " actorId=" + ref.actorId()
                + " generation=" + ref.generation() : null),
            () -> {
                String message =
                    "session relay route was not ready before timeout: "
                        + ref.actorId();
                //  Spec 32-framework-error-model:90 — a route wait past its
                //  deadline is DeadlineExceeded, not a raw language timeout.
                //  The TimeoutException cause is kept for diagnostics.
                return new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                    message,
                    new TimeoutException(message));
            });
    }

    private static void trace(String message) {
        if (STREAM_TRACE) {
            LOGGER.fine("[zlink-java-stream-trace] " + message);
        }
    }

    private static String errorSummary(Throwable error) {
        Throwable current = error;
        while (current instanceof CompletionException
            && current.getCause() != null) {
            current = current.getCause();
        }
        String message = current.getMessage();
        return current.getClass().getSimpleName()
            + (message == null || message.isBlank() ? "" : ":" + message);
    }

}
