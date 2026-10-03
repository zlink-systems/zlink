package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.*;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.*;

import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * A User Spot leaves two same-node Actors one after the other. Each {@code leaveActor} returns the
 * Actor to the Entry Spot: the Entry Spot {@code OnJoinedActor} runs before the leave completes and
 * the User Spot {@code OnLeaveActor} runs for both Actors (membership §4.1 return to Entry).
 */
final class ZLinkSpotLeaveToEntryTest {
    private static final String ROOM = "leave-room";
    private static final List<String> PLAYERS = List.of("player-1", "player-2");
    private static final List<String> EVENTS = new CopyOnWriteArrayList<>();
    private static final Map<String, Player> MEMBERS = new ConcurrentHashMap<>();
    private static volatile CompletableFuture<Void> joined;
    private static volatile CountDownLatch roomLeft;
    private static volatile boolean firstLeaveYields;
    private static volatile boolean destroyOnEntryJoin;

    /** The Bingo cleanup: the first OnLeaveActor yields and the Entry Spot destroys the Actor. */
    @Test
    void spotLeavesTwoActorsWhileFirstOnLeaveActorYields() throws Exception {
        leaveTwoActors(true, true, false);
    }

    @Test
    void spotLeavesTwoActorsToEntrySpot() throws Exception {
        leaveTwoActors(false, false, false);
    }

    /**
     * The TicTacToe leave: each Actor leaves from its own Spot handler and the Entry destroys it.
     */
    @Test
    void actorHandlersLeaveTheirSpot() throws Exception {
        leaveTwoActors(false, true, true);
    }

    private static void leaveTwoActors(
            boolean yieldFirstLeave, boolean destroyOnEntry, boolean fromActorHandler)
            throws Exception {
        firstLeaveYields = yieldFirstLeave;
        destroyOnEntryJoin = destroyOnEntry;
        EVENTS.clear();
        MEMBERS.clear();
        roomLeft = new CountDownLatch(PLAYERS.size());
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        options.configureDispatch()
                .messageFlow(systems.zlink.framework.configuration.ZLinkMessageFlowLogMode.ERRORS);
        var node = options.addRouteMesh("game");
        node.listen("inproc://spot-leave-entry-" + System.nanoTime())
                .setRoutingId(RoutingId.from("spot-leave-entry"));
        var objects = node.objects().server();
        objects.addEntrySpot(Entry.class);
        objects.addSpotFactory("room", Room.class, factory -> factory.disableRelocation());
        objects.addActorFactory(
                "player", Player.class, Factory.class, factory -> factory.disableRelocation());
        try (ZLinkFrameworkRuntime runtime = ZLinkFrameworkRuntimeTestAccess.start(options)) {
            runtime.spotManager()
                    .getOrCreate(ROOM, "room")
                    .submit()
                    .toCompletableFuture()
                    .get(3, TimeUnit.SECONDS);
            for (String actorId : PLAYERS) {
                assertInstanceOf(
                        ZLinkActorCreateResult.Created.class,
                        runtime.actorManager()
                                .create(actorId, "player")
                                .submit()
                                .toCompletableFuture()
                                .get(3, TimeUnit.SECONDS));
                joined = new CompletableFuture<>();
                assertEquals(
                        "scheduled",
                        runtime.actorClient()
                                .requestToActor(actorId, new JoinRoom())
                                .timeout(Duration.ofSeconds(3))
                                .submit(String.class)
                                .toCompletableFuture()
                                .get(3, TimeUnit.SECONDS));
                joined.get(3, TimeUnit.SECONDS);
            }
            EVENTS.clear();

            String left =
                    fromActorHandler ? leaveFromActorHandlers(runtime) : leaveFromSpot(runtime);

            assertEquals("left", left, EVENTS.toString());
            assertTrue(roomLeft.await(3, TimeUnit.SECONDS), EVENTS.toString());
            String entryEvent = destroyOnEntry ? "entry-destroyed:" : "entry-joined:";
            for (String actorId : PLAYERS) {
                int entry = EVENTS.indexOf(entryEvent + actorId);
                assertTrue(entry >= 0, EVENTS.toString());
                assertTrue(entry < EVENTS.indexOf("leave-completed:" + actorId), EVENTS.toString());
                assertTrue(EVENTS.contains("room-left:" + actorId), EVENTS.toString());
            }
            assertTrue(
                    EVENTS.indexOf("leave-completed:player-1")
                            < EVENTS.indexOf("leave-completed:player-2"),
                    EVENTS.toString());
        }
    }

    private static String leaveFromSpot(ZLinkFrameworkRuntime runtime) throws Exception {
        return runtime.route()
                .requestToSpot(ROOM, new LeaveAll())
                .timeout(Duration.ofSeconds(5))
                .submit(String.class)
                .toCompletableFuture()
                .handle((value, error) -> error == null ? value : error.toString())
                .get(10, TimeUnit.SECONDS);
    }

    private static String leaveFromActorHandlers(ZLinkFrameworkRuntime runtime) throws Exception {
        for (String actorId : PLAYERS) {
            String left =
                    runtime.actorClient()
                            .requestToActor(actorId, new LeaveSelf())
                            .timeout(Duration.ofSeconds(5))
                            .submit(String.class)
                            .toCompletableFuture()
                            .handle((value, error) -> error == null ? value : error.toString())
                            .get(10, TimeUnit.SECONDS);
            if (!left.equals("left")) {
                return left;
            }
        }
        return "left";
    }

    public record JoinRoom() {}

    public record LeaveAll() {}

    public record LeaveSelf() {}

    public static final class Player implements ZLinkActor {
        private final ZLinkActorContext context;

        public Player(ZLinkActorContext context) {
            this.context = context;
        }

        @Override
        public ZLinkActorContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onJoinCompleted(ZLinkActorJoinCompletion result) {
            if (result instanceof ZLinkActorJoinCompletion.Accepted && joined != null) {
                joined.complete(null);
            }
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class Factory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new Player(context));
        }
    }

    public static final class Entry implements ZLinkEntrySpot<Player> {
        private final ZLinkEntrySpotContext context;

        public Entry(ZLinkEntrySpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkEntrySpotContext context() {
            return context;
        }

        @Override
        public void configure() {
            context.handlers().addHandler(EntryJoinRoom.class);
        }

        @Override
        public CompletionStage<Void> onJoinedActor(Player actor) {
            String actorId = actor.context().actorId();
            EVENTS.add("entry-joined:" + actorId);
            if (!destroyOnEntryJoin) {
                return CompletableFuture.completedFuture(null);
            }
            return context.destroyActor(actor)
                    .thenRun(() -> EVENTS.add("entry-destroyed:" + actorId));
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
            context.handlers().addHandler(LeaveAllHandler.class);
            context.handlers().addHandler(LeaveSelfHandler.class);
        }

        @Override
        public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
                String actorId, ZLinkMessage request) {
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());
        }

        @Override
        public CompletionStage<Void> onJoinedActor(Player actor) {
            MEMBERS.put(actor.context().actorId(), actor);
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(Player actor) {
            String actorId = actor.context().actorId();
            if (!firstLeaveYields || !actorId.equals(PLAYERS.getFirst())) {
                return CompletableFuture.completedFuture(null).thenRun(() -> markRoomLeft(actorId));
            }
            // The first OnLeaveActor yields the Spot turn while it waits, like a sample that
            // reports the result to another service before it removes the member.
            return context.runCpuWorker(
                            cancellation -> {
                                Thread.sleep(100);
                                return null;
                            })
                    .yield()
                    .thenRun(() -> markRoomLeft(actorId));
        }

        private static void markRoomLeft(String actorId) {
            EVENTS.add("room-left:" + actorId);
            roomLeft.countDown();
        }

        CompletionStage<String> leaveAll() {
            CompletionStage<Void> leaving = CompletableFuture.completedFuture(null);
            for (String actorId : PLAYERS) {
                Player actor = MEMBERS.get(actorId);
                leaving =
                        leaving.thenCompose(ignored -> context.leaveActor(actor))
                                .thenRun(() -> EVENTS.add("leave-completed:" + actorId));
            }
            return leaving.thenApply(ignored -> "left");
        }
    }

    public static final class EntryJoinRoom
            implements ZLinkEntrySpotActorRequestHandler<Entry, Player, JoinRoom, String> {
        @Override
        public CompletionStage<String> handle(
                Entry spot, Player actor, ZLinkMessageContext context, JoinRoom request) {
            actor.context().joinSpot(ROOM, new JoinRoom()).defer();
            return CompletableFuture.completedFuture("scheduled");
        }
    }

    public static final class LeaveAllHandler
            implements ZLinkSpotRequestHandler<Room, LeaveAll, String> {
        @Override
        public CompletionStage<String> handle(Room spot, LeaveAll request) {
            return spot.leaveAll();
        }
    }

    public static final class LeaveSelfHandler
            implements ZLinkSpotActorRequestHandler<Room, Player, LeaveSelf, String> {
        @Override
        public CompletionStage<String> handle(
                Room spot, Player actor, ZLinkMessageContext context, LeaveSelf request) {
            String actorId = actor.context().actorId();
            return spot.context()
                    .leaveActor(actor)
                    .thenApply(
                            ignored -> {
                                EVENTS.add("leave-completed:" + actorId);
                                return "left";
                            });
        }
    }
}
