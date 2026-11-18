package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.*;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.*;

import java.time.Duration;
import java.util.UUID;
import java.util.concurrent.*;

final class ZLinkSpotCloseInLeave4432Test {
    @org.junit.jupiter.api.io.TempDir(cleanup = org.junit.jupiter.api.io.CleanupMode.ON_SUCCESS)
    java.nio.file.Path flowDirectory;

    static String phase;
    static boolean awaitClose;
    static CompletableFuture<Void> joined;
    static final java.util.Map<String, Room> rooms = new java.util.concurrent.ConcurrentHashMap<>();
    static int checks;
    static CompletableFuture<Void> checked;

    @ParameterizedTest
    @ValueSource(
            strings = {
                "join-submit",
                "join-await",
                "joined-submit",
                "joined-await",
                "leave-submit",
                "leave-await"
            })
    void closeInsideLifecycleIsRejectedAtInvocation(String scenario) throws Exception {
        phase = scenario.substring(0, scenario.indexOf('-'));
        awaitClose = scenario.endsWith("await");
        checks = 0;
        checked = new CompletableFuture<>();
        var logger =
                java.util.logging.Logger.getLogger(
                        "systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer");
        var previousLevel = logger.getLevel();
        var flowPath = flowDirectory.resolve(scenario + ".flow");
        var flow = new java.util.logging.FileHandler(flowPath.toString());
        flow.setFormatter(new java.util.logging.SimpleFormatter());
        flow.setLevel(java.util.logging.Level.ALL);
        logger.addHandler(flow);
        logger.setLevel(java.util.logging.Level.ALL);
        System.out.println("Message flow file: " + flowPath);
        var options = new DefaultZLinkFrameworkOptions();
        options.configureLocations().setOwnerLeaseFencingMargin(Duration.ofNanos(1));
        options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.DETAILED);
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        var objects =
                options.addRouteMesh("play")
                        .listen("inproc://close4432-" + UUID.randomUUID())
                        .objects()
                        .server();
        objects.addEntrySpot(Entry.class);
        objects.addActorFactory(
                "player", Player.class, PlayerFactory.class, f -> f.disableRelocation());
        objects.addSpotFactory("room", Room.class, f -> f.disableRelocation());
        var runtime =
                ZLinkFrameworkRuntimeTestAccess.start(
                        options, new ZLinkJavaBackendAdapterFactory());
        try {
            ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime)
                    .toCompletableFuture()
                    .get(5, TimeUnit.SECONDS);
            assertTrue(
                    ZLinkFrameworkRuntimeTestAccess.renewOwnerLeaseOnce(runtime)
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS));
            var source =
                    runtime.spotManager()
                            .create("room")
                            .submit()
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS)
                            .spot();
            var target =
                    runtime.spotManager()
                            .create("room")
                            .submit()
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS)
                            .spot();
            var other =
                    runtime.spotManager()
                            .create("room")
                            .submit()
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS)
                            .spot();
            var room = rooms.get(source.spotId());
            room.checkedPhase = phase;
            room.other = rooms.get(other.spotId()).context;
            var actor =
                    assertInstanceOf(
                                    ZLinkActorCreateResult.Created.class,
                                    runtime.actorManager()
                                            .create("player-4432", "player")
                                            .submit()
                                            .toCompletableFuture()
                                            .get(5, TimeUnit.SECONDS))
                            .actor();
            for (var spot : new SpotRef[] {source, target}) {
                joined = new CompletableFuture<>();
                assertEquals(
                        "scheduled",
                        runtime.actorClient()
                                .requestToActor(actor.actorId(), new Move(spot.spotId()))
                                .submit(String.class)
                                .toCompletableFuture()
                                .get(5, TimeUnit.SECONDS));
                joined.get(5, TimeUnit.SECONDS);
            }
            checked.get(5, TimeUnit.SECONDS);
            assertEquals(1, checks);
            assertTrue(
                    runtime.spotManager()
                            .close(source)
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS));
        } finally {
            try {
                runtime.close();
            } finally {
                flow.close();
                logger.removeHandler(flow);
                logger.setLevel(previousLevel);
                rooms.clear();
            }
        }
    }

    public record Move(String spotId) {}

    public static final class Player implements ZLinkActor {
        final ZLinkActorContext context;

        public Player(ZLinkActorContext context) {
            this.context = context;
        }

        public ZLinkActorContext context() {
            return context;
        }

        public CompletionStage<Void> onJoinCompleted(ZLinkActorJoinCompletion result) {
            if (result instanceof ZLinkActorJoinCompletion.Accepted) joined.complete(null);
            else joined.completeExceptionally(new AssertionError(result));
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class PlayerFactory implements ZLinkActorFactory {
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new Player(context));
        }
    }

    public static final class Entry implements ZLinkEntrySpot<Player> {
        final ZLinkEntrySpotContext context;

        public Entry(ZLinkEntrySpotContext context) {
            this.context = context;
        }

        public ZLinkEntrySpotContext context() {
            return context;
        }

        public void configure() {
            context.handlers().addHandler(EntryMove.class);
        }

        public CompletionStage<Void> onJoinedActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        public CompletionStage<Void> onLeaveActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class Room implements ZLinkSpot<Player> {
        final ZLinkSpotContext context;

        public Room(ZLinkSpotContext context) {
            this.context = context;
            rooms.put(context.spotId(), this);
        }

        public ZLinkSpotContext context() {
            return context;
        }

        public void configure() {
            context.handlers().addHandler(RoomMove.class);
        }

        public CompletionStage<Void> onInitialize() {
            var failure =
                    assertThrows(
                            systems.zlink.framework.errors.ZLinkFrameworkException.class,
                            () -> context.close());
            assertEquals(
                    systems.zlink.framework.errors.ZLinkFrameworkErrorKind.INVALID_OPERATION,
                    failure.kind());
            return CompletableFuture.completedFuture(null);
        }

        public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
                String actorId, ZLinkMessage request) {
            return checkClose("join").thenApply(ignored -> ZLinkSpotActorJoinResult.accept());
        }

        String checkedPhase;
        ZLinkSpotContext other;

        public CompletionStage<Void> onJoinedActor(Player actor) {
            return checkClose("joined");
        }

        public CompletionStage<Void> onLeaveActor(Player actor) {
            return checkClose("leave");
        }

        private CompletionStage<Void> checkClose(String currentPhase) {
            if (!currentPhase.equals(checkedPhase)) return CompletableFuture.completedFuture(null);
            var failure =
                    assertThrows(
                            systems.zlink.framework.errors.ZLinkFrameworkException.class,
                            () -> context.close());
            assertEquals(
                    systems.zlink.framework.errors.ZLinkFrameworkErrorKind.INVALID_OPERATION,
                    failure.kind());
            if (awaitClose) {
                assertThrows(
                        systems.zlink.framework.errors.ZLinkFrameworkException.class,
                        () -> context.close().toCompletableFuture().join());
            }
            return other.close()
                    .thenAccept(
                            value -> {
                                assertTrue(value);
                                checks++;
                                checked.complete(null);
                            })
                    .whenComplete(
                            (ignored, error) -> {
                                if (error != null) checked.completeExceptionally(error);
                            });
        }
    }

    public static final class EntryMove
            implements ZLinkEntrySpotActorRequestHandler<Entry, Player, Move, String> {
        public CompletionStage<String> handle(
                Entry spot, Player actor, ZLinkMessageContext context, Move request) {
            actor.context().joinSpot(request.spotId()).defer();
            return CompletableFuture.completedFuture("scheduled");
        }
    }

    public static final class RoomMove
            implements ZLinkSpotActorRequestHandler<Room, Player, Move, String> {
        public CompletionStage<String> handle(
                Room spot, Player actor, ZLinkMessageContext context, Move request) {
            actor.context().joinSpot(request.spotId()).defer();
            return CompletableFuture.completedFuture("scheduled");
        }
    }
}
