package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.*;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.monitoring.*;
import systems.zlink.framework.runtime.InMemoryRelocationStore;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.*;

import java.time.Duration;
import java.util.concurrent.*;
import java.util.function.Predicate;

/** Public inputs for the source observation and current membership contracts (#1740). */
final class ZLinkRelocationObservationPublicTest {
    private static final String MESH = "observation";
    private static final Duration WAIT = Duration.ofSeconds(15);
    private static CompletableFuture<ZLinkActorJoinCompletion> joined;

    @Test
    void existingObserverReceivesSourceQuiescenceAfterRelocated() throws Exception {
        var locations = new ZLinkInMemoryLocationStore();
        var relocations = new InMemoryRelocationStore();
        try (var source =
                ZLinkFrameworkRuntimeTestAccess.start(
                        options("source", locations, relocations, null))) {
            source.spotManager()
                    .getOrCreate("room", "room")
                    .submit()
                    .toCompletableFuture()
                    .get(WAIT.toSeconds(), TimeUnit.SECONDS);
            var endpoint = source.listenerStatus(ZLinkListenerKind.ROUTE_MESH, MESH).endpoint();
            try (var target =
                            ZLinkFrameworkRuntimeTestAccess.start(
                                    options("target", locations, relocations, endpoint));
                    var readySource =
                            observe(
                                    source.routeMeshRuntime().observe(MESH, 8),
                                    s -> s.isReady() && s.readyPeerCount() > 0);
                    var readyTarget =
                            observe(
                                    target.routeMeshRuntime().observe(MESH, 8),
                                    s -> s.isReady() && s.readyPeerCount() > 0);
                    var unsafe =
                            observe(
                                    source.observe(),
                                    s ->
                                            s.state() == ZLinkFrameworkRuntimeState.RELOCATED
                                                    && !s.safeToShutdown());
                    var safe =
                            observe(
                                    source.observe(),
                                    s ->
                                            s.state() == ZLinkFrameworkRuntimeState.RELOCATED
                                                    && s.safeToShutdown())) {
                readySource.result.get(WAIT.toSeconds(), TimeUnit.SECONDS);
                readyTarget.result.get(WAIT.toSeconds(), TimeUnit.SECONDS);
                var result =
                        source.relocate(
                                        new ZLinkFrameworkRelocationOptions(
                                                ZLinkFrameworkRelocationMode.PLANNED_MAINTENANCE,
                                                null,
                                                WAIT))
                                .toCompletableFuture()
                                .get(WAIT.toSeconds(), TimeUnit.SECONDS);
                assertEquals(ZLinkFrameworkRelocationOutcome.RELOCATED, result.outcome());
                var before = unsafe.result.get(WAIT.toSeconds(), TimeUnit.SECONDS);
                assertFalse(
                        source.status().safeToShutdown(),
                        "Message Follow still retains source obligations");
                ZLinkFrameworkRuntimeStatus after;
                try {
                    after = safe.result.get(WAIT.toSeconds(), TimeUnit.SECONDS);
                } catch (TimeoutException failure) {
                    assertTrue(source.status().safeToShutdown(), "source quiescence is queryable");
                    throw new AssertionError(
                            "existing observer did not receive Relocated/safeToShutdown=true",
                            failure);
                }
                assertTrue(
                        after.sequence() > before.sequence(),
                        "quiescence publishes a new Sequence");
                var queried = source.status();
                assertTrue(queried.safeToShutdown());
                assertTrue(queried.sequence() >= after.sequence());
                assertEquals(
                        queried, source.status(), "unchanged queries reuse the published status");
            }
        }
    }

    @Test
    void findSpotReturnsTheCreatedSpotIncarnationAfterJoin() throws Exception {
        joined = new CompletableFuture<>();
        try (var runtime =
                ZLinkFrameworkRuntimeTestAccess.start(
                        options(
                                "membership",
                                new ZLinkInMemoryLocationStore(),
                                new InMemoryRelocationStore(),
                                null))) {
            var first =
                    runtime.spotManager()
                            .getOrCreate("room", "room")
                            .submit()
                            .toCompletableFuture()
                            .get(WAIT.toSeconds(), TimeUnit.SECONDS)
                            .spot();
            assertTrue(
                    runtime.spotManager()
                            .close(first)
                            .toCompletableFuture()
                            .get(WAIT.toSeconds(), TimeUnit.SECONDS));
            var current =
                    runtime.spotManager()
                            .getOrCreate("room", "room")
                            .submit()
                            .toCompletableFuture()
                            .get(WAIT.toSeconds(), TimeUnit.SECONDS)
                            .spot();
            assertNotEquals(first.objectGeneration(), current.objectGeneration());
            var actor =
                    assertInstanceOf(
                                    ZLinkActorCreateResult.Created.class,
                                    runtime.actorManager()
                                            .create("player", "player")
                                            .submit()
                                            .toCompletableFuture()
                                            .get(WAIT.toSeconds(), TimeUnit.SECONDS))
                            .actor();
            try {
                assertEquals(
                        "scheduled",
                        runtime.actorClient()
                                .requestToActor("player", new Join("room"))
                                .timeout(WAIT)
                                .submit(String.class)
                                .toCompletableFuture()
                                .get(WAIT.toSeconds(), TimeUnit.SECONDS));
                assertInstanceOf(
                        ZLinkActorJoinCompletion.Accepted.class,
                        joined.get(WAIT.toSeconds(), TimeUnit.SECONDS));
                assertEquals(
                        current,
                        runtime.actorManager()
                                .findSpot("player")
                                .toCompletableFuture()
                                .get(WAIT.toSeconds(), TimeUnit.SECONDS)
                                .orElseThrow());
            } finally {
                joined = new CompletableFuture<>();
                runtime.actorClient()
                        .requestToActor("player", new Leave())
                        .timeout(WAIT)
                        .submit(String.class)
                        .toCompletableFuture()
                        .get(WAIT.toSeconds(), TimeUnit.SECONDS);
                assertInstanceOf(
                        ZLinkActorJoinCompletion.Accepted.class,
                        joined.get(WAIT.toSeconds(), TimeUnit.SECONDS));
                runtime.actorManager()
                        .destroy(actor)
                        .toCompletableFuture()
                        .get(WAIT.toSeconds(), TimeUnit.SECONDS);
            }
        }
    }

    private static DefaultZLinkFrameworkOptions options(
            String rid,
            ZLinkInMemoryLocationStore locations,
            InMemoryRelocationStore relocations,
            String peer) {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(locations);
        options.addRelocationStore(relocations);
        options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
        options.configureLocations().setRouteCacheMaxAge(Duration.ZERO);
        options.configureLocations().setMessageFollowDuration(Duration.ofSeconds(5));
        var node =
                options.addRouteMesh(MESH)
                        .listen("tcp://127.0.0.1:0")
                        .setRoutingId(RoutingId.from(rid));
        if (peer != null) node.peerConnections().connect(peer);
        var objects = node.objects().server();
        objects.addEntrySpot(Lobby.class);
        objects.addActorFactory(
                "player",
                Player.class,
                PlayerFactory.class,
                f -> f.preserveStateWith(PlayerState.class));
        objects.addSpotFactory("room", Room.class, f -> f.recreateOnRelocation());
        return options;
    }

    private static <T> Observation<T> observe(
            Flow.Publisher<ZLinkObservedStatus<T>> publisher, Predicate<T> predicate) {
        var observation = new Observation<T>(predicate);
        publisher.subscribe(observation);
        return observation;
    }

    private static final class Observation<T>
            implements Flow.Subscriber<ZLinkObservedStatus<T>>, AutoCloseable {
        final CompletableFuture<T> result = new CompletableFuture<>();
        private final Predicate<T> predicate;
        private Flow.Subscription subscription;

        Observation(Predicate<T> predicate) {
            this.predicate = predicate;
        }

        @Override
        public void onSubscribe(Flow.Subscription subscription) {
            this.subscription = subscription;
            subscription.request(Long.MAX_VALUE);
        }

        @Override
        public void onNext(ZLinkObservedStatus<T> observed) {
            if (predicate.test(observed.status())) result.complete(observed.status());
        }

        @Override
        public void onError(Throwable failure) {
            result.completeExceptionally(failure);
        }

        @Override
        public void onComplete() {}

        @Override
        public void close() {
            subscription.cancel();
        }
    }

    public record Join(String spotId) {}

    public record Leave() {}

    public record Player(ZLinkActorContext context) implements ZLinkActor {
        @Override
        public CompletionStage<Void> onJoinCompleted(ZLinkActorJoinCompletion completion) {
            joined.complete(completion);
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class PlayerFactory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new Player(context));
        }
    }

    public static final class PlayerState implements ZLinkActorRelocationAdapter<Player> {
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

    public record Lobby(ZLinkEntrySpotContext context) implements ZLinkEntrySpot<Player> {
        @Override
        public CompletionStage<Void> onJoinedActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public void configure() {
            context.handlers().addHandler(JoinHandler.class);
        }
    }

    public record Room(ZLinkSpotContext context) implements ZLinkSpot<Player> {
        @Override
        public CompletionStage<Void> onJoinedActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public void configure() {
            context.handlers().addHandler(LeaveHandler.class);
        }

        @Override
        public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
                String actorId, ZLinkMessage request) {
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());
        }
    }

    public static final class JoinHandler
            implements ZLinkEntrySpotActorRequestHandler<Lobby, Player, Join, String> {
        @Override
        public CompletionStage<String> handle(
                Lobby spot, Player actor, ZLinkMessageContext context, Join request) {
            actor.context().joinSpot(request.spotId()).defer();
            return CompletableFuture.completedFuture("scheduled");
        }
    }

    public static final class LeaveHandler
            implements ZLinkSpotActorRequestHandler<Room, Player, Leave, String> {
        @Override
        public CompletionStage<String> handle(
                Room spot, Player actor, ZLinkMessageContext context, Leave request) {
            actor.context().joinEntrySpot().defer();
            return CompletableFuture.completedFuture("scheduled");
        }
    }
}
