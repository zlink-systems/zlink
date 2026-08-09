package systems.zlink.framework.runtime.actors;
import java.util.Objects;
import java.util.concurrent.CompletionException;
import java.util.concurrent.ConcurrentHashMap;

import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;

import systems.zlink.framework.runtime.internal.backend.*;

import java.time.Duration;
import java.util.List;
import java.util.Optional;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.TimeoutException;
import java.util.function.Predicate;
import java.util.function.Supplier;
import java.util.logging.Logger;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.streams.ZLinkSessionActor;
import systems.zlink.framework.streams.ZLinkSessionActors;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;

public final class ZLinkSessionActorsRuntime implements ZLinkSessionActors {
    private static final Logger LOGGER = Logger.getLogger(ZLinkSessionActorsRuntime.class.getName());
    static final Duration RELAY_SUBMIT_TIMEOUT = Duration.ofSeconds(30);
    private static final boolean STREAM_TRACE =
        "1".equals(System.getenv("ZLINK_JAVA_STREAM_TRACE"));
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
    private final List<ZLinkSessionActor> bound = new CopyOnWriteArrayList<>();
    private final ConcurrentHashMap<String, StoredBindingRoute>
        bindingRoutes = new ConcurrentHashMap<>();
    private final ConcurrentHashMap<String, CompletableFuture<Void>>
        bindingTransitions = new ConcurrentHashMap<>();
    private final AtomicLong bindingGenerations = new AtomicLong();
    private ZLinkRelayMetadataPolicy metadataPolicy = ZLinkRelayMetadataPolicy.EMPTY;

    public ZLinkSessionActorsRuntime metadataPolicy(
        Set<String> sessionToActorKeys,
        Set<String> actorToSessionKeys) {
        metadataPolicy =
            new ZLinkRelayMetadataPolicy(sessionToActorKeys, actorToSessionKeys);
        return this;
    }

    @FunctionalInterface
    public interface LocalActorDispatcher {
        CompletionStage<Optional<Message>> dispatch(
            ZLinkBackendActorRef actor,
            ZLinkStreamHeader header,
            Message payload);
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
    }

    @Override
    public List<ZLinkSessionActor> bound() {
        return List.copyOf(bound);
    }

    @Override
    public CompletionStage<ZLinkSessionActor> bind(ZLinkActor actor) {
        return bindManagedAsync(actor);
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
        return notifyDisconnectedAll(RELAY_SUBMIT_TIMEOUT);
    }

    public CompletionStage<Void> notifyDisconnectedAll(Duration timeout) {
        Objects.requireNonNull(timeout, "timeout");
        List<ZLinkSessionActor> current = List.copyOf(bound);
        return CompletableFuture.allOf(current.stream()
            .map(actor -> notifyDisconnectedSafely(actor, timeout))
            .toArray(CompletableFuture[]::new))
            .whenComplete((ignored, error) -> current.forEach(
                this::removeBinding));
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
        trace("session-actor bind-start sessionRid=" + sessionRid
            + " actorNode=" + ref.nodeRid()
            + " actorId=" + ref.actorId()
            + " generation=" + ref.generation());
        if (actors != null) {
            Optional<ZLinkActor> localActor = actors.localActor(ref.actorId());
            if (localActor.isPresent()) {
                ZLinkBackendActorRef localRef = actors.refFor(localActor.get());
                if (localRef.nodeRid().equals(ref.nodeRid())
                    && localRef.actorId().equals(ref.actorId())
                    && localRef.generation() == ref.generation()) {
                    trace("session-actor bind-local sessionRid=" + sessionRid
                        + " actorNode=" + ref.nodeRid()
                        + " actorId=" + ref.actorId()
                        + " generation=" + ref.generation());
                    return bindManagedAsync(localActor.get());
                }
            }
        }
        CompletionStage<Void> authorityReady = actors == null
            ? CompletableFuture.completedFuture(null)
            : actors.prepareRemoteSessionBinding(ref);
        return authorityReady.thenCompose(ignored -> replaceBinding(
            ref.actorId(),
            () -> awaitRouteReady(ref).thenCompose(routeReadyIgnored -> {
                trace("session-actor bind-native-submit sessionRid=" + sessionRid
                    + " actorNode=" + ref.nodeRid()
                    + " actorId=" + ref.actorId()
                    + " generation=" + ref.generation());
                return ZLinkBoundSessionRuntime.bindActorWithRetry(
                    stream, sessionRid, ref, RELAY_SUBMIT_TIMEOUT);
            })
            .thenApply(bindIgnored -> {
                trace("session-actor bind-native-ok sessionRid=" + sessionRid
                    + " actorNode=" + ref.nodeRid()
                    + " actorId=" + ref.actorId()
                    + " generation=" + ref.generation());
                AtomicReference<ZLinkBoundActor> binding = new AtomicReference<>();
                ZLinkBoundActor actor = new ZLinkBoundActor(
                    stream,
                    sessionRid,
                    ref,
                    meshName,
                    Optional.empty(),
                    actors,
                    serializer,
                    0,
                    routeReady,
                    null,
                    true,
                    defaultCodec,
                    RELAY_HEADERS,
                    flow,
                    () -> isCurrentBinding(binding.get()),
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
                    trace("session-actor bind-native-error sessionRid=" + sessionRid
                        + " actorNode=" + ref.nodeRid()
                        + " actorId=" + ref.actorId()
                        + " generation=" + ref.generation()
                        + " error=" + errorSummary(error));
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

    CompletionStage<ZLinkSessionActor> bindManagedAsync(ZLinkActor actor) {
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
                    sourceSessionRid);
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
                    routeReady,
                    localActorDispatcher,
                    nativeSessionRelayAttached,
                    defaultCodec,
                    RELAY_HEADERS,
                    flow,
                    () -> isCurrentBinding(binding.get()),
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
        bindingRoutes.computeIfPresent(
            actor.actorId(),
            (ignored, current) -> current.toNativeTarget(targetActor));
    }

    private CompletionStage<ZLinkBoundActor> replaceBinding(
        String actorId,
        Supplier<CompletionStage<ZLinkBoundActor>> createBinding) {
        CompletableFuture<Void> previousTransition;
        CompletableFuture<Void> tail = new CompletableFuture<>();
        synchronized (this) {
            previousTransition = bindingTransitions.get(actorId);
            bindingTransitions.put(actorId, tail);
        }
        CompletionStage<Void> ready = previousTransition == null
            ? CompletableFuture.completedFuture(null)
            : previousTransition.handle((ignored, failure) -> null);
        CompletionStage<ZLinkBoundActor> installation = ready
            .thenCompose(ignored -> installReplacement(
                actorId, createBinding));
        installation.whenComplete((ignored, failure) -> {
            tail.complete(null);
            synchronized (this) {
                bindingTransitions.remove(actorId, tail);
            }
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

    private ZLinkBoundActor installBinding(ZLinkBoundActor actor) {
        bound.add(actor);
        ActorRef current = actor.ref();
        bindingRoutes.put(actor.actorId(), new StoredBindingRoute(
            current.actorId(),
            current.objectGeneration(),
            current.meshName(),
            current.nodeRid(),
            0,
            0,
            0,
            bindingGenerations.incrementAndGet(),
            0));
        return actor;
    }

    private void removeBinding(ZLinkSessionActor actor) {
        if (bound.remove(actor)) {
            bindingRoutes.computeIfPresent(actor.actorId(),
                (ignored, route) -> bound.stream().anyMatch(
                    candidate -> candidate.actorId().equals(actor.actorId()))
                        ? route
                        : null);
        }
    }

    /**
     * Applies relocation command 44 to this Session owner's stored route.
     * Successful completion is command 45 ACK; the route is changed only
     * after the target route is Ready and the exact source route still matches.
     */
    CompletionStage<Void> applyRelocationRouteUpdate(
        RelocationRouteUpdate update) {
        Objects.requireNonNull(update, "update");
        StoredBindingRoute observed = bindingRoutes.get(update.actorId());
        if (observed == null || !observed.matchesSource(update)) {
            return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                "stored binding route does not match relocation source: "
                    + update.actorId()
                    + (observed == null
                        ? " observed=none"
                        : " observedNode=" + observed.nodeRid()
                            + " observedAuthority="
                            + observed.authorityOwnerGeneration()
                            + " observedBinding=" + observed.bindingGeneration()
                            + " observedSeq="
                            + observed.lastAcceptedSessionSequence()
                            + " sourceNode=" + update.sourceNodeRid()
                            + " sourceAuthority="
                            + update.sourceAuthorityOwnerGeneration()
                            + " updateBinding=" + update.bindingGeneration()
                            + " updateSeq="
                            + update.lastAcceptedSessionSequence())));
        }
        ZLinkBackendActorRef target = new ZLinkBackendActorRef(
            update.targetNodeRid(),
            update.actorId(),
            update.objectGeneration());
        if (spotNode != null) {
            spotNode.rememberActorAuthority(
                target,
                update.targetAuthorityOwnerGeneration(),
                spotNode.localAuthorityLeaseGeneration());
        }
        return awaitRouteReady(target).thenCompose(ignored -> {
            ZLinkBoundActor actor;
            synchronized (this) {
                StoredBindingRoute current = bindingRoutes.get(
                    update.actorId());
                if (current == null || !current.matchesSource(update)) {
                    throw new ZLinkConfigurationException(
                        "stored binding route changed before relocation ACK: "
                            + update.actorId());
                }
                actor = bound.stream()
                    .filter(candidate -> candidate.actorId().equals(
                        update.actorId()))
                    .map(ZLinkBoundActor.class::cast)
                    .findFirst()
                    .orElseThrow(() -> new ZLinkConfigurationException(
                        "bound Actor disappeared before relocation ACK: "
                            + update.actorId()));
            }
            return actor.rebindNativeActorRoute(target, RELAY_SUBMIT_TIMEOUT)
                .thenRun(() -> {
                    synchronized (this) {
                        StoredBindingRoute current = bindingRoutes.get(
                            update.actorId());
                        if (current == null || !current.matchesSource(update)) {
                            throw new ZLinkConfigurationException(
                                "stored binding route changed before relocation ACK: "
                                    + update.actorId());
                        }
                        bindingRoutes.put(
                            update.actorId(),
                            current.toTarget(update));
                    }
                });
        });
    }

    public CompletionStage<ZLinkServiceM6BWireCodec.SessionRelocationRouted>
        applyRelocationRouteCommand(
            ZLinkServiceM6BWireCodec.SessionRelocationRoute command) {
        Objects.requireNonNull(command, "command");
        if (!sessionRid.equals(command.session().sessionRid())) {
            return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                "Session relocation route command does not target this Session"));
        }
        if (command.action()
                != ZLinkServiceM6BWireCodec.SessionRelocationRouteAction.COMMIT) {
            //  Abort route update. Internals 12 §"Ready 시점": a pre-owner-change
            //  abort never changed the Session route, so there is no route to
            //  cancel and nobody waits for a cancel response; §"Session route":
            //  commands 44/45 are used only for the post-`Completed` route
            //  switch and its ACK; spec 20 §6: after commit the route is never
            //  rolled back to the source. So an abort action must leave this
            //  binding's route and location snapshot untouched. It still gets a
            //  terminal ACK — an unanswered command 44 is retransmitted for as
            //  long as the sender lives (spec 20 §5 step 8).
            LOGGER.warning("[zlink-java-stream-trace] session-route abort"
                + " acknowledged without a route change actor="
                + command.actor().actorId()
                + " authority=" + command.currentAuthorityOwnerGeneration());
            return CompletableFuture.completedFuture(echoRouted(command));
        }
        StoredBindingRoute observed = bindingRoutes.get(command.actor().actorId());
        //  Idempotent retransmit (AlreadyApplied): a command 44 whose target
        //  route this owner already installed must re-ACK instead of failing
        //  the source-fence CAS, or a lost command 45 leaves the target
        //  retrying forever (spec 20 §5 idempotency).
        if (observed != null
            && observed.bindingGeneration()
                == command.session().bindingGeneration()
            && observed.objectGeneration() == command.actor().generation()
            && observed.nodeRid().equals(command.targetNodeRid())
            && observed.authorityOwnerGeneration()
                == command.currentAuthorityOwnerGeneration()) {
            LOGGER.warning("[zlink-java-stream-trace] session-route already-applied"
                + " actor=" + command.actor().actorId()
                + " target=" + command.targetNodeRid()
                + " authority=" + command.currentAuthorityOwnerGeneration());
            return CompletableFuture.completedFuture(echoRouted(command));
        }
        //  Spec 20 §5 asks the Session owner to check that the request's
        //  generation and high-water equal the values recorded in the current
        //  binding. Exact equality presumes the `sessionRelocationSeal(42)` /
        //  `sessionRelocationSealed(43)` handshake, which makes the source's
        //  captured high-water and this owner's accepted high-water provably
        //  the same number. Commands 42/43 are not implemented here, so this
        //  owner has no accepted-sequence counter to compare against: the
        //  stored value is only the last applied route update's high-water
        //  (zero before the first relocation) while the command carries the
        //  source Actor owner's ingress count. Until 42/43 exist the gate
        //  stays monotonic — a command whose high-water regressed below an
        //  already applied one is a superseded relocation and is rejected.
        if (observed == null
            || observed.bindingGeneration()
                != command.session().bindingGeneration()
            || observed.lastAcceptedSessionSequence()
                > command.lastAcceptedSessionSequence()) {
            LOGGER.warning(staleFenceDiagnostic(command, observed));
            return CompletableFuture.completedFuture(rejectedRouted(command));
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
                command.session().bindingGeneration(),
                command.lastAcceptedSessionSequence());
        } catch (RuntimeException invalid) {
            LOGGER.warning(staleFenceDiagnostic(command, observed)
                + " reason=" + invalid.getMessage());
            return CompletableFuture.completedFuture(rejectedRouted(command));
        }
        return applyRelocationRouteUpdate(update).handle((ignored, failure) -> {
            if (failure == null) {
                return CompletableFuture.completedFuture(echoRouted(command));
            }
            Throwable cause = failure;
            while (cause instanceof CompletionException && cause.getCause() != null) {
                cause = cause.getCause();
            }
            if (cause instanceof ZLinkConfigurationException) {
                //  Source-fence mismatch is terminal for this command: the
                //  stored route diverged, so retransmitting the same fence can
                //  never succeed. Reply the rejection so the target stops.
                LOGGER.warning(staleFenceDiagnostic(command,
                        bindingRoutes.get(command.actor().actorId()))
                    + " reason=" + cause.getMessage());
                return CompletableFuture.completedFuture(rejectedRouted(command));
            }
            //  Transient failures (route-ready timeout, relay submit) stay
            //  unreplied; the target retries the same fence.
            return CompletableFuture.<ZLinkServiceM6BWireCodec
                .SessionRelocationRouted>failedFuture(failure);
        }).thenCompose(stage -> stage);
    }

    private static ZLinkServiceM6BWireCodec.SessionRelocationRouted echoRouted(
        ZLinkServiceM6BWireCodec.SessionRelocationRoute command) {
        return new ZLinkServiceM6BWireCodec.SessionRelocationRouted(
            command.relocation(),
            command.coordinator(),
            command.actor(),
            command.session(),
            command.action(),
            command.currentAuthorityOwnerGeneration(),
            command.lastAcceptedSessionSequence());
    }

    //  Rejection ACK: echoes the command identity with the action flipped to
    //  ABORT. The target treats an action flip as the spec `Stale` result -
    //  terminal for this relocation's route command.
    private static ZLinkServiceM6BWireCodec.SessionRelocationRouted rejectedRouted(
        ZLinkServiceM6BWireCodec.SessionRelocationRoute command) {
        return new ZLinkServiceM6BWireCodec.SessionRelocationRouted(
            command.relocation(),
            command.coordinator(),
            command.actor(),
            command.session(),
            ZLinkServiceM6BWireCodec.SessionRelocationRouteAction.ABORT,
            command.currentAuthorityOwnerGeneration(),
            command.lastAcceptedSessionSequence());
    }

    private static String staleFenceDiagnostic(
        ZLinkServiceM6BWireCodec.SessionRelocationRoute command,
        StoredBindingRoute observed) {
        return "[zlink-java-stream-trace] session-route stale-fence rejected"
            + " actor=" + command.actor().actorId()
            + " commandBinding=" + command.session().bindingGeneration()
            + " commandSeq=" + command.lastAcceptedSessionSequence()
            + " commandPrevAuthority=" + command.previousAuthorityOwnerGeneration()
            + " commandTargetAuthority=" + command.currentAuthorityOwnerGeneration()
            + " commandTarget=" + command.targetNodeRid()
            + " commandGeneration=" + command.actor().generation()
            + (observed == null
                ? " observed=none"
                : " observedBinding=" + observed.bindingGeneration()
                    + " observedSeq=" + observed.lastAcceptedSessionSequence()
                    + " observedAuthority=" + observed.authorityOwnerGeneration()
                    + " observedNode=" + observed.nodeRid()
                    + " observedGeneration=" + observed.objectGeneration());
    }

    record RelocationRouteUpdate(
        String actorId,
        long objectGeneration,
        RoutingId sourceNodeRid,
        RoutingId targetNodeRid,
        long targetNodeGeneration,
        long sourceAuthorityOwnerGeneration,
        long targetAuthorityOwnerGeneration,
        long bindingGeneration,
        long lastAcceptedSessionSequence) {
        RelocationRouteUpdate(
            String actorId,
            long objectGeneration,
            RoutingId sourceNodeRid,
            RoutingId targetNodeRid,
            long targetNodeGeneration,
            long sourceAuthorityOwnerGeneration,
            long targetAuthorityOwnerGeneration) {
            this(actorId, objectGeneration, sourceNodeRid, targetNodeRid,
                targetNodeGeneration, sourceAuthorityOwnerGeneration,
                targetAuthorityOwnerGeneration, 0, 0);
        }

        RelocationRouteUpdate {
            if (actorId == null || actorId.isBlank()
                || objectGeneration <= 0
                || targetNodeGeneration <= 0
                || sourceAuthorityOwnerGeneration <= 0
                || bindingGeneration < 0
                || lastAcceptedSessionSequence < 0
                || targetAuthorityOwnerGeneration
                    <= sourceAuthorityOwnerGeneration) {
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
                && (update.bindingGeneration() == 0
                    || bindingGeneration == update.bindingGeneration())
                && lastAcceptedSessionSequence
                    <= update.lastAcceptedSessionSequence()
                && (authorityOwnerGeneration == 0
                    || authorityOwnerGeneration
                        == update.sourceAuthorityOwnerGeneration());
        }

        StoredBindingRoute toTarget(RelocationRouteUpdate update) {
            return new StoredBindingRoute(
                actorId,
                objectGeneration,
                meshName,
                update.targetNodeRid(),
                update.targetNodeGeneration(),
                update.targetAuthorityOwnerGeneration(),
                ownerLeaseGeneration,
                bindingGeneration,
                update.lastAcceptedSessionSequence());
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
    }

    private boolean isCurrentBinding(ZLinkBoundActor actor) {
        return actor != null && bound.contains(actor);
    }

    private CompletionStage<Void> awaitRouteReady(ZLinkBackendActorRef ref) {
        return ZLinkActorRetryScheduler.waitUntilRelay(
            RELAY_SUBMIT_TIMEOUT,
            () -> routeReady.test(ref.nodeRid()),
            () -> trace("session-actor route-ready sessionRid=" + sessionRid
                + " actorNode=" + ref.nodeRid()
                + " actorId=" + ref.actorId()
                + " generation=" + ref.generation()),
            () -> new TimeoutException(
                "session relay route was not ready before timeout: "
                    + ref.actorId()));
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
