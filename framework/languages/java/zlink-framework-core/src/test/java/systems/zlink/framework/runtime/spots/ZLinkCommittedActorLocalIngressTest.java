package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.*;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.monitoring.ZLinkListenerKind;
import systems.zlink.framework.runtime.InMemoryRelocationStore;
import systems.zlink.framework.runtime.actors.ZLinkActorRuntime;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateFence;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.runtime.locations.ZLinkStoreLocationResolvers;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec;
import systems.zlink.framework.spots.*;

import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Proxy;
import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.BiFunction;
import java.util.function.Function;

final class ZLinkCommittedActorLocalIngressTest {
    private static final String ACTOR = "committed-player";
    private static final String ROOM = "committed-room";
    private static final String MESH = "committed-ingress";
    private static final RoutingId TARGET = RoutingId.from("committed-target");
    private static final long WAIT_SECONDS = 10;
    private static CompletableFuture<ZLinkActorJoinCompletion> joined;
    private static final AtomicInteger sourceCompletions = new AtomicInteger();
    private static CompletableFuture<Void> handled;
    private static CompletableFuture<Void> firstHandlerRelease;
    private static CompletableFuture<Void> secondHandled;
    private static final String SECOND = "second-player";
    private static final String SECOND_ROOM = "second-room";
    private static ZLinkActorClient pairClient;
    private static Function<ZLinkActorContext, CompletionStage<ZLinkActor>> targetFactory;
    private static final Map<String, CompletableFuture<ZLinkActorJoinCompletion>> PAIR_COMPLETIONS =
            new ConcurrentHashMap<>();
    private static final List<Integer> RECEIVED = new CopyOnWriteArrayList<>();

    @Test
    void readyPublicationAlreadyHasRawActorAndAcceptsLocalSend() throws Exception {
        pairClient = null;
        targetFactory = null;
        joined = new CompletableFuture<>();
        handled = new CompletableFuture<>();
        firstHandlerRelease = new CompletableFuture<>();
        secondHandled = new CompletableFuture<>();
        RECEIVED.clear();
        var locations = new ZLinkInMemoryLocationStore();
        var relocations = new InMemoryRelocationStore();
        var publicationRelease = new CompletableFuture<Void>();
        var committed = new CompletableFuture<Void>();
        try (var source = start(options("committed-source", locations, relocations, null));
                var target =
                        start(
                                options(
                                        "committed-target",
                                        locations,
                                        relocations,
                                        source.listenerStatus(ZLinkListenerKind.ROUTE_MESH, MESH)
                                                .endpoint()))) {
            target.spotManager()
                    .getOrCreate(ROOM, "room")
                    .submit()
                    .toCompletableFuture()
                    .get(WAIT_SECONDS, TimeUnit.SECONDS);
            source.actorManager()
                    .create(ACTOR, "player")
                    .submit()
                    .toCompletableFuture()
                    .get(WAIT_SECONDS, TimeUnit.SECONDS);
            var targetNode = node(target);
            holdCommittedPublication(targetNode, committed, publicationRelease);
            holdCommittedPublication(node(source), committed, publicationRelease);
            try {
                source.actorClient()
                        .requestToActor(ACTOR, new Join(ROOM))
                        .timeout(Duration.ofSeconds(WAIT_SECONDS))
                        .submit(String.class)
                        .toCompletableFuture()
                        .get(WAIT_SECONDS, TimeUnit.SECONDS);
                committed.get(WAIT_SECONDS, TimeUnit.SECONDS);
                var resolvers =
                        (ZLinkStoreLocationResolvers) field(target, "storeLocationResolvers");
                var route =
                        resolvers
                                .resolveActor(ACTOR)
                                .toCompletableFuture()
                                .get(WAIT_SECONDS, TimeUnit.SECONDS);
                assertNotNull(route, "the real Store must expose a Ready Actor route");
                assertEquals(TARGET, route.nodeRid(), "Store commit must select the target owner");
                var preparedRaw = targetNode.actorLookup(ACTOR);
                assertFalse(publicationRelease.isDone());
                assertFalse(joined.isDone(), "Join completion must still await publication");
                assertFalse(handled.isDone());
                var selectedActor =
                        new ZLinkBackendActorRef(
                                route.nodeRid(), ACTOR, route.actorRef().objectGeneration());
                assertRejectedStagingRoute(
                        targetNode,
                        new ZLinkServiceM6BWireCodec.ActorRouteFence(
                                new ZLinkBackendActorRef(
                                        route.nodeRid(), ACTOR, selectedActor.generation() + 1),
                                route.targetNodeGeneration(),
                                route.authorityOwnerGeneration(),
                                route.ownerLeaseGeneration()));
                assertRejectedStagingRoute(
                        targetNode,
                        new ZLinkServiceM6BWireCodec.ActorRouteFence(
                                selectedActor,
                                route.targetNodeGeneration() == 1 ? 2 : 1,
                                route.authorityOwnerGeneration(),
                                route.ownerLeaseGeneration()));
                var missing =
                        assertThrows(
                                java.util.concurrent.CompletionException.class,
                                () ->
                                        target.actorClient()
                                                .sendToActor(
                                                        "missing-committed-player", new Probe(0))
                                                .submit()
                                                .toCompletableFuture()
                                                .join());
                assertEquals(
                        ZLinkFrameworkErrorKind.NOT_FOUND,
                        assertInstanceOf(ZLinkFrameworkException.class, missing.getCause()).kind());
                var admission =
                        target.actorClient()
                                .sendToActor(ACTOR, new Probe(1))
                                .submit()
                                .toCompletableFuture();
                admission.get(WAIT_SECONDS, TimeUnit.SECONDS);
                assertNotNull(preparedRaw, "Ready publication requires the raw Actor");
                assertFalse(handled.isDone(), "accepted payload must await Join completion");
                assertEquals(List.of(), RECEIVED);
                publicationRelease.complete(null);
                assertInstanceOf(
                        ZLinkActorJoinCompletion.Accepted.class,
                        joined.get(WAIT_SECONDS, TimeUnit.SECONDS));
                handled.get(WAIT_SECONDS, TimeUnit.SECONDS);
                assertNotNull(targetNode.actorLookup(ACTOR));
                target.actorClient()
                        .sendToActor(ACTOR, new Probe(2))
                        .submit()
                        .toCompletableFuture()
                        .get(WAIT_SECONDS, TimeUnit.SECONDS);
                assertFalse(
                        firstHandlerRelease.isDone(),
                        "second send admission must not wait for the first handler terminal");
                assertFalse(secondHandled.isDone());
                assertEquals(List.of(1), RECEIVED);
                firstHandlerRelease.complete(null);
                secondHandled.get(WAIT_SECONDS, TimeUnit.SECONDS);
                assertEquals(List.of(1, 2), RECEIVED);
            } finally {
                publicationRelease.complete(null);
                firstHandlerRelease.complete(null);
                if (committed.isDone() && !committed.isCompletedExceptionally()) {
                    joined.get(WAIT_SECONDS, TimeUnit.SECONDS);
                }
            }
        }
    }

    @Test
    void twoReadyActorsAcceptSendsFromFirstJoinCallbackBeforeSecondContinuation() throws Exception {
        targetFactory = null;
        joined = new CompletableFuture<>();
        handled = new CompletableFuture<>();
        firstHandlerRelease = CompletableFuture.completedFuture(null);
        secondHandled = new CompletableFuture<>();
        RECEIVED.clear();
        PAIR_COMPLETIONS.clear();
        for (String id : List.of(ACTOR, SECOND))
            PAIR_COMPLETIONS.put(id, new CompletableFuture<>());
        var locations = new ZLinkInMemoryLocationStore();
        var relocations = new InMemoryRelocationStore();
        var commits = List.of(new CompletableFuture<Void>(), new CompletableFuture<Void>());
        var releases = List.of(new CompletableFuture<Void>(), new CompletableFuture<Void>());
        var order = new java.util.concurrent.ConcurrentHashMap<Object, Integer>();
        var count = new AtomicInteger();
        Function<Object, CompletableFuture<Void>> continuation =
                key -> {
                    int index = order.computeIfAbsent(key, ignored -> count.getAndIncrement());
                    commits.get(index).complete(null);
                    return releases.get(index);
                };
        try (var source = start(options("committed-source", locations, relocations, null));
                var target =
                        start(
                                options(
                                        "committed-target",
                                        locations,
                                        relocations,
                                        source.listenerStatus(ZLinkListenerKind.ROUTE_MESH, MESH)
                                                .endpoint()))) {
            pairClient = target.actorClient();
            for (String room : List.of(ROOM, SECOND_ROOM))
                target.spotManager()
                        .getOrCreate(room, "room")
                        .submit()
                        .toCompletableFuture()
                        .get(WAIT_SECONDS, TimeUnit.SECONDS);
            for (String id : List.of(ACTOR, SECOND))
                source.actorManager()
                        .create(id, "player")
                        .submit()
                        .toCompletableFuture()
                        .get(WAIT_SECONDS, TimeUnit.SECONDS);
            holdCommitContinuation(node(source), continuation);
            holdCommitContinuation(node(target), continuation);
            try {
                for (int index = 0; index < 2; index++) {
                    String id = List.of(ACTOR, SECOND).get(index);
                    source.actorClient()
                            .requestToActor(id, new Join(index == 0 ? ROOM : SECOND_ROOM))
                            .submit(String.class)
                            .toCompletableFuture()
                            .get(WAIT_SECONDS, TimeUnit.SECONDS);
                    commits.get(index).get(WAIT_SECONDS, TimeUnit.SECONDS);
                }
                // Separate Spot lifecycle queues let both targets prepare independently.
                // Both Store records are Ready; only the first CAS continuation may progress.
                releases.get(0).complete(null);
                assertInstanceOf(
                        ZLinkActorJoinCompletion.Accepted.class,
                        PAIR_COMPLETIONS.get(ACTOR).get(WAIT_SECONDS, TimeUnit.SECONDS));
                var actors = (ZLinkActorRuntime) target.actorManager();
                var first = actors.localActor(ACTOR).orElseThrow();
                actors.awaitMoveCompletion(first)
                        .toCompletableFuture()
                        .get(WAIT_SECONDS, TimeUnit.SECONDS);
                handled.get(WAIT_SECONDS, TimeUnit.SECONDS);
                assertFalse(releases.get(1).isDone());
                assertFalse(secondHandled.isDone());
                assertNotNull(node(target).actorLookup(SECOND));
                releases.get(1).complete(null);
                assertInstanceOf(
                        ZLinkActorJoinCompletion.Accepted.class,
                        PAIR_COMPLETIONS.get(SECOND).get(WAIT_SECONDS, TimeUnit.SECONDS));
                secondHandled.get(WAIT_SECONDS, TimeUnit.SECONDS);
                var second = actors.localActor(SECOND).orElseThrow();
                actors.awaitMoveCompletion(second)
                        .toCompletableFuture()
                        .get(WAIT_SECONDS, TimeUnit.SECONDS);
                assertEquals(List.of(1, 2), RECEIVED);
            } finally {
                releases.forEach(release -> release.complete(null));
                pairClient = null;
            }
        }
    }

    @Test
    void factoryFailureDiscardsThePreparedRawActor() throws Exception {
        assertPreparationDiscarded(false);
    }

    @Test
    void casConflictDiscardsThePreparedRawActor() throws Exception {
        assertPreparationDiscarded(true);
    }

    private static void assertPreparationDiscarded(boolean conflict) throws Exception {
        sourceCompletions.set(0);
        pairClient = null;
        targetFactory = null;
        PAIR_COMPLETIONS.clear();
        joined = new CompletableFuture<>();
        var observedRaw = new CompletableFuture<ZLinkBackendActorRef>();
        var locations = new ZLinkInMemoryLocationStore();
        var relocations = new InMemoryRelocationStore();
        try (var source = start(options("committed-source", locations, relocations, null));
                var target =
                        start(
                                options(
                                        "committed-target",
                                        locations,
                                        relocations,
                                        source.listenerStatus(ZLinkListenerKind.ROUTE_MESH, MESH)
                                                .endpoint()))) {
            target.spotManager()
                    .getOrCreate(ROOM, "room")
                    .submit()
                    .toCompletableFuture()
                    .get(WAIT_SECONDS, TimeUnit.SECONDS);
            source.actorManager()
                    .create(ACTOR, "player")
                    .submit()
                    .toCompletableFuture()
                    .get(WAIT_SECONDS, TimeUnit.SECONDS);
            var targetNode = node(target);
            if (conflict) {
                BiFunction<ZLinkLocationRepository, Object[], CompletionStage<?>> abortCommit =
                        (repository, arguments) -> {
                            observedRaw.complete(targetNode.actorLookup(ACTOR));
                            return repository
                                    .abortAggregate(
                                            (ZLinkAggregateFence) arguments[0],
                                            (systems.zlink.framework.runtime.internal.locations
                                                            .ZLinkStoreCancellation)
                                                    arguments[1])
                                    .thenApply(
                                            ignored ->
                                                    systems.zlink.framework.runtime.internal
                                                            .locations.ZLinkAggregateCommitResult
                                                            .STALE);
                        };
                interceptCommit(node(source), abortCommit);
                interceptCommit(targetNode, abortCommit);
            } else {
                targetFactory =
                        context -> {
                            observedRaw.complete(targetNode.actorLookup(ACTOR));
                            return CompletableFuture.failedFuture(
                                    new IllegalStateException("injected target factory failure"));
                        };
            }
            try {
                source.actorClient()
                        .requestToActor(ACTOR, new Join(ROOM))
                        .submit(String.class)
                        .toCompletableFuture()
                        .get(WAIT_SECONDS, TimeUnit.SECONDS);
                assertNotNull(
                        observedRaw.get(WAIT_SECONDS, TimeUnit.SECONDS),
                        "preparation must own the raw Actor");
                var sourceActors = (ZLinkActorRuntime) source.actorManager();
                sourceActors
                        .awaitMoveCompletion(sourceActors.localActor(ACTOR).orElseThrow())
                        .toCompletableFuture()
                        .get(WAIT_SECONDS, TimeUnit.SECONDS);
                sourceActors
                        .awaitDrainBarrier()
                        .toCompletableFuture()
                        .get(WAIT_SECONDS, TimeUnit.SECONDS);
                // The next source handler verifies the deferred Join terminal. A CAS
                // conflict after relay-ready must restore work without a source completion
                // (membership §4); a factory failure before readiness still reports Failed.
                assertEquals(
                        "source",
                        source.actorClient()
                                .requestToActor(ACTOR, new SourceProbe())
                                .submit(String.class)
                                .toCompletableFuture()
                                .get(WAIT_SECONDS, TimeUnit.SECONDS));
                if (conflict) {
                    assertEquals(0, sourceCompletions.get());
                } else {
                    assertInstanceOf(
                            ZLinkActorJoinCompletion.Failed.class,
                            joined.get(WAIT_SECONDS, TimeUnit.SECONDS));
                    assertEquals(1, sourceCompletions.get());
                }
                assertNull(
                        targetNode.actorLookup(ACTOR), "failed target must release the raw Actor");
                var actors = (ZLinkActorRuntime) target.actorManager();
                assertTrue(actors.localActor(ACTOR).isEmpty());
                assertNotNull(
                        node(source).actorLookup(ACTOR), "source authority must be preserved");
            } finally {
                targetFactory = null;
            }
        }
    }

    private static void assertRejectedStagingRoute(
            ZLinkInternalSpotNode node, ZLinkServiceM6BWireCodec.ActorRouteFence route)
            throws Exception {
        var serializer = new ZLinkJsonMessageSerializer();
        try (var header =
                        Message.from(
                                ZLinkStreamHeaderCodec.encode(
                                        new ZLinkStreamHeader(
                                                "Probe",
                                                java.util.Map.of(),
                                                java.util.Optional.empty())));
                var payload = Message.from(serializer.serialize(new Probe(0)).bytes())) {
            var failure =
                    assertThrows(
                            java.util.concurrent.ExecutionException.class,
                            () ->
                                    node.sendToActorAsync(route, List.of(header, payload))
                                            .toCompletableFuture()
                                            .get(WAIT_SECONDS, TimeUnit.SECONDS));
            assertEquals(
                    SubmitResult.NOT_FOUND,
                    assertInstanceOf(ZlinkSubmitException.class, failure.getCause()).getResult());
        }
        assertFalse(handled.isDone());
        assertEquals(List.of(), RECEIVED);
    }

    private static void holdCommittedPublication(
            ZLinkInternalSpotNode node,
            CompletableFuture<Void> committed,
            CompletableFuture<Void> release)
            throws Exception {
        holdCommitContinuation(
                node,
                ignored -> {
                    committed.complete(null);
                    return release;
                });
    }

    private static void holdCommitContinuation(
            ZLinkInternalSpotNode node, Function<Object, CompletableFuture<Void>> continuation)
            throws Exception {
        interceptCommit(
                node,
                (repository, arguments) ->
                        repository
                                .commitAggregate(
                                        (systems.zlink.framework.runtime.internal.locations
                                                        .ZLinkAggregateFence)
                                                arguments[0],
                                        (systems.zlink.framework.runtime.internal.locations
                                                        .ZLinkStoreCancellation)
                                                arguments[1])
                                .thenCompose(
                                        value ->
                                                continuation
                                                        .apply(arguments[0])
                                                        .thenApply(ignored -> value)));
    }

    private static void interceptCommit(
            ZLinkInternalSpotNode node,
            BiFunction<ZLinkLocationRepository, Object[], CompletionStage<?>> commit)
            throws Exception {
        Object endpoint = field(node, "relocationStagingIngressHandler");
        Object coordinator = field(endpoint, "coordinator");
        var delegate = (ZLinkLocationRepository) field(coordinator, "authorityStore");
        var observed =
                Proxy.newProxyInstance(
                        ZLinkLocationRepository.class.getClassLoader(),
                        new Class<?>[] {ZLinkLocationRepository.class},
                        (proxy, method, arguments) -> {
                            if (method.getName().equals("commitAggregate"))
                                return commit.apply(delegate, arguments);
                            try {
                                return method.invoke(delegate, arguments);
                            } catch (InvocationTargetException failure) {
                                throw failure.getCause();
                            }
                        });
        Field repository = coordinator.getClass().getDeclaredField("authorityStore");
        repository.setAccessible(true);
        repository.set(coordinator, observed);
    }

    private static Object field(Object owner, String name) throws Exception {
        Field field = owner.getClass().getDeclaredField(name);
        field.setAccessible(true);
        return field.get(owner);
    }

    private static ZLinkInternalSpotNode node(ZLinkFrameworkRuntime host) throws Exception {
        return (ZLinkInternalSpotNode) field(field(host, "spots"), "primaryNode");
    }

    private static ZLinkFrameworkRuntime start(DefaultZLinkFrameworkOptions options) {
        return ZLinkFrameworkRuntimeTestAccess.start(options);
    }

    private static DefaultZLinkFrameworkOptions options(
            String rid,
            ZLinkInMemoryLocationStore locations,
            InMemoryRelocationStore relocations,
            String peerEndpoint) {
        var options = new DefaultZLinkFrameworkOptions();
        options.configureDispatch()
                .messageFlow(systems.zlink.framework.configuration.ZLinkMessageFlowLogMode.NORMAL);
        options.addLocationStore(locations);
        options.addRelocationStore(relocations);
        var node = options.addRouteMesh(MESH);
        node.listen("tcp://127.0.0.1:0").setRoutingId(RoutingId.from(rid));
        if (peerEndpoint != null) node.peerConnections().connect(peerEndpoint);
        var objects = node.objects().server();
        objects.addEntrySpot(Lobby.class);
        objects.addActorFactory(
                "player",
                Player.class,
                RoutingId.from(rid).equals(TARGET)
                        ? TargetPlayerFactory.class
                        : PlayerFactory.class,
                factory -> factory.preserveStateWith(State.class));
        if (RoutingId.from(rid).equals(TARGET)) {
            objects.addSpotFactory("room", Room.class, factory -> factory.recreateOnRelocation());
        }
        return options;
    }

    public record Join(String room) {}

    public record Probe(int value) {}

    public static final class Player implements ZLinkActor {
        private final ZLinkActorContext context;
        private final boolean target;

        public Player(ZLinkActorContext context) {
            this(context, false);
        }

        private Player(ZLinkActorContext context, boolean target) {
            this.context = context;
            this.target = target;
        }

        @Override
        public ZLinkActorContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onJoinCompleted(ZLinkActorJoinCompletion result) {
            if (!target) sourceCompletions.incrementAndGet();
            joined.complete(result);
            var pair = PAIR_COMPLETIONS.get(context.actorId());
            if (pair != null) pair.complete(result);
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class PlayerFactory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new Player(context));
        }
    }

    public static final class TargetPlayerFactory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            var factory = targetFactory;
            return factory == null
                    ? CompletableFuture.completedFuture(new Player(context, true))
                    : factory.apply(context);
        }
    }

    public static final class State implements ZLinkActorRelocationAdapter<Player> {
        @Override
        public CompletionStage<byte[]> capture(
                Player actor, ZLinkRelocationCancellation cancellation) {
            return CompletableFuture.completedFuture(new byte[0]);
        }

        @Override
        public CompletionStage<Void> restore(
                Player actor, byte[] state, ZLinkRelocationCancellation cancellation) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class Lobby implements ZLinkEntrySpot<Player> {
        private final ZLinkEntrySpotContext context;

        public Lobby(ZLinkEntrySpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkEntrySpotContext context() {
            return context;
        }

        @Override
        public void configure() {
            context.handlers().addHandler(JoinHandler.class);
            context.handlers().addHandler(SourceProbeHandler.class);
        }

        @Override
        public CompletionStage<Void> onJoinedActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class Room implements ZLinkSpot<Player> {
        private final ZLinkSpotContext context;

        public Room(ZLinkSpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override
        public void configure() {
            context.handlers().addHandler(ProbeHandler.class);
        }

        @Override
        public CompletionStage<Void> onJoinedActor(Player actor) {
            if (pairClient != null && actor.context().actorId().equals(ACTOR)) {
                return pairClient
                        .sendToActor(ACTOR, new Probe(1))
                        .submit()
                        .thenCompose(
                                ignored -> pairClient.sendToActor(SECOND, new Probe(2)).submit());
            }
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
                String actorId, ZLinkMessage request) {
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());
        }
    }

    public record SourceProbe() {}

    public static final class SourceProbeHandler
            implements ZLinkEntrySpotActorRequestHandler<Lobby, Player, SourceProbe, String> {
        @Override
        public CompletionStage<String> handle(
                Lobby spot, Player actor, ZLinkMessageContext context, SourceProbe request) {
            return CompletableFuture.completedFuture("source");
        }
    }

    public static final class JoinHandler
            implements ZLinkEntrySpotActorRequestHandler<Lobby, Player, Join, String> {
        @Override
        public CompletionStage<String> handle(
                Lobby spot, Player actor, ZLinkMessageContext context, Join request) {
            actor.context().joinSpot(request.room()).defer();
            return CompletableFuture.completedFuture("scheduled");
        }
    }

    public static final class ProbeHandler
            implements ZLinkSpotActorSendHandler<Room, Player, Probe> {
        @Override
        public CompletionStage<Void> handle(
                Room spot, Player actor, ZLinkMessageContext context, Probe probe) {
            RECEIVED.add(probe.value());
            if (probe.value() == 1) {
                handled.complete(null);
                return firstHandlerRelease;
            }
            secondHandled.complete(null);
            return CompletableFuture.completedFuture(null);
        }
    }
}
