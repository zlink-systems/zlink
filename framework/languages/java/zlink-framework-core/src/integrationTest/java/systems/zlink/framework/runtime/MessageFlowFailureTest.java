package systems.zlink.framework.runtime;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotActorSendHandler;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotPacketHandler;

import java.util.List;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.logging.Handler;
import java.util.logging.Level;
import java.util.logging.LogRecord;
import java.util.logging.Logger;

final class MessageFlowFailureTest {
    private static final String ACTOR_ID = "message-flow-actor";
    private static final String SPOT_ID = "message-flow-spot";
    private static final int WAIT_SECONDS = 5;
    private static CompletableFuture<String> winner;
    private static CompletableFuture<Void> handlerEntered;
    private static CompletableFuture<Void> handlerResult;

    @BeforeEach
    void resetHandlers() {
        winner = new CompletableFuture<>();
        handlerEntered = new CompletableFuture<>();
        handlerResult = new CompletableFuture<>();
    }

    @Test
    void actorSendReportsEncodeFailureBeforeTransportSubmission() throws Exception {
        try (var flow = new FlowLog();
                var runtime = start()) {
            assertInstanceOf(
                    ZLinkActorCreateResult.Created.class,
                    await(runtime.actorManager().create(ACTOR_ID, "player").submit()));

            var failure =
                    assertThrows(
                            ExecutionException.class,
                            () ->
                                    await(
                                            runtime.actorClient()
                                                    .sendToActor(ACTOR_ID, new GameSnapshot(null))
                                                    .submit()));
            assertInstanceOf(IllegalArgumentException.class, failure.getCause());
            assertTrue(failure.getCause().getCause().getMessage().contains("winner"));
            assertFalse(winner.isDone());

            await(runtime.actorClient().sendToActor(ACTOR_ID, new GameSnapshot("player")).submit());
            assertEquals("player", await(winner));
            flow.await("GameSnapshot", "phase=dispatched");
            var failures = flow.failures("GameSnapshot");
            assertEquals(1, failures.size(), flow.records::toString);
            String event = failures.getFirst();
            assertTrue(
                    event.contains("outcome=failed reason=encode_error action=fail_caller"), event);
            assertTrue(event.contains(" actor=" + ACTOR_ID + " "), event);
            assertTrue(field(event, "error_message").contains("winner"), event);
            String failedFlow = field(event, "flow");
            assertNotEquals("", failedFlow, event);
            assertFalse(
                    flow.records.stream()
                            .anyMatch(
                                    line ->
                                            line.contains(" phase=sent ")
                                                    && field(line, "flow").equals(failedFlow)));
        }
    }

    @Test
    void userSpotReportsAsyncOneWayFailureOnTheReceivedFlow() throws Exception {
        try (var flow = new FlowLog();
                var runtime = start()) {
            await(runtime.spotManager().getOrCreate(SPOT_ID, "room").submit());
            try {
                await(runtime.route().sendToSpot(SPOT_ID, new CommitRoom(true)).submit());
                await(handlerEntered);
                String received = flow.await("CommitRoom", "phase=received");
                String failedFlow = field(received, "flow");
                assertNotEquals("", failedFlow, received);
                assertEquals(List.of(), flow.failures("CommitRoom"));
                handlerResult.completeExceptionally(new IllegalStateException("commit rejected"));

                await(runtime.route().sendToSpot(SPOT_ID, new CommitRoom(false)).submit());
                String completed = flow.await("CommitRoom", "phase=completed");
                assertNotEquals(failedFlow, field(completed, "flow"), completed);
                var failures = flow.failures("CommitRoom");
                assertEquals(1, failures.size(), flow.records::toString);
                String event = failures.getFirst();
                assertEquals(failedFlow, field(event, "flow"), event);
                assertEquals(field(received, "origin"), field(event, "origin"), event);
                assertTrue(
                        event.contains("outcome=failed reason=handler_exception action=drop"),
                        event);
                assertEquals("commit rejected", field(event, "error_message"), event);
            } finally {
                handlerResult.complete(null);
            }
        }
    }

    private static ZLinkFrameworkRuntime start() {
        var options = new DefaultZLinkFrameworkOptions();
        options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        var objects =
                options.addRouteMesh("game")
                        .listen("inproc://message-flow-" + System.nanoTime())
                        .setRoutingId(RoutingId.from("message-flow"))
                        .objects()
                        .server();
        objects.addEntrySpot(Entry.class);
        objects.addActorFactory(
                "player", Player.class, PlayerFactory.class, f -> f.disableRelocation());
        objects.addSpotFactory("room", Room.class, f -> f.disableRelocation());
        return RuntimeTestSupport.startFixedRidFramework(
                options, new ZLinkJavaBackendAdapterFactory());
    }

    private static <T> T await(CompletionStage<T> result) throws Exception {
        return result.toCompletableFuture().get(WAIT_SECONDS, TimeUnit.SECONDS);
    }

    private static String field(String line, String key) {
        String prefix = " " + key + "=";
        int start = line.indexOf(prefix);
        if (start < 0) return "";
        String value = line.substring(start + prefix.length());
        if (key.equals("error_message")) return value;
        int end = value.indexOf(' ');
        return end < 0 ? value : value.substring(0, end);
    }

    private static final class FlowLog extends Handler implements AutoCloseable {
        private final Logger logger = Logger.getLogger(ZLinkMessageFlowTracer.class.getName());
        private final Level previousLevel = logger.getLevel();
        private final List<String> records = new CopyOnWriteArrayList<>();
        private final BlockingQueue<String> arrivals = new LinkedBlockingQueue<>();

        FlowLog() {
            logger.addHandler(this);
            logger.setLevel(Level.ALL);
        }

        @Override
        public void publish(LogRecord record) {
            String line = record.getMessage();
            if (line.startsWith("zlink flow:")) {
                records.add(line);
                arrivals.add(line);
            }
        }

        String await(String packet, String attribute) throws InterruptedException {
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(WAIT_SECONDS);
            String record;
            while ((record = arrivals.poll(deadline - System.nanoTime(), TimeUnit.NANOSECONDS))
                    != null) {
                if (field(record, "packet").equals(packet)
                        && record.contains(" " + attribute + " ")) {
                    return record;
                }
            }
            return fail("Expected " + packet + " " + attribute + " in " + records);
        }

        List<String> failures(String packet) {
            return records.stream()
                    .filter(line -> field(line, "packet").equals(packet))
                    .filter(line -> field(line, "outcome").equals("failed"))
                    .toList();
        }

        @Override
        public void flush() {}

        @Override
        public void close() {
            logger.removeHandler(this);
            logger.setLevel(previousLevel);
        }
    }

    public record GameSnapshot(String winner) {}

    public record CommitRoom(boolean fail) {}

    public record Player(ZLinkActorContext context) implements ZLinkActor {}

    public static final class PlayerFactory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new Player(context));
        }
    }

    public record Entry(ZLinkEntrySpotContext context) implements ZLinkEntrySpot<Player> {
        @Override
        public void configure() {
            context.handlers().addHandler(SnapshotHandler.class);
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

    public static final class SnapshotHandler
            implements ZLinkEntrySpotActorSendHandler<Entry, Player, GameSnapshot> {
        @Override
        public CompletionStage<Void> handle(
                Entry spot, Player actor, ZLinkMessageContext context, GameSnapshot snapshot) {
            winner.complete(snapshot.winner());
            return CompletableFuture.completedFuture(null);
        }
    }

    public record Room(ZLinkSpotContext context) implements ZLinkSpot<Player> {
        @Override
        public void configure() {
            context.handlers().addHandler(CommitHandler.class);
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

    public static final class CommitHandler implements ZLinkSpotPacketHandler<Room, CommitRoom> {
        @Override
        public CompletionStage<Void> handle(Room spot, CommitRoom request) {
            if (!request.fail()) return CompletableFuture.completedFuture(null);
            handlerEntered.complete(null);
            return handlerResult;
        }
    }
}
