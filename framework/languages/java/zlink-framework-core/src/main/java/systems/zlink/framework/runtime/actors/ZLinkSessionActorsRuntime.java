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
import systems.zlink.framework.runtime.protocol.ServiceWirePilotCodec;
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
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.LongFunction;
import java.util.function.LongSupplier;
import java.util.function.Supplier;
import java.util.logging.Level;
import java.util.logging.Logger;

public final class ZLinkSessionActorsRuntime implements ZLinkSessionActors {
    private static final Logger LOGGER =
            Logger.getLogger(ZLinkSessionActorsRuntime.class.getName());
    static final Duration RELAY_SUBMIT_TIMEOUT = Duration.ofSeconds(30);
    private final ZLinkSessionRelayHeaders relayHeaders = new ZLinkSessionRelayHeaders();
    private final ZLinkBackendStreamSocket stream;

    private ZLinkSessionDisconnected disconnectIdentity(long generation) {
        return new ZLinkSessionDisconnected(
                spotNode == null ? null : spotNode.routingId(),
                spotNode == null ? 0 : spotNode.localNodeGeneration(),
                generation);
    }

    private final ZLinkInternalSpotNode spotNode;
    private final RoutingId sessionRid;
    private final ZLinkActorRuntime actors;
    private final ZLinkMessageSerializer serializer;
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
    //  Command 42 `sealed` result records keyed by (relocation identity, ActorId)
    //  (Session-Actor binding §8.3). A binding keeps its open seal record and at
    //  most its last closed one; the next seal or the binding's removal deletes it.
    private static final int ROUTE_TERMINAL_CAPACITY = 65_536;
    private final java.util.HashMap<SealRecordKey, SealTerminal> sealTerminals =
            new java.util.HashMap<>();
    private final java.util.LinkedHashMap<SessionRelocationKey, RouteTerminal> routeTerminals =
            new java.util.LinkedHashMap<>();
    private final java.util.HashMap<SessionRelocationKey, RouteFlight> routeFlights =
            new java.util.HashMap<>();
    // Binding generations are unique across every Session of this Session owner
    // node lifecycle (Session-Actor binding §4). The node owns the counter.
    private final LongSupplier bindingGenerations;
    private final java.util.HashMap<String, IngressGate> ingressGates = new java.util.HashMap<>();
    private long nextFallbackIngressSequence = 1;
    private int nextActorSlot = 1;
    private ZLinkRelayMetadataPolicy metadataPolicy = ZLinkRelayMetadataPolicy.EMPTY;
    private boolean relocationStopped;
    private boolean relocationFailed;

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
                RoutingId sourceNodeRid,
                RoutingId sourceSessionRid,
                long bindingGeneration,
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

    public void enterRelayDispatch(ZLinkStreamHeader header) {
        relayHeaders.enter(header);
    }

    public void enterRelayDispatch(ZLinkSessionDispatchContext dispatch, ZLinkStreamHeader header) {
        relayHeaders.enter(dispatch, header);
    }

    public void exitRelayDispatch() {
        relayHeaders.exit();
    }

    public void exitRelayDispatch(ZLinkSessionDispatchContext dispatch) {
        relayHeaders.exit(dispatch);
    }

    public ZLinkSessionActorsRuntime(
            ZLinkBackendStreamSocket stream,
            RoutingId sessionRid,
            ZLinkActorRuntime actors,
            ZLinkMessageSerializer serializer) {
        this(null, stream, sessionRid, actors, serializer, null, true, ZLinkStreamCodec.JSON, null);
    }

    public ZLinkSessionActorsRuntime(
            ZLinkBackendStreamSocket stream,
            RoutingId sessionRid,
            ZLinkActorRuntime actors,
            ZLinkMessageSerializer serializer,
            LocalActorDispatcher localActorDispatcher,
            boolean nativeSessionRelayAttached,
            ZLinkStreamCodec defaultCodec) {
        this(
                null,
                stream,
                sessionRid,
                actors,
                serializer,
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
            LocalActorDispatcher localActorDispatcher,
            boolean nativeSessionRelayAttached,
            ZLinkStreamCodec defaultCodec) {
        this(
                spotNode,
                stream,
                sessionRid,
                actors,
                serializer,
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
        // Physical disconnect ends this Session owner's admission before Actor callbacks.
        stopRelocationOwner(
                new ZLinkConfigurationException(
                        "Session disconnected while relocation state was pending"));
        return CompletableFuture.allOf(
                        current.stream()
                                .map(actor -> notifyDisconnectedSafely(actor, timeout))
                                .toArray(CompletableFuture[]::new))
                .whenComplete(
                        (ignored, error) -> {
                            current.forEach(this::removeBinding);
                        });
    }

    private void stopRelocationOwner(Throwable failure) {
        completeRelocationStop(stopRelocationOwnerCore(null, null), failure);
    }

    private StopRelocationState stopRelocationOwnerCore(
            SealTerminal timeoutTerminal, RouteFlight failedRoute) {
        return inStateLane(
                () -> {
                    if (relocationStopped
                            || (timeoutTerminal != null
                                    && (timeoutTerminal.deadline == null
                                            || sealTerminals.get(sealKey(timeoutTerminal.seal()))
                                                    != timeoutTerminal))
                            || (failedRoute != null
                                    && routeFlights.get(relocationKey(failedRoute.command()))
                                            != failedRoute)) {
                        return null;
                    }
                    relocationStopped = true;
                    relocationFailed = timeoutTerminal != null || failedRoute != null;
                    List<HeldIngress> held = new ArrayList<>();
                    ingressGates.values().forEach(gate -> held.addAll(gate.detachHeld()));
                    ingressGates.clear();
                    bindingRoutes.clear();
                    bindingTransitions.clear();
                    List<SealTerminal> seals = List.copyOf(sealTerminals.values());
                    List<RouteFlight> routes = List.copyOf(routeFlights.values());
                    routeFlights.clear();
                    sealTerminals.clear();
                    routeTerminals.clear();
                    List<ZLinkSessionActor> disconnected =
                            relocationFailed ? List.copyOf(bound) : List.of();
                    if (relocationFailed) {
                        bound.clear();
                    }
                    return new StopRelocationState(held, seals, routes, disconnected);
                });
    }

    private static void completeRelocationStop(StopRelocationState stopped, Throwable failure) {
        if (stopped != null) {
            failHeld(stopped.held(), failure);
            stopped.seals().forEach(seal -> seal.fail(failure));
            stopped.routes().forEach(route -> route.completion().completeExceptionally(failure));
        }
    }

    private record StopRelocationState(
            List<HeldIngress> held,
            List<SealTerminal> seals,
            List<RouteFlight> routes,
            List<ZLinkSessionActor> disconnected) {}

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
                                        () -> {
                                            int allocatedActorSlot = allocateActorSlot();
                                            return ZLinkBoundSessionRuntime.bindActorWithRetry(
                                                            stream,
                                                            sessionRid,
                                                            ref,
                                                            allocatedActorSlot,
                                                            RELAY_SUBMIT_TIMEOUT)
                                                    .thenApply(
                                                            bindIgnored -> {
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
                                                                                Optional.empty(),
                                                                                actors,
                                                                                serializer,
                                                                                0,
                                                                                disconnectIdentity(
                                                                                        bindingGeneration),
                                                                                allocatedActorSlot,
                                                                                null,
                                                                                true,
                                                                                defaultCodec,
                                                                                relayHeaders,
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
                                                                        () -> removeBinding(actor));
                                                                return actor;
                                                            });
                                        }))
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
                                            ? ZLinkBoundSessionRuntime.bindActorWithRetry(
                                                    stream,
                                                    sessionRid,
                                                    ref,
                                                    actorSlot,
                                                    RELAY_SUBMIT_TIMEOUT)
                                            : CompletableFuture.completedFuture(null);
                            return nativeBinding.thenCompose(
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
                                                        metadataPolicy);
                                        RoutingId sourceNodeRid =
                                                nativeSessionRelayAttached && spotNode != null
                                                        ? spotNode.routingId()
                                                        : null;
                                        RoutingId sourceSessionRid =
                                                nativeSessionRelayAttached ? sessionRid : null;
                                        return actors.bindSession(
                                                        actor,
                                                        boundSession,
                                                        sourceNodeRid,
                                                        sourceSessionRid,
                                                        bindingGeneration,
                                                        spotNode == null
                                                                ? 0
                                                                : spotNode.localNodeGeneration())
                                                .thenApply(
                                                        bindingToken -> {
                                                            boundSession.setBindingToken(
                                                                    bindingToken);
                                                            boundSession.setActorSlot(actorSlot);
                                                            AtomicReference<ZLinkBoundActor>
                                                                    binding =
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
                                                                            disconnectIdentity(
                                                                                    bindingGeneration),
                                                                            actorSlot,
                                                                            localActorDispatcher,
                                                                            nativeSessionRelayAttached,
                                                                            defaultCodec,
                                                                            relayHeaders,
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
                                                            binding.set(boundActor);
                                                            boundSession.setUnbindListener(
                                                                    () ->
                                                                            removeBinding(
                                                                                    boundActor));
                                                            boundActor.setUnbindListener(
                                                                    () ->
                                                                            removeBinding(
                                                                                    boundActor));
                                                            boundSession.setRebindListener(
                                                                    target ->
                                                                            recordNativeRebind(
                                                                                    boundActor,
                                                                                    target));
                                                            return boundActor;
                                                        });
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
                    if (nextActorSlot > ZLinkBoundActor.MAX_ACTOR_SLOT) {
                        throw new ZLinkFrameworkException(
                                ZLinkFrameworkErrorKind.INVALID_OPERATION,
                                "Session Actor slots are exhausted");
                    }
                    return nextActorSlot++;
                });
    }

    private CompletionStage<Void> announceBound(ZLinkBoundActor actor) {
        return submitActorControl(
                ZLinkStreamActorControl.BOUND,
                ZLinkStreamActorControl.bound(actor.actorSlot(), actor.actorId()),
                true);
    }

    private void announceUnbound(ZLinkBoundActor actor) {
        submitActorControl(
                        ZLinkStreamActorControl.UNBOUND,
                        ZLinkStreamActorControl.unbound(actor.actorSlot()),
                        false)
                .whenComplete(
                        (ignored, failure) -> {
                            if (failure != null) {
                                LOGGER.log(
                                        Level.WARNING, "Actor unbound submission failed", failure);
                            }
                        });
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
                SealTerminal terminal =
                        sealTerminals.get(new SealRecordKey(previous.seal, actor.actorId()));
                if (terminal != null && !terminal.consumed()) {
                    terminal.consume();
                    abandonedSeal = terminal;
                }
            }
            removeSealRecords(actor.actorId());
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
                                IngressGate removed = ingressGates.remove(actor.actorId());
                                if (removed != null) {
                                    abandoned = removed.detachHeld();
                                    if (removed.seal != null) {
                                        SealTerminal terminal =
                                                sealTerminals.get(
                                                        new SealRecordKey(
                                                                removed.seal, actor.actorId()));
                                        if (terminal != null && !terminal.consumed()) {
                                            terminal.consume();
                                            abandonedSeal = terminal;
                                        }
                                    }
                                }
                                removeSealRecords(actor.actorId());
                            }
                            return new BindingCleanup(abandoned, abandonedSeal);
                        });
        if (cleanup != null) {
            if (!inStateLane(() -> relocationStopped)) {
                announceUnbound((ZLinkBoundActor) actor);
            }
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
     * method and command 42 use the Session owner turn, so the ACK high-water cannot race a
     * post-seal relay into the captured prefix.
     */
    private CompletionStage<Void> admitIngress(
            ZLinkBoundActor actor, LongFunction<CompletionStage<Void>> operation) {
        Objects.requireNonNull(operation, "operation");
        IngressAdmissionState admission =
                inStateLane(
                        () -> {
                            if (!isCurrentBinding(actor)) {
                                return new IngressAdmissionState(
                                        null,
                                        CompletableFuture.<Void>failedFuture(
                                                new ZLinkFrameworkException(
                                                        ZLinkFrameworkErrorKind.UNAVAILABLE,
                                                        "bound Actor is no longer the current"
                                                                + " Session binding: "
                                                                + actor.actorId())));
                            }
                            StoredBindingRoute route = bindingRoutes.get(actor.actorId());
                            IngressGate gate = ingressGates.get(actor.actorId());
                            if (route == null || gate == null) {
                                return new IngressAdmissionState(
                                        null,
                                        CompletableFuture.<Void>failedFuture(
                                                new ZLinkFrameworkException(
                                                        ZLinkFrameworkErrorKind.UNAVAILABLE,
                                                        "Session ingress binding state is"
                                                                + " unavailable: "
                                                                + actor.actorId())));
                            }
                            if (gate.seal != null) {
                                CompletableFuture<Void> held = new CompletableFuture<>();
                                gate.held.addLast(new HeldIngress(actor, operation, held));
                                return new IngressAdmissionState(null, held);
                            }
                            long accepted = allocateIngressSequence(gate);
                            gate.activeIngress++;
                            CompletionStage<Void> submission = invokeIngress(operation, accepted);
                            return new IngressAdmissionState(gate, submission);
                        });
        if (admission.gate() != null) {
            admission
                    .submission()
                    .whenComplete((ignored, failure) -> completeIngress(actor, admission.gate()));
        }
        return admission.submission();
    }

    private record IngressAdmissionState(IngressGate gate, CompletionStage<Void> submission) {}

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
                                                    new SealRecordKey(
                                                            admittedGate.seal, actor.actorId()));
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

    private static void failHeld(List<HeldIngress> held, Throwable failure) {
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
     * Applies command 42 to this Session's current binding of the command's ActorId (Session-Actor
     * binding §8.3). The Session owner ({@code ZLinkStreamRuntime}) routes the command here only
     * when this Session holds that ActorId's current binding.
     *
     * <p>The stage completes with {@code sealed}, with {@code bindingAbsent} when the binding
     * disappeared before this turn, or with {@code null} when the command duplicates a closed
     * record and must not be answered. A mismatch completes exceptionally.
     */
    public CompletionStage<ZLinkServiceM6BWireCodec.SessionRelocationSealed>
            applyRelocationSealCommand(ZLinkServiceM6BWireCodec.SessionRelocationSeal command) {
        return onStateLane(() -> applyRelocationSealCommandCore(command))
                .thenCompose(stage -> stage);
    }

    /** Returns the binding generation of this Session's current binding of {@code actorId}. */
    public java.util.OptionalLong boundBindingGeneration(String actorId) {
        StoredBindingRoute route = bindingRoutes.get(actorId);
        return route == null
                ? java.util.OptionalLong.empty()
                : java.util.OptionalLong.of(route.bindingGeneration());
    }

    private CompletionStage<ZLinkServiceM6BWireCodec.SessionRelocationSealed>
            applyRelocationSealCommandCore(ZLinkServiceM6BWireCodec.SessionRelocationSeal command) {
        Objects.requireNonNull(command, "command");
        String actorId = command.actor().actor().actorId();
        SealTerminal recorded = sealTerminals.get(sealKey(command));
        if (recorded != null) {
            if (!recorded.seal().equals(command)) {
                return CompletableFuture.failedFuture(
                        new ZLinkConfigurationException(
                                ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                                "Session relocation seal changed fields of the recorded"
                                        + " command 42: "
                                        + actorId));
            }
            if (recorded.consumed()) {
                LOGGER.warning(
                        "late_session_relocation_seal actor="
                                + actorId
                                + " relocation="
                                + command.relocation());
                return CompletableFuture.completedFuture(null);
            }
            return recorded.completion();
        }
        StoredBindingRoute observed = bindingRoutes.get(actorId);
        IngressGate gate = ingressGates.get(actorId);
        if (relocationStopped || observed == null || gate == null) {
            return CompletableFuture.completedFuture(
                    ZLinkServiceM6BWireCodec.SessionRelocationSealed.replyTo(
                            command, ServiceWirePilotCodec.SessionSealResult.BINDING_ABSENT));
        }
        if (!sessionRid.equals(command.session().sessionRid())
                || !sealFenceMatches(command, observed)
                || gate.objectGeneration != command.actor().actor().generation()
                || gate.bindingGeneration != command.session().bindingGeneration()
                || command.actor().ownerLeaseGeneration() < observed.ownerLeaseGeneration()) {
            return CompletableFuture.failedFuture(
                    new ZLinkConfigurationException(
                            "Session relocation seal fence differs from the current "
                                    + "binding: "
                                    + actorId));
        }
        if (gate.seal != null) {
            return CompletableFuture.failedFuture(
                    new ZLinkConfigurationException(
                            ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                            "Session binding is already sealed by another relocation: " + actorId));
        }
        removeSealRecords(actorId);
        gate.seal = command.relocation();
        SealTerminal installed =
                new SealTerminal(
                        command,
                        ZLinkServiceM6BWireCodec.SessionRelocationSealed.replyTo(
                                command, ServiceWirePilotCodec.SessionSealResult.SEALED));
        sealTerminals.put(sealKey(command), installed);
        installed.armDeadline(() -> expireRelocationSeal(installed), sessionRelocationSealTimeout);
        if (ingressDrainedCore(command)) {
            installed.completeSealed();
        }
        return installed.completion();
    }

    private void removeSealRecords(String actorId) {
        sealTerminals.keySet().removeIf(key -> key.actorId().equals(actorId));
    }

    private void expireRelocationSeal(SealTerminal terminal) {
        //  Spec 32-framework-error-model:38 — a live operation past its deadline
        //  is DeadlineExceeded, not a configuration error.
        RuntimeException failure =
                new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                        "Session relocation route update timed out: "
                                + terminal.seal().actor().actor().actorId());
        closeRelocationSession(failure, terminal, null, terminal.seal().actor().actor().actorId());
    }

    private void closeRelocationSession(
            Throwable failure,
            SealTerminal timeoutTerminal,
            RouteFlight failedRoute,
            String actorId) {
        StopRelocationState stopped = stopRelocationOwnerCore(timeoutTerminal, failedRoute);
        if (stopped == null) {
            return;
        }
        try {
            stream.disconnectPeer(sessionRid);
        } catch (RuntimeException disconnectFailure) {
            failure.addSuppressed(disconnectFailure);
            if (flow != null)
                flow.reportFailure(
                        systems.zlink.framework.runtime.internal.diagnostics
                                .ZLinkDispatchErrorSurface.STREAM_SESSION,
                        systems.zlink.framework.runtime.internal.diagnostics
                                .ZLinkDispatchMessageKind.SEND,
                        systems.zlink.framework.runtime.internal.diagnostics
                                .ZLinkDispatchErrorReason.TARGET_CLOSED,
                        systems.zlink.framework.runtime.internal.diagnostics
                                .ZLinkDispatchErrorAction.DROP,
                        actorId,
                        disconnectFailure);
        }
        stopped.disconnected()
                .forEach(actor -> notifyDisconnectedSafely(actor, RELAY_SUBMIT_TIMEOUT));
        completeRelocationStop(stopped, failure);
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

    /**
     * Admits command 36 by current binding identity and submits it to the physical STREAM owner.
     * Session-Actor binding §8.1 seals ingress only; outbound pushes never wait for command 44.
     */
    public CompletionStage<Boolean> acceptBoundSessionSendAsync(
            ZLinkServiceM6BWireCodec.BoundSessionSend command,
            ZLinkServiceM6AWireCodec.ApplicationPayload payload) {
        Objects.requireNonNull(command, "command");
        Objects.requireNonNull(payload, "payload");
        return stateLane
                .<CompletionStage<Boolean>>runAsync(
                        () -> {
                            if (!isCurrentPushBindingLocked(command)) {
                                return CompletableFuture.completedFuture(false);
                            }
                            return submitBoundSessionPush(
                                    command.actor().actor().actorId(), payload);
                        })
                .thenCompose(submitted -> submitted);
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

    // Physical admission, completion and disconnect cancellation belong to the backend.
    private CompletionStage<Boolean> submitBoundSessionPush(
            String actorId, ZLinkServiceM6AWireCodec.ApplicationPayload payload) {
        List<Message> parts = List.of();
        try {
            parts = ZLinkServiceM6AWireCodec.decodeFrameworkMultipart(payload);
            return stream.sendBoundSessionPushAsync(sessionRid, actorSlotFor(actorId), parts)
                    .thenApply(ignored -> true);
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

    private record SealRecordKey(
            ZLinkServiceM6BWireCodec.RelocationIdentity relocation, String actorId) {}

    private static SealRecordKey sealKey(ZLinkServiceM6BWireCodec.SessionRelocationSeal command) {
        return new SealRecordKey(command.relocation(), command.actor().actor().actorId());
    }

    private static SealRecordKey sealKey(ZLinkServiceM6BWireCodec.SessionRelocationRoute command) {
        return new SealRecordKey(command.relocation(), command.actor().actorId());
    }

    private record SessionRelocationKey(
            ZLinkServiceM6BWireCodec.RelocationIdentity relocation,
            String actorId,
            long objectGeneration,
            RoutingId sessionRid,
            long bindingGeneration) {}

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
            ZLinkBackendActorRef targetActor,
            CompletableFuture<Void> completion) {}

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
        if (routeTerminals.size() >= ROUTE_TERMINAL_CAPACITY) {
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
            if (relocationFailed) {
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
        SessionRelocationKey key = relocationKey(command);
        RouteFlight active = routeFlights.get(key);
        if (active != null) {
            if (!active.command().equals(command)) {
                return protocolRouteConflict("command 44 conflicts with an in-flight route update");
            }
            return active.completion();
        }
        if (command.action() != ZLinkServiceM6BWireCodec.SessionRelocationRouteAction.COMMIT) {
            return applyRelocationAbortCore(command);
        }
        StoredBindingRoute observed = bindingRoutes.get(command.actor().actorId());
        SealTerminal seal = sealTerminals.get(sealKey(command));
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
        RouteFlight flight =
                new RouteFlight(
                        command, observed, update, actor, target, new CompletableFuture<>());
        routeFlights.put(key, flight);
        // §8.1: the deadline ends when this exact command 44 wins the owner turn,
        // before native route preparation. A dequeued expiry observes the same armed state.
        seal.cancelDeadline();
        CompletableFuture.runAsync(() -> startRoutePreparation(flight));
        return flight.completion();
    }

    private CompletionStage<Void> applyRelocationAbortCore(
            ZLinkServiceM6BWireCodec.SessionRelocationRoute command) {
        List<HeldIngress> held;
        SessionRelocationKey key = relocationKey(command);
        SealTerminal terminal = sealTerminals.get(sealKey(command));
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
                            .prepareNativeActorRoute(
                                    flight.targetActor(),
                                    flight.command().currentAuthorityOwnerGeneration(),
                                    flight.command().targetOwnerLeaseGeneration(),
                                    RELAY_SUBMIT_TIMEOUT);
        } catch (RuntimeException failure) {
            preparation = CompletableFuture.failedFuture(failure);
        }
        preparation.whenComplete(
                (ignored, failure) -> {
                    if (failure != null) {
                        failRouteFlight(flight, unwrapRouteFailure(failure));
                    } else {
                        commitPreparedRouteFlight(flight);
                    }
                });
    }

    private void commitPreparedRouteFlight(RouteFlight flight) {
        RouteCommitState state;
        try {
            state =
                    inStateLane(
                            () -> {
                                List<HeldIngress> held = null;
                                Throwable failure = null;
                                SessionRelocationKey key = relocationKey(flight.command());
                                StoredBindingRoute current =
                                        bindingRoutes.get(flight.command().actor().actorId());
                                SealTerminal seal = sealTerminals.get(sealKey(flight.command()));
                                IngressGate gate =
                                        ingressGates.get(flight.command().actor().actorId());
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
                                    flight.actor()
                                            .commitPreparedNativeActorRoute(flight.targetActor());
                                    bindingRoutes.put(
                                            flight.command().actor().actorId(),
                                            current.toTarget(
                                                    flight.update(),
                                                    flight.command().targetOwnerLeaseGeneration()));
                                    seal.consume();
                                    gate.seal = null;
                                    held = gate.detachHeld();
                                    recordRouteTerminalLocked(flight.command());
                                    routeFlights.remove(key, flight);
                                }
                                return new RouteCommitState(held, failure);
                            });
        } catch (RuntimeException failure) {
            failRouteFlight(flight, failure);
            return;
        }
        if (state.failure() != null) {
            failRouteFlight(flight, state.failure());
            return;
        }
        resumeHeld(state.held());
        flight.completion().completeAsync(() -> null);
    }

    private record RouteCommitState(List<HeldIngress> held, Throwable failure) {}

    private void failRouteFlight(RouteFlight flight, Throwable failure) {
        closeRelocationSession(failure, null, flight, flight.command().actor().actorId());
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
            long ownerLeaseGeneration) {
        boolean matchesSource(RelocationRouteUpdate update) {
            return actorId.equals(update.actorId())
                    && objectGeneration == update.objectGeneration()
                    && nodeRid.equals(update.sourceNodeRid())
                    && bindingGeneration == update.bindingGeneration();
        }

        StoredBindingRoute toTarget(RelocationRouteUpdate update, long targetOwnerLeaseGeneration) {
            return new StoredBindingRoute(
                    actorId,
                    objectGeneration,
                    meshName,
                    update.targetNodeRid(),
                    bindingGeneration,
                    targetOwnerLeaseGeneration);
        }

        StoredBindingRoute toNativeTarget(ZLinkBackendActorRef targetActor) {
            return new StoredBindingRoute(
                    actorId,
                    objectGeneration,
                    meshName,
                    targetActor.nodeRid(),
                    bindingGeneration,
                    ownerLeaseGeneration);
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
}
