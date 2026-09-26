package systems.zlink.framework.runtime.actors;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locations.ZLinkLocationOptions;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.internal.backend.*;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamActorControl;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag;
import systems.zlink.framework.streams.ZLinkSessionActor;
import systems.zlink.framework.streams.ZLinkSessionActors;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

import java.time.Duration;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.EnumSet;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.LongFunction;
import java.util.function.LongSupplier;
import java.util.function.Predicate;
import java.util.function.Supplier;
import java.util.logging.Logger;

public final class ZLinkSessionActorsRuntime implements ZLinkSessionActors {
    private static final Logger LOGGER =
            Logger.getLogger(ZLinkSessionActorsRuntime.class.getName());
    static final Duration RELAY_SUBMIT_TIMEOUT = Duration.ofSeconds(30);
    private static final ZLinkSessionRelayHeaders RELAY_HEADERS = new ZLinkSessionRelayHeaders();
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
    private final Duration sessionRelocationSealTimeout;
    // This runtime is one Session owner's C2 state boundary.  Keep the concurrent
    // binding views below for the synchronous ZLinkSessionActors query surface;
    // all cross-collection state transitions run through this lane.
    private final ZLinkStateLane stateLane = new ZLinkStateLane();
    private final List<ZLinkSessionActor> bound = new CopyOnWriteArrayList<>();
    private final ConcurrentHashMap<String, StoredBindingRoute> bindingRoutes =
            new ConcurrentHashMap<>();
    private final ConcurrentHashMap<String, CompletableFuture<Void>> bindingTransitions =
            new ConcurrentHashMap<>();
    //  Command 42 terminals keyed by relocation id. The bound and the
    //  bounded eviction mirror the C++ `_session_seal_terminals` store
    //  (public_host_runtime.cpp:4005-4020).
    private static final int SEAL_TERMINAL_CAPACITY = 65_536;
    //  How many spent terminals stay behind to answer a command 42 that was
    //  still in flight when its route reached a terminal.
    private static final int SPENT_SEAL_TERMINAL_RETENTION = 64;
    private final java.util.LinkedHashMap<SessionRelocationKey, SealTerminal> sealTerminals =
            new java.util.LinkedHashMap<>();
    private final java.util.LinkedHashMap<SessionRelocationKey, RouteTerminal> routeTerminals =
            new java.util.LinkedHashMap<>();
    private final java.util.HashMap<SessionRelocationKey, RouteFlight> routeFlights =
            new java.util.HashMap<>();
    private final java.util.HashMap<String, PushQueue> pushQueues = new java.util.HashMap<>();
    // Binding generations are unique across every Session of this Session owner
    // node lifecycle (Session-Actor binding §4). The node owns the counter.
    private final LongSupplier bindingGenerations;
    private final java.util.HashMap<String, IngressGate> ingressGates = new java.util.HashMap<>();
    private long nextFallbackIngressSequence = 1;
    private int nextActorSlot = 1;
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
            Set<String> sessionToActorKeys, Set<String> actorToSessionKeys) {
        metadataPolicy = new ZLinkRelayMetadataPolicy(sessionToActorKeys, actorToSessionKeys);
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

    public record LocalActorReply(Message payload, ZLinkStreamCodec codec) {
        public LocalActorReply {
            Objects.requireNonNull(payload, "payload");
            Objects.requireNonNull(codec, "codec");
        }
    }

    @FunctionalInterface
    interface IngressAdmission {
        CompletionStage<Void> submit(LongFunction<CompletionStage<Void>> operation);
    }

    public static void enterRelayDispatch(ZLinkStreamHeader header) {
        RELAY_HEADERS.enter(header);
    }

    public static void enterRelayDispatch(
            ZLinkSessionDispatchContext dispatch, ZLinkStreamHeader header) {
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
        this(
                null,
                stream,
                sessionRid,
                actors,
                serializer,
                routeReady,
                localActorDispatcher,
                nativeSessionRelayAttached,
                defaultCodec,
                null);
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
        this(
                spotNode,
                stream,
                sessionRid,
                actors,
                serializer,
                routeReady,
                localActorDispatcher,
                nativeSessionRelayAttached,
                defaultCodec,
                null);
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
                spotNode,
                stream,
                sessionRid,
                actors,
                serializer,
                routeReady,
                localActorDispatcher,
                nativeSessionRelayAttached,
                defaultCodec,
                flow,
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
                spotNode,
                stream,
                sessionRid,
                actors,
                serializer,
                routeReady,
                localActorDispatcher,
                nativeSessionRelayAttached,
                defaultCodec,
                flow,
                sessionRelocationSealTimeout,
                new AtomicLong()::incrementAndGet);
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
            Duration sessionRelocationSealTimeout,
            LongSupplier bindingGenerations) {
        this.bindingGenerations = Objects.requireNonNull(bindingGenerations, "bindingGenerations");
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
        this.sessionRelocationSealTimeout =
                validateSessionRelocationSealTimeout(sessionRelocationSealTimeout);
    }

    private static Duration defaultSessionRelocationSealTimeout() {
        return new ZLinkLocationOptions().sessionRelocationSealTimeout();
    }

    private static Duration validateSessionRelocationSealTimeout(Duration value) {
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
        Optional<ZLinkSessionActor> existing =
                bound.stream().filter(boundActor -> sameRef(boundActor.ref(), actor)).findFirst();
        if (existing.isPresent()) {
            return CompletableFuture.completedFuture(existing.orElseThrow());
        }
        ZLinkBackendActorRef ref =
                new ZLinkBackendActorRef(
                        actor.nodeRid(), actor.actorId(), actor.objectGeneration());
        return bindBackendRef(ref, actor.meshName());
    }

    @Override
    public CompletionStage<ZLinkSessionActor> bindOrGet(ActorRef actor) {
        ZLinkBackendActorRef ref =
                new ZLinkBackendActorRef(
                        actor.nodeRid(), actor.actorId(), actor.objectGeneration());
        Optional<ZLinkSessionActor> existing =
                bound.stream().filter(boundActor -> sameRef(boundActor.ref(), actor)).findFirst();
        return existing.<CompletionStage<ZLinkSessionActor>>map(CompletableFuture::completedFuture)
                .orElseGet(() -> bindBackendRef(ref, actor.meshName()));
    }

    @Override
    public Optional<ZLinkSessionActor> find(String actorId) {
        return bound.stream().filter(actor -> actor.actorId().equals(actorId)).findFirst();
    }

    public Optional<ZLinkSessionActor> findBySlot(int actorSlot) {
        return bound.stream()
                .map(ZLinkBoundActor.class::cast)
                .filter(actor -> actor.actorSlot() == actorSlot)
                .map(ZLinkSessionActor.class::cast)
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
        return CompletableFuture.allOf(
                        current.stream()
                                .map(actor -> notifyDisconnectedSafely(actor, timeout))
                                .toArray(CompletableFuture[]::new))
                .whenComplete(
                        (ignored, error) -> {
                            current.forEach(this::removeBinding);
                            stopRelocationOwner(
                                    new ZLinkConfigurationException(
                                            "Session disconnected while relocation state was"
                                                    + " pending"));
                        });
    }

    private void stopRelocationOwner(RuntimeException failure) {
        stopRelocationOwnerCore(failure, null);
    }

    private boolean stopRelocationOwner(RuntimeException failure, SealTerminal timeoutTerminal) {
        return stopRelocationOwnerCore(failure, timeoutTerminal);
    }

    private boolean stopRelocationOwnerCore(
            RuntimeException failure, SealTerminal timeoutTerminal) {
        StopRelocationState stopped =
                inStateLane(
                        () -> {
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
                            ingressGates.values().forEach(gate -> held.addAll(gate.detachHeld()));
                            ingressGates.clear();
                            bindingRoutes.clear();
                            bindingTransitions.clear();
                            List<SealTerminal> seals = List.copyOf(sealTerminals.values());
                            pushQueues
                                    .values()
                                    .forEach(
                                            queue -> queue.stop(TargetOutboundSettlement.SHUTDOWN));
                            pushQueues.clear();
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
        stopped.routes().forEach(route -> route.completion().completeExceptionally(failure));
        return true;
    }

    private record StopRelocationState(
            List<HeldIngress> held, List<SealTerminal> seals, List<RouteFlight> routes) {}

    private static CompletableFuture<Void> notifyDisconnectedSafely(
            ZLinkSessionActor actor, Duration timeout) {
        try {
            return actor instanceof ZLinkBoundActor boundActor
                    ? boundActor.notifyDisconnected(timeout).toCompletableFuture()
                    : actor.notifyDisconnected().toCompletableFuture();
        } catch (RuntimeException error) {
            return CompletableFuture.failedFuture(error);
        }
    }

    private CompletionStage<ZLinkSessionActor> bindBackendRef(
            ZLinkBackendActorRef ref, String meshName) {
        if (actors != null) {
            Optional<ZLinkActor> localActor = actors.localActor(ref.actorId());
            if (localActor.isPresent()) {
                ZLinkBackendActorRef localRef = actors.refFor(localActor.get());
                if (localRef.nodeRid().equals(ref.nodeRid())
                        && localRef.actorId().equals(ref.actorId())
                        && localRef.generation() == ref.generation()) {
                    return bindManagedAsyncCore(localActor.get());
                }
            }
        }
        CompletionStage<Void> authorityReady =
                actors == null
                        ? CompletableFuture.completedFuture(null)
                        : actors.prepareRemoteSessionBinding(ref);
        return authorityReady
                .thenCompose(
                        ignored ->
                                replaceBinding(
                                        ref.actorId(),
                                        () ->
                                                awaitRouteReady(ref)
                                                        .thenCompose(
                                                                routeReadyIgnored -> {
                                                                    int actorSlot =
                                                                            allocateActorSlot();
                                                                    return ZLinkBoundSessionRuntime
                                                                            .bindActorWithRetry(
                                                                                    stream,
                                                                                    sessionRid,
                                                                                    ref,
                                                                                    actorSlot,
                                                                                    RELAY_SUBMIT_TIMEOUT)
                                                                            .thenApply(
                                                                                    bindIgnored ->
                                                                                            actorSlot);
                                                                })
                                                        .thenApply(
                                                                actorSlot -> {
                                                                    AtomicReference<ZLinkBoundActor>
                                                                            binding =
                                                                                    new AtomicReference<>();
                                                                    long bindingGeneration =
                                                                            currentBindingGeneration(
                                                                                    ref.actorId());
                                                                    ZLinkBoundActor actor =
                                                                            new ZLinkBoundActor(
                                                                                    stream,
                                                                                    sessionRid,
                                                                                    ref,
                                                                                    meshName,
                                                                                    Optional
                                                                                            .empty(),
                                                                                    actors,
                                                                                    serializer,
                                                                                    0,
                                                                                    bindingGeneration,
                                                                                    actorSlot,
                                                                                    routeReady,
                                                                                    null,
                                                                                    true,
                                                                                    defaultCodec,
                                                                                    RELAY_HEADERS,
                                                                                    flow,
                                                                                    () ->
                                                                                            isCurrentBinding(
                                                                                                    binding
                                                                                                            .get()),
                                                                                    operation ->
                                                                                            admitIngress(
                                                                                                    binding
                                                                                                            .get(),
                                                                                                    operation),
                                                                                    metadataPolicy);
                                                                    binding.set(actor);
                                                                    actor.setUnbindListener(
                                                                            () ->
                                                                                    removeBinding(
                                                                                            actor));
                                                                    return actor;
                                                                })))
                .thenCompose(
                        actor ->
                                actor.notifyRemoteBoundSession()
                                        .thenApply(notificationIgnored -> actor))
                .whenComplete(
                        (actor, error) -> {
                            if (error != null && actor != null) {
                                removeBinding(actor);
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
            return CompletableFuture.failedFuture(
                    new ZLinkConfigurationException(
                            "managed actor binding requires an actor runtime"));
        }
        ZLinkBackendActorRef ref = actors.refFor(actor);
        Optional<ZLinkSessionActor> existing =
                bound.stream()
                        .filter(
                                boundActor ->
                                        boundActor.actorId().equals(ref.actorId())
                                                && boundActor.ref().nodeRid().equals(ref.nodeRid())
                                                && boundActor.ref().objectGeneration()
                                                        == ref.generation())
                        .findFirst();
        if (existing.isPresent()) {
            return CompletableFuture.completedFuture(existing.orElseThrow());
        }
        return replaceBinding(
                        actor.context().actorId(),
                        () -> {
                            int actorSlot = allocateActorSlot();
                            CompletionStage<Void> nativeBinding =
                                    nativeSessionRelayAttached
                                            ? awaitRouteReady(ref)
                                                    .thenCompose(
                                                            ignored ->
                                                                    ZLinkBoundSessionRuntime
                                                                            .bindActorWithRetry(
                                                                                    stream,
                                                                                    sessionRid,
                                                                                    ref,
                                                                                    actorSlot,
                                                                                    RELAY_SUBMIT_TIMEOUT))
                                            : CompletableFuture.completedFuture(null);
                            return nativeBinding.thenApply(
                                    ignored -> {
                                        long bindingGeneration =
                                                currentBindingGeneration(ref.actorId());
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
                                                nativeSessionRelayAttached && spotNode != null
                                                        ? spotNode.routingId()
                                                        : null;
                                        RoutingId sourceSessionRid =
                                                nativeSessionRelayAttached ? sessionRid : null;
                                        long bindingToken =
                                                actors.bindSession(
                                                        actor,
                                                        boundSession,
                                                        sourceNodeRid,
                                                        sourceSessionRid,
                                                        bindingGeneration,
                                                        0);
                                        boundSession.setBindingToken(bindingToken);
                                        boundSession.setActorSlot(actorSlot);
                                        AtomicReference<ZLinkBoundActor> binding =
                                                new AtomicReference<>();
                                        ZLinkBoundActor boundActor =
                                                new ZLinkBoundActor(
                                                        stream,
                                                        sessionRid,
                                                        ref,
                                                        actors.meshName(),
                                                        Optional.of(actor),
                                                        actors,
                                                        serializer,
                                                        bindingToken,
                                                        bindingGeneration,
                                                        actorSlot,
                                                        routeReady,
                                                        localActorDispatcher,
                                                        nativeSessionRelayAttached,
                                                        defaultCodec,
                                                        RELAY_HEADERS,
                                                        flow,
                                                        () -> isCurrentBinding(binding.get()),
                                                        operation ->
                                                                admitIngress(
                                                                        binding.get(), operation),
                                                        metadataPolicy);
                                        binding.set(boundActor);
                                        boundSession.setUnbindListener(
                                                () -> removeBinding(boundActor));
                                        boundActor.setUnbindListener(
                                                () -> removeBinding(boundActor));
                                        boundSession.setRebindListener(
                                                target -> recordNativeRebind(boundActor, target));
                                        return boundActor;
                                    });
                        })
                .thenApply(value -> (ZLinkSessionActor) value);
    }

    void recordNativeRebind(ZLinkBoundActor actor, ZLinkBackendActorRef targetActor) {
        actor.rebindNativeActor(targetActor);
        inStateLane(
                () ->
                        bindingRoutes.computeIfPresent(
                                actor.actorId(),
                                (ignored, current) -> current.toNativeTarget(targetActor)));
    }

    private CompletionStage<ZLinkBoundActor> replaceBinding(
            String actorId, Supplier<CompletionStage<ZLinkBoundActor>> createBinding) {
        CompletableFuture<Void> tail = new CompletableFuture<>();
        CompletableFuture<Void> previousTransition =
                inStateLane(
                        () -> {
                            CompletableFuture<Void> previous = bindingTransitions.get(actorId);
                            bindingTransitions.put(actorId, tail);
                            return previous;
                        });
        CompletionStage<Void> ready =
                previousTransition == null
                        ? CompletableFuture.completedFuture(null)
                        : previousTransition.handle((ignored, failure) -> null);
        CompletionStage<ZLinkBoundActor> installation =
                ready.thenCompose(ignored -> installReplacement(actorId, createBinding));
        installation.whenComplete(
                (ignored, failure) -> {
                    // Complete off the lane so an inline dependent cannot re-enter it.
                    tail.completeAsync(() -> null)
                            .whenComplete(
                                    (completed, ignoredError) ->
                                            inStateLane(
                                                    () -> {
                                                        bindingTransitions.remove(actorId, tail);
                                                        return null;
                                                    }));
                });
        return installation;
    }

    private CompletionStage<ZLinkBoundActor> installReplacement(
            String actorId, Supplier<CompletionStage<ZLinkBoundActor>> createBinding) {
        List<ZLinkBoundActor> previous =
                bound.stream()
                        .filter(existing -> existing.actorId().equals(actorId))
                        .map(ZLinkBoundActor.class::cast)
                        .toList();
        CompletionStage<ZLinkBoundActor> created;
        try {
            created = createBinding.get();
        } catch (RuntimeException failure) {
            throw failure;
        }
        return created.thenCompose(
                actor -> {
                    previous.forEach(this::removeBinding);
                    BindingPublication publication =
                            inStateLane(
                                    () -> {
                                        BindingCleanup cleanup = installBindingOnLane(actor);
                                        CompletionStage<Void> physical = announceBound(actor);
                                        stream.publishBoundActor(sessionRid, actor.actorId());
                                        return new BindingPublication(cleanup, physical);
                                    });
                    finishBindingInstall(actor, publication.cleanup());
                    return publication.physical().thenApply(ignored -> actor);
                });
    }

    private int allocateActorSlot() {
        return inStateLane(
                () -> {
                    if (nextActorSlot > 0xffff) {
                        throw new ZLinkFrameworkException(
                                ZLinkFrameworkErrorKind.INVALID_OPERATION,
                                "Session Actor slots are exhausted");
                    }
                    return nextActorSlot++;
                });
    }

    private CompletionStage<Void> announceBound(ZLinkBoundActor actor) {
        return admitActorControl(
                ZLinkStreamActorControl.BOUND,
                ZLinkStreamActorControl.bound(actor.actorSlot(), actor.actorId()));
    }

    private void announceUnbound(ZLinkBoundActor actor) {
        sendActorControl(
                ZLinkStreamActorControl.UNBOUND,
                ZLinkStreamActorControl.unbound(actor.actorSlot()));
    }

    private CompletionStage<Void> sendActorControl(String name, byte[] payload) {
        return submitActorControl(name, payload, false);
    }

    private CompletionStage<Void> admitActorControl(String name, byte[] payload) {
        return submitActorControl(name, payload, true);
    }

    private CompletionStage<Void> submitActorControl(
            String name, byte[] payload, boolean synchronousAdmission) {
        ZLinkStreamHeader header =
                new ZLinkStreamHeader(
                        ZLinkStreamMessageKind.CONTROL,
                        ZLinkStreamCodec.RAW,
                        EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
                        Optional.empty(),
                        name,
                        Map.of());
        Message body = Message.from(payload);
        try {
            CompletionStage<Void> submission =
                    synchronousAdmission
                            ? stream.admitSessionControl(sessionRid, header, List.of(body))
                            : stream.sendAsync(sessionRid, header, List.of(body));
            return submission.whenComplete((ignored, failure) -> body.close());
        } catch (RuntimeException failure) {
            body.close();
            return CompletableFuture.failedFuture(failure);
        }
    }

    private long currentBindingGeneration(String actorId) {
        long generation =
                nativeSessionRelayAttached
                        ? stream.boundActorBindingGeneration(sessionRid, actorId)
                        : 0;
        if (generation > 0) {
            return generation;
        }
        generation = bindingGenerations.getAsLong();
        if (generation <= 0) {
            throw new ZLinkConfigurationException("Session binding generation is exhausted");
        }
        return generation;
    }

    private ZLinkBoundActor installBinding(ZLinkBoundActor actor) {
        BindingCleanup cleanup = inStateLane(() -> installBindingOnLane(actor));
        finishBindingInstall(actor, cleanup);
        return actor;
    }

    private BindingCleanup installBindingOnLane(ZLinkBoundActor actor) {
        ActorRef current = actor.ref();
        long bindingGeneration = actor.bindingGeneration();
        List<HeldIngress> abandoned = List.of();
        SealTerminal abandonedSeal = null;
        if (relocationStopped) {
            throw new ZLinkConfigurationException(
                    "Session is closed while installing an Actor binding");
        }
        bound.add(actor);
        PushQueue previousPushes = pushQueues.remove(actor.actorId());
        if (previousPushes != null) {
            previousPushes.stop(TargetOutboundSettlement.SHUTDOWN);
        }
        bindingRoutes.put(
                actor.actorId(),
                new StoredBindingRoute(
                        current.actorId(),
                        current.objectGeneration(),
                        current.meshName(),
                        current.nodeRid(),
                        bindingGeneration,
                        0));
        IngressGate previous =
                ingressGates.put(
                        actor.actorId(),
                        new IngressGate(current.objectGeneration(), bindingGeneration));
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
    }

    private void finishBindingInstall(ZLinkBoundActor actor, BindingCleanup cleanup) {
        failHeld(
                cleanup.held(),
                new ZLinkConfigurationException(
                        "Session binding changed while relocation ingress was held: "
                                + actor.actorId()));
        if (cleanup.seal() != null) {
            cleanup.seal()
                    .fail(
                            new ZLinkConfigurationException(
                                    "Session binding changed before relocation seal drained: "
                                            + actor.actorId()));
        }
    }

    private void removeBinding(ZLinkSessionActor actor) {
        BindingCleanup cleanup =
                inStateLane(
                        () -> {
                            if (!bound.remove(actor)) {
                                return null;
                            }
                            List<HeldIngress> abandoned = List.of();
                            SealTerminal abandonedSeal = null;
                            boolean replacementExists =
                                    bound.stream()
                                            .anyMatch(
                                                    candidate ->
                                                            candidate
                                                                    .actorId()
                                                                    .equals(actor.actorId()));
                            if (!replacementExists) {
                                bindingRoutes.remove(actor.actorId());
                                PushQueue removedPushes = pushQueues.remove(actor.actorId());
                                if (removedPushes != null) {
                                    removedPushes.stop(TargetOutboundSettlement.SHUTDOWN);
                                }
                                IngressGate removed = ingressGates.remove(actor.actorId());
                                if (removed != null) {
                                    abandoned = removed.detachHeld();
                                    if (removed.seal != null) {
                                        SealTerminal terminal =
                                                sealTerminals.get(
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
            announceUnbound((ZLinkBoundActor) actor);
            failHeld(
                    cleanup.held(),
                    new ZLinkConfigurationException(
                            "bound Actor was removed while relocation ingress was held: "
                                    + actor.actorId()));
            if (cleanup.seal() != null) {
                cleanup.seal()
                        .fail(
                                new ZLinkConfigurationException(
                                        "bound Actor was removed before relocation seal drained: "
                                                + actor.actorId()));
            }
        }
    }

    private record BindingCleanup(List<HeldIngress> held, SealTerminal seal) {}

    private record BindingPublication(BindingCleanup cleanup, CompletionStage<Void> physical) {}

    /**
     * Accepts one Session-to-Actor ingress record or holds it behind the relocation seal. This
     * method and command 42 use {@link #sealTerminals} as their shared linearization lock, so the
     * ACK high-water cannot race a post-seal relay into the captured prefix.
     */
    private CompletionStage<Void> admitIngress(
            ZLinkBoundActor actor, LongFunction<CompletionStage<Void>> operation) {
        Objects.requireNonNull(operation, "operation");
        IngressAdmissionState admission =
                inStateLane(
                        () -> {
                            CompletableFuture<Void> held = null;
                            IngressGate admittedGate = null;
                            long acceptedSequence = 0;
                            if (!isCurrentBinding(actor)) {
                                return IngressAdmissionState.failed(
                                        new ZLinkConfigurationException(
                                                "bound Actor is no longer the current Session"
                                                        + " binding: "
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
                                bindingRoutes.put(
                                        actor.actorId(), route.withAcceptedHighWater(accepted));
                            }
                            return new IngressAdmissionState(
                                    held, admittedGate, acceptedSequence, null);
                        });
        if (admission.failure() != null) {
            return CompletableFuture.failedFuture(admission.failure());
        }
        if (admission.held() != null) {
            return admission.held();
        }
        IngressGate completionGate = admission.gate();
        CompletionStage<Void> submission = invokeIngress(operation, admission.sequence());
        submission.whenComplete((ignored, failure) -> completeIngress(actor, completionGate));
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
            if (nextFallbackIngressSequence <= 0 || nextFallbackIngressSequence == Long.MAX_VALUE) {
                throw new ZLinkConfigurationException("Session ingress sequence is exhausted");
            }
            sequence = nextFallbackIngressSequence++;
        }
        return gate.accept(sequence);
    }

    private void completeIngress(ZLinkBoundActor actor, IngressGate admittedGate) {
        SealTerminal drained =
                inStateLane(
                        () -> {
                            SealTerminal candidateDrained = null;
                            if (ingressGates.get(actor.actorId()) == admittedGate
                                    && admittedGate.activeIngress > 0) {
                                admittedGate.activeIngress--;
                                if (admittedGate.activeIngress == 0 && admittedGate.seal != null) {
                                    SealTerminal terminal =
                                            sealTerminals.get(
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
            LongFunction<CompletionStage<Void>> operation, long sourceSessionSequence) {
        try {
            return Objects.requireNonNull(
                    operation.apply(sourceSessionSequence), "Session ingress operation stage");
        } catch (RuntimeException failure) {
            return CompletableFuture.failedFuture(failure);
        }
    }

    private void resumeHeld(List<HeldIngress> held) {
        CompletionStage<Void> tail = CompletableFuture.completedFuture(null);
        for (HeldIngress pending : held) {
            tail =
                    tail.handle((ignored, failure) -> null)
                            .thenComposeAsync(
                                    ignored -> admitIngress(pending.actor(), pending.operation()))
                            .whenComplete(
                                    (ignored, failure) -> {
                                        if (failure == null) {
                                            pending.result().complete(null);
                                        } else {
                                            pending.result().completeExceptionally(failure);
                                        }
                                    });
        }
    }

    private static void failHeld(List<HeldIngress> held, RuntimeException failure) {
        held.forEach(pending -> pending.result().completeExceptionally(failure));
    }

    private ZLinkBoundActor currentBoundActor(String actorId) {
        return bound.stream()
                .filter(candidate -> candidate.actorId().equals(actorId))
                .map(ZLinkBoundActor.class::cast)
                .findFirst()
                .orElseThrow(
                        () ->
                                new ZLinkConfigurationException(
                                        "bound Actor is unavailable: " + actorId));
    }

    /**
     * Applies relocation command 42 at this Session owner and answers with command 43. Ported from
     * the C++ session-owner handler
     * (`framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp:3974-4078`)
     * in the same order: cached-terminal lookup keyed by relocation id (identical retransmit
     * re-sends the cached ACK, a conflicting seal is refused), cache bound, the binding fence check
     * that C++ performs in `stream_session_registry_t::seal_remote_route`
     * (`stream_session_registry.cpp:330`), then the ACK that echoes every command 42 field plus the
     * owner's accepted high-water.
     *
     * <p>A refusal completes exceptionally so the seal stays unanswered and the source retransmits
     * on the spec 20 §5 step 8 schedule, mirroring the C++ handler's `continue`.
     */
    public CompletionStage<ZLinkServiceM6BWireCodec.SessionRelocationSealed>
            applyRelocationSealCommand(ZLinkServiceM6BWireCodec.SessionRelocationSeal command) {
        return onStateLane(() -> applyRelocationSealCommandCore(command))
                .thenCompose(stage -> stage);
    }

    private CompletionStage<ZLinkServiceM6BWireCodec.SessionRelocationSealed>
            applyRelocationSealCommandCore(ZLinkServiceM6BWireCodec.SessionRelocationSeal command) {
        Objects.requireNonNull(command, "command");
        if (!sessionRid.equals(command.session().sessionRid())) {
            return CompletableFuture.failedFuture(
                    new ZLinkConfigurationException(
                            "Session relocation seal command does not target this Session"));
        }
        if (relocationStopped) {
            return CompletableFuture.failedFuture(
                    new ZLinkConfigurationException("Session relocation owner is stopped"));
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
        if (sealTerminals.size() >= SEAL_TERMINAL_CAPACITY && !evictOneSealTerminal()) {
            return CompletableFuture.failedFuture(
                    new ZLinkConfigurationException(
                            "Session relocation seal terminal store is full"));
        }
        StoredBindingRoute observed = bindingRoutes.get(command.actor().actor().actorId());
        IngressGate gate = ingressGates.get(command.actor().actor().actorId());
        if (!sealFenceMatches(command, observed)
                || gate == null
                || gate.objectGeneration != command.actor().actor().generation()
                || gate.bindingGeneration != command.session().bindingGeneration()) {
            return CompletableFuture.failedFuture(
                    new ZLinkConfigurationException(
                            "Session relocation seal fence differs from the current "
                                    + "binding: "
                                    + command.actor().actor().actorId()));
        }
        if (gate.seal != null) {
            return CompletableFuture.failedFuture(
                    new ZLinkConfigurationException(
                            ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                            "Session binding is already sealed by another relocation: "
                                    + command.actor().actor().actorId()));
        }
        sealed =
                new ZLinkServiceM6BWireCodec.SessionRelocationSealed(
                        command.relocation(),
                        command.coordinator(),
                        command.actor(),
                        command.session());
        gate.seal = command.relocation();
        installed = new SealTerminal(command, sealed);
        sealTerminals.put(key, installed);
        installed.armDeadline(() -> expireRelocationSeal(installed), sessionRelocationSealTimeout);
        pruneSpentSealTerminals();
        if (ingressDrainedCore(command)) {
            installed.completeSealed();
        }
        return installed.completion();
    }

    private void expireRelocationSeal(SealTerminal terminal) {
        //  Spec 32-framework-error-model:38 — a live operation past its deadline
        //  is DeadlineExceeded, not a configuration error.
        RuntimeException failure =
                new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                        "Session relocation route update timed out: "
                                + terminal.seal().actor().actor().actorId());
        if (!stopRelocationOwner(failure, terminal)) {
            return;
        }

        List<ZLinkSessionActor> current =
                inStateLane(
                        () -> {
                            List<ZLinkSessionActor> snapshot = List.copyOf(bound);
                            bound.clear();
                            return snapshot;
                        });
        try {
            stream.disconnectPeer(sessionRid);
        } catch (RuntimeException ignored) {
        }
        current.forEach(actor -> notifyDisconnectedSafely(actor, RELAY_SUBMIT_TIMEOUT));
    }

    private boolean ingressDrained(ZLinkServiceM6BWireCodec.SessionRelocationSeal command) {
        return inStateLane(() -> ingressDrainedCore(command));
    }

    private boolean ingressDrainedCore(ZLinkServiceM6BWireCodec.SessionRelocationSeal command) {
        IngressGate gate = ingressGates.get(command.actor().actor().actorId());
        return gate != null && command.relocation().equals(gate.seal) && gate.activeIngress == 0;
    }

    /**
     * Returns whether command 36 names this Session's current binding. Session-Actor binding §3
     * item 3 and §8.1: ActorId, ObjectGeneration and binding generation are the only admission
     * inputs; the fence's node, authority and lease fields are not compared here.
     */
    public boolean matchesBoundSessionSend(ZLinkServiceM6BWireCodec.BoundSessionSend command) {
        Objects.requireNonNull(command, "command");
        return inStateLane(() -> isCurrentPushBindingLocked(command));
    }

    /** Admits command 36 without blocking its infrastructure receive owner. */
    public CompletionStage<Boolean> acceptBoundSessionSendAsync(
            ZLinkServiceM6BWireCodec.BoundSessionSend command,
            ZLinkServiceM6AWireCodec.ApplicationPayload payload) {
        Objects.requireNonNull(command, "command");
        Objects.requireNonNull(payload, "payload");
        return stateLane
                .<PushDecision>runAsync(() -> decidePushLocked(command, payload))
                .thenCompose(
                        decision -> {
                            if (decision.submitted() != null) {
                                return decision.submitted();
                            }
                            startPushDrain(decision.queue());
                            return CompletableFuture.completedFuture(
                                    decision.admission().admitted());
                        });
    }

    /** Admits command 36 and exposes the settlement of its physical STREAM submission. */
    TargetOutboundAdmission admitBoundSessionSend(
            ZLinkServiceM6BWireCodec.BoundSessionSend command,
            ZLinkServiceM6AWireCodec.ApplicationPayload payload) {
        Objects.requireNonNull(command, "command");
        Objects.requireNonNull(payload, "payload");
        PushDecision decision = inStateLane(() -> decidePushLocked(command, payload));
        startPushDrain(decision.queue());
        return decision.admission();
    }

    /**
     * The Session owner's single command-36 admission decision (Session-Actor binding §3 item 3,
     * §8.1). A push for the current binding is submitted to the STREAM connection on this lane
     * unless the binding is sealed for relocation or earlier held pushes still wait; then it joins
     * the binding's FIFO, which drains once command 44 commit or abort releases the seal.
     */
    private PushDecision decidePushLocked(
            ZLinkServiceM6BWireCodec.BoundSessionSend command,
            ZLinkServiceM6AWireCodec.ApplicationPayload payload) {
        if (!isCurrentPushBindingLocked(command)) {
            return new PushDecision(
                    null,
                    TargetOutboundAdmission.rejected(TargetOutboundSettlement.REJECTED),
                    null);
        }
        String actorId = command.actor().actor().actorId();
        PushQueue queue = pushQueues.get(actorId);
        if (!pushHeldLocked(actorId) && (queue == null || queue.entries.isEmpty())) {
            CompletionStage<Boolean> submitted = deliverBoundSessionPushAsync(actorId, payload);
            return new PushDecision(
                    null,
                    new TargetOutboundAdmission(
                            true,
                            submitted.handle(
                                    (ignored, failure) ->
                                            failure == null
                                                    ? TargetOutboundSettlement.DELIVERED
                                                    : TargetOutboundSettlement.REJECTED)),
                    submitted);
        }
        if (queue == null) {
            queue = new PushQueue(actorId);
            pushQueues.put(actorId, queue);
        }
        return new PushDecision(queue, queue.admit(payload), null);
    }

    private boolean isCurrentPushBindingLocked(ZLinkServiceM6BWireCodec.BoundSessionSend command) {
        if (relocationStopped) {
            return false;
        }
        var actor = command.actor().actor();
        StoredBindingRoute route = bindingRoutes.get(actor.actorId());
        return route != null
                && route.objectGeneration() == actor.generation()
                && route.bindingGeneration() == command.expectedBindingGeneration();
    }

    private boolean pushHeldLocked(String actorId) {
        IngressGate gate = ingressGates.get(actorId);
        return gate != null && gate.seal != null;
    }

    //  submitted is the direct STREAM submission; queue is set when the push joined the FIFO.
    private record PushDecision(
            PushQueue queue,
            TargetOutboundAdmission admission,
            CompletionStage<Boolean> submitted) {}

    private CompletionStage<Boolean> deliverBoundSessionPushAsync(
            String actorId, ZLinkServiceM6AWireCodec.ApplicationPayload payload) {
        return submitBoundSessionPush(actorId, payload).thenApply(ignored -> true);
    }

    //  Returns the backend physical-admission stage itself, so a stopped FIFO can cancel it.
    private CompletionStage<Void> submitBoundSessionPush(
            String actorId, ZLinkServiceM6AWireCodec.ApplicationPayload payload) {
        List<Message> parts = List.of();
        try {
            parts = ZLinkServiceM6AWireCodec.decodeFrameworkMultipart(payload);
            return stream.sendBoundSessionPushAsync(sessionRid, actorSlotFor(actorId), parts);
        } catch (RuntimeException failure) {
            return CompletableFuture.failedFuture(failure);
        } finally {
            parts.forEach(Message::close);
        }
    }

    private int actorSlotFor(String actorId) {
        return bound.stream()
                .map(ZLinkBoundActor.class::cast)
                .filter(actor -> actor.actorId().equals(actorId))
                .findFirst()
                .orElseThrow(
                        () ->
                                new ZLinkConfigurationException(
                                        "bound Actor is unavailable: " + actorId))
                .actorSlot();
    }

    private void startPushDrain(PushQueue owner) {
        if (owner == null) {
            return;
        }
        PushEntry pending =
                inStateLane(
                        () -> {
                            if (owner.drainRunning || !pushDrainable(owner)) {
                                return null;
                            }
                            owner.drainRunning = true;
                            return owner.entries.peekFirst();
                        });
        if (pending == null) {
            return;
        }
        CompletableFuture<Void> physical;
        try {
            physical =
                    Objects.requireNonNull(
                            submitBoundSessionPush(owner.actorId, pending.payload)
                                    .toCompletableFuture(),
                            "physical STREAM admission future");
        } catch (RuntimeException failure) {
            physical = CompletableFuture.failedFuture(failure);
        }
        CompletableFuture<Void> submitted = physical;
        boolean cancel =
                inStateLane(
                        () -> {
                            boolean cancelled =
                                    owner.stopped || owner.entries.peekFirst() != pending;
                            if (cancelled) {
                                owner.drainRunning = false;
                            } else {
                                owner.physicalDrain = submitted;
                            }
                            return cancelled;
                        });
        if (cancel) {
            submitted.cancel(false);
            return;
        }
        submitted.whenComplete(
                (ignored, failure) -> {
                    DrainCompletion completion =
                            inStateLane(
                                    () -> {
                                        if (owner.physicalDrain != submitted) {
                                            return null;
                                        }
                                        owner.physicalDrain = null;
                                        owner.drainRunning = false;
                                        TargetOutboundSettlement settlement = null;
                                        if (owner.entries.peekFirst() == pending) {
                                            owner.entries.removeFirst();
                                            settlement =
                                                    failure == null
                                                            ? TargetOutboundSettlement.DELIVERED
                                                            : TargetOutboundSettlement.REJECTED;
                                        }
                                        return new DrainCompletion(
                                                settlement, pushDrainable(owner));
                                    });
                    if (completion == null) {
                        return;
                    }
                    if (completion.settlement() != null) {
                        pending.settle(completion.settlement());
                    }
                    if (completion.continueDrain()) {
                        CompletableFuture.runAsync(() -> startPushDrain(owner));
                    }
                });
    }

    private boolean pushDrainable(PushQueue owner) {
        return !owner.stopped && !owner.entries.isEmpty() && !pushHeldLocked(owner.actorId);
    }

    private record DrainCompletion(TargetOutboundSettlement settlement, boolean continueDrain) {}

    private boolean sealFenceMatches(
            ZLinkServiceM6BWireCodec.SessionRelocationSeal command, StoredBindingRoute observed) {
        if (observed == null) {
            return false;
        }
        var actor = command.actor();
        boolean storedRouteMatches =
                observed.bindingGeneration() == command.session().bindingGeneration()
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
        private final CompletableFuture<ZLinkServiceM6BWireCodec.SessionRelocationSealed>
                completion = new CompletableFuture<>();
        private boolean consumed;
        private CompletableFuture<Void> deadline;

        private SealTerminal(
                ZLinkServiceM6BWireCodec.SessionRelocationSeal seal,
                ZLinkServiceM6BWireCodec.SessionRelocationSealed sealed) {
            this.seal = seal;
            this.sealed = sealed;
        }

        private ZLinkServiceM6BWireCodec.SessionRelocationSeal seal() {
            return seal;
        }

        private ZLinkServiceM6BWireCodec.SessionRelocationSealed sealed() {
            return sealed;
        }

        private CompletableFuture<ZLinkServiceM6BWireCodec.SessionRelocationSealed> completion() {
            return completion;
        }

        private boolean consumed() {
            return consumed;
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
                deadline = ZLinkActorRetryScheduler.scheduleAfter(expiration, timeout);
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
            completion.completeExceptionally(failure);
        }
    }

    enum TargetOutboundSettlement {
        DELIVERED,
        REJECTED,
        SHUTDOWN
    }

    record TargetOutboundAdmission(
            boolean admitted, CompletionStage<TargetOutboundSettlement> settlement) {
        private static TargetOutboundAdmission rejected(TargetOutboundSettlement result) {
            return new TargetOutboundAdmission(false, CompletableFuture.completedFuture(result));
        }
    }

    private static final class PushEntry {
        private final ZLinkServiceM6AWireCodec.ApplicationPayload payload;
        private final CompletableFuture<TargetOutboundSettlement> settlement =
                new CompletableFuture<>();
        private final AtomicBoolean settled = new AtomicBoolean();

        private PushEntry(ZLinkServiceM6AWireCodec.ApplicationPayload payload) {
            this.payload = Objects.requireNonNull(payload, "payload");
        }

        private void settle(TargetOutboundSettlement result) {
            if (settled.compareAndSet(false, true)) {
                settlement.completeAsync(() -> result);
            }
        }
    }

    /**
     * The FIFO of one binding's pushes held behind its relocation seal, or queued behind held
     * pushes that are still draining, so a later push never overtakes an earlier one.
     */
    private static final class PushQueue {
        private final String actorId;
        private final ArrayDeque<PushEntry> entries = new ArrayDeque<>();
        private CompletableFuture<Void> physicalDrain;
        private boolean drainRunning;
        private boolean stopped;

        private PushQueue(String actorId) {
            this.actorId = actorId;
        }

        private TargetOutboundAdmission admit(ZLinkServiceM6AWireCodec.ApplicationPayload payload) {
            if (stopped) {
                return TargetOutboundAdmission.rejected(TargetOutboundSettlement.SHUTDOWN);
            }
            PushEntry entry = new PushEntry(payload);
            entries.addLast(entry);
            return new TargetOutboundAdmission(true, entry.settlement);
        }

        private void stop(TargetOutboundSettlement result) {
            if (stopped) {
                return;
            }
            stopped = true;
            CompletableFuture<Void> active = physicalDrain;
            physicalDrain = null;
            drainRunning = false;
            for (PushEntry entry : entries) {
                entry.settle(result);
            }
            entries.clear();
            if (active != null) {
                active.cancel(false);
            }
        }
    }

    private record SessionRelocationKey(
            ZLinkServiceM6BWireCodec.RelocationIdentity relocation,
            String actorId,
            long objectGeneration,
            RoutingId sessionRid,
            long bindingGeneration) {}

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

    private record RouteTerminal(ZLinkServiceM6BWireCodec.SessionRelocationRoute command) {}

    private record RouteFlight(
            ZLinkServiceM6BWireCodec.SessionRelocationRoute command,
            StoredBindingRoute sourceRoute,
            RelocationRouteUpdate update,
            ZLinkBoundActor actor,
            ZLinkBackendActorRef sourceActor,
            ZLinkBackendActorRef targetActor,
            CompletableFuture<Void> completion) {}

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
            return CompletableFuture.failedFuture(
                    new ZLinkConfigurationException(
                            "Session relocation route command does not target this Session"));
        }
        if (relocationStopped) {
            if (relocationSealTimedOut) {
                LOGGER.warning(
                        "late_session_route_update session=" + command.session().sessionRid());
                return CompletableFuture.completedFuture(null);
            }
            return CompletableFuture.failedFuture(
                    new ZLinkConfigurationException("Session relocation owner is stopped"));
        }
        CompletionStage<Void> cached = cachedRouteTerminalCore(command);
        if (cached != null) {
            return cached;
        }
        if (command.action() != ZLinkServiceM6BWireCodec.SessionRelocationRouteAction.COMMIT) {
            return applyRelocationAbortCore(command);
        }
        final RouteFlight flight;
        if (relocationStopped) {
            if (relocationSealTimedOut) {
                LOGGER.warning(
                        "late_session_route_update session=" + command.session().sessionRid());
                return CompletableFuture.completedFuture(null);
            }
            return CompletableFuture.failedFuture(
                    new ZLinkConfigurationException("Session relocation owner is stopped"));
        }
        SessionRelocationKey key = relocationKey(command);
        RouteTerminal terminal = routeTerminals.get(key);
        if (terminal != null) {
            if (!terminal.command().equals(command)) {
                return protocolRouteConflict("command 44 conflicts with its recorded terminal");
            }
            return CompletableFuture.completedFuture(null);
        }
        RouteFlight active = routeFlights.get(key);
        if (active != null) {
            if (!active.command().equals(command)) {
                return protocolRouteConflict("command 44 conflicts with an in-flight route update");
            }
            return active.completion();
        }
        StoredBindingRoute observed = bindingRoutes.get(command.actor().actorId());
        SealTerminal seal = sealTerminals.get(key);
        if (observed == null
                || seal == null
                || seal.consumed()
                || !seal.completion().isDone()
                || !sealMatchesRoute(seal.seal(), command)
                || observed.bindingGeneration() != command.session().bindingGeneration()) {
            return CompletableFuture.completedFuture(null);
        }
        RelocationRouteUpdate update;
        try {
            update =
                    new RelocationRouteUpdate(
                            command.actor().actorId(),
                            command.actor().generation(),
                            observed.nodeRid(),
                            command.targetNodeRid(),
                            command.targetNodeGeneration(),
                            command.previousAuthorityOwnerGeneration(),
                            command.currentAuthorityOwnerGeneration(),
                            command.session().bindingGeneration());
        } catch (RuntimeException invalid) {
            return CompletableFuture.completedFuture(null);
        }
        if (!observed.matchesSource(update)) {
            return CompletableFuture.completedFuture(null);
        }
        ZLinkBackendActorRef target =
                new ZLinkBackendActorRef(
                        update.targetNodeRid(), update.actorId(), update.objectGeneration());
        ZLinkBoundActor actor = currentBoundActor(update.actorId());
        flight =
                new RouteFlight(
                        command,
                        observed,
                        update,
                        actor,
                        new ZLinkBackendActorRef(
                                observed.nodeRid(),
                                observed.actorId(),
                                observed.objectGeneration()),
                        target,
                        new CompletableFuture<>());
        routeFlights.put(key, flight);
        CompletableFuture.runAsync(() -> startRoutePreparation(flight));
        return flight.completion();
    }

    private CompletionStage<Void> applyRelocationAbort(
            ZLinkServiceM6BWireCodec.SessionRelocationRoute command) {
        return onStateLane(() -> applyRelocationAbortCore(command)).thenCompose(stage -> stage);
    }

    private CompletionStage<Void> applyRelocationAbortCore(
            ZLinkServiceM6BWireCodec.SessionRelocationRoute command) {
        List<HeldIngress> held;
        SessionRelocationKey key = relocationKey(command);
        RouteTerminal recorded = routeTerminals.get(key);
        if (recorded != null) {
            if (!recorded.command().equals(command)) {
                return protocolRouteConflict("abort conflicts with its recorded route terminal");
            }
            return CompletableFuture.completedFuture(null);
        }
        RouteFlight flight = routeFlights.get(key);
        if (flight != null) {
            if (!flight.command().equals(command)) {
                return protocolRouteConflict("abort conflicts with an in-flight route update");
            }
            return flight.completion();
        }
        SealTerminal terminal = sealTerminals.get(key);
        IngressGate gate = ingressGates.get(command.actor().actorId());
        if (terminal == null
                || terminal.consumed()
                || !terminal.completion().isDone()
                || !sealMatchesRoute(terminal.seal(), command)
                || gate == null
                || !command.relocation().equals(gate.seal)) {
            return CompletableFuture.completedFuture(null);
        }
        terminal.consume();
        gate.seal = null;
        held = gate.detachHeld();
        recordRouteTerminalLocked(command);
        pruneSpentSealTerminals();
        PushQueue pushes = pushQueues.get(command.actor().actorId());
        if (pushes != null) {
            CompletableFuture.runAsync(() -> startPushDrain(pushes));
        }
        resumeHeld(held);
        return CompletableFuture.completedFuture(null);
    }

    private CompletionStage<Void> protocolRouteConflict(String message) {
        return CompletableFuture.failedFuture(
                new ZLinkConfigurationException(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, message));
    }

    private void startRoutePreparation(RouteFlight flight) {
        CompletionStage<Void> preparation;
        try {
            preparation =
                    flight.actor()
                            .prepareNativeActorRoute(flight.targetActor(), RELAY_SUBMIT_TIMEOUT);
        } catch (RuntimeException failure) {
            preparation = CompletableFuture.failedFuture(failure);
        }
        preparation.whenComplete(
                (ignored, failure) -> {
                    if (failure != null) {
                        compensateRouteFlight(flight, unwrapRouteFailure(failure));
                    } else {
                        commitPreparedRouteFlight(flight);
                    }
                });
    }

    private void commitPreparedRouteFlight(RouteFlight flight) {
        RouteCommitState state =
                inStateLane(
                        () -> {
                            List<HeldIngress> held = null;
                            PushQueue pushes = null;
                            Throwable failure = null;
                            SessionRelocationKey key = relocationKey(flight.command());
                            StoredBindingRoute current =
                                    bindingRoutes.get(flight.command().actor().actorId());
                            SealTerminal seal = sealTerminals.get(key);
                            IngressGate gate = ingressGates.get(flight.command().actor().actorId());
                            ZLinkBoundActor liveActor =
                                    bound.stream()
                                            .filter(
                                                    candidate ->
                                                            candidate
                                                                    .actorId()
                                                                    .equals(
                                                                            flight.command()
                                                                                    .actor()
                                                                                    .actorId()))
                                            .map(ZLinkBoundActor.class::cast)
                                            .findFirst()
                                            .orElse(null);
                            if (routeFlights.get(key) != flight
                                    || current != flight.sourceRoute()
                                    || !current.matchesSource(flight.update())
                                    || liveActor != flight.actor()
                                    || seal == null
                                    || seal.consumed()
                                    || !seal.completion().isDone()
                                    || !sealMatchesRoute(seal.seal(), flight.command())
                                    || gate == null
                                    || !flight.command().relocation().equals(gate.seal)) {
                                failure =
                                        new ZLinkConfigurationException(
                                                "Session route or authority changed during native"
                                                        + " preparation: "
                                                        + flight.command().actor().actorId());
                            } else {
                                flight.actor().commitPreparedNativeActorRoute(flight.targetActor());
                                bindingRoutes.put(
                                        flight.command().actor().actorId(),
                                        current.toTarget(flight.update()));
                                seal.consume();
                                gate.seal = null;
                                held = gate.detachHeld();
                                recordRouteTerminalLocked(flight.command());
                                routeFlights.remove(key, flight);
                                pruneSpentSealTerminals();
                                pushes = pushQueues.get(flight.command().actor().actorId());
                            }
                            return new RouteCommitState(held, pushes, failure);
                        });
        if (state.failure() != null) {
            compensateRouteFlight(flight, state.failure());
            return;
        }
        startPushDrain(state.pushes());
        resumeHeld(state.held());
        flight.completion().completeAsync(() -> null);
    }

    private record RouteCommitState(List<HeldIngress> held, PushQueue pushes, Throwable failure) {}

    private void compensateRouteFlight(RouteFlight flight, Throwable failure) {
        CompletionStage<Void> compensation;
        try {
            compensation =
                    flight.actor()
                            .compensatePreparedNativeActorRoute(
                                    flight.sourceActor(), RELAY_SUBMIT_TIMEOUT);
        } catch (RuntimeException compensationFailure) {
            compensation = CompletableFuture.failedFuture(compensationFailure);
        }
        compensation.whenComplete(
                (ignored, compensationFailure) -> {
                    Throwable terminal = failure;
                    if (compensationFailure != null) {
                        terminal.addSuppressed(unwrapRouteFailure(compensationFailure));
                    }
                    inStateLane(
                            () -> {
                                routeFlights.remove(relocationKey(flight.command()), flight);
                                return null;
                            });
                    flight.completion()
                            .completeAsync(
                                    () -> {
                                        throw new CompletionException(terminal);
                                    });
                });
    }

    private static Throwable unwrapRouteFailure(Throwable failure) {
        Throwable current = failure;
        while (current instanceof CompletionException && current.getCause() != null) {
            current = current.getCause();
        }
        return current;
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
            if (actorId == null
                    || actorId.isBlank()
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
            long bindingGeneration,
            long lastAcceptedSessionSequence) {
        boolean matchesSource(RelocationRouteUpdate update) {
            return actorId.equals(update.actorId())
                    && objectGeneration == update.objectGeneration()
                    && nodeRid.equals(update.sourceNodeRid())
                    && bindingGeneration == update.bindingGeneration();
        }

        StoredBindingRoute toTarget(RelocationRouteUpdate update) {
            return new StoredBindingRoute(
                    actorId,
                    objectGeneration,
                    meshName,
                    update.targetNodeRid(),
                    bindingGeneration,
                    lastAcceptedSessionSequence);
        }

        StoredBindingRoute toNativeTarget(ZLinkBackendActorRef targetActor) {
            return new StoredBindingRoute(
                    actorId,
                    objectGeneration,
                    meshName,
                    targetActor.nodeRid(),
                    bindingGeneration,
                    lastAcceptedSessionSequence);
        }

        StoredBindingRoute withAcceptedHighWater(long highWater) {
            return new StoredBindingRoute(
                    actorId, objectGeneration, meshName, nodeRid, bindingGeneration, highWater);
        }
    }

    private static final class IngressGate {
        private final long objectGeneration;
        private final long bindingGeneration;
        private long acceptedHighWater;
        private long activeIngress;
        private ZLinkServiceM6BWireCodec.RelocationIdentity seal;
        private final ArrayDeque<HeldIngress> held = new ArrayDeque<>();

        private IngressGate(long objectGeneration, long bindingGeneration) {
            this.objectGeneration = objectGeneration;
            this.bindingGeneration = bindingGeneration;
        }

        private long accept(long sequence) {
            if (sequence <= 0 || sequence <= acceptedHighWater) {
                throw new ZLinkConfigurationException("Session ingress sequence is not monotonic");
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
            CompletableFuture<Void> result) {}

    private boolean isCurrentBinding(ZLinkBoundActor actor) {
        return actor != null && bound.contains(actor);
    }

    private CompletionStage<Void> awaitRouteReady(ZLinkBackendActorRef ref) {
        return ZLinkActorRetryScheduler.waitUntilRelay(
                RELAY_SUBMIT_TIMEOUT,
                () -> routeReady.test(ref.nodeRid()),
                () -> {},
                () -> {
                    String message =
                            "session relay route was not ready before timeout: " + ref.actorId();
                    //  Spec 32-framework-error-model:90 — a route wait past its
                    //  deadline is DeadlineExceeded, not a raw language timeout.
                    //  The TimeoutException cause is kept for diagnostics.
                    return new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                            message,
                            new TimeoutException(message));
                });
    }
}
