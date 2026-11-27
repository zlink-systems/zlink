package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertAll;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.actors.ZLinkActorJoinCompletion;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;
import systems.zlink.framework.spots.ZLinkSpotActorRequestHandler;
import systems.zlink.framework.spots.ZLinkSpotActorSendHandler;
import systems.zlink.framework.spots.ZLinkSpotContext;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.logging.Handler;
import java.util.logging.LogRecord;
import java.util.logging.Logger;

final class ZLinkSameNodeJoinLateIngressTest {
    private static final String ROOM = "room";
    private static final String ACTOR = "player";
    private static final Duration REQUEST_DEADLINE = Duration.ofSeconds(2);
    private static final CompletableFuture<Void> DONE = CompletableFuture.completedFuture(null);

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void lateRequestRequiresSuccessfulJoinCompletion(boolean failCompletion) throws Exception {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        options.configureLocations().setOwnerLeaseFencingMargin(Duration.ofNanos(1));
        options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
        var node = options.addRouteMesh("game");
        node.listen("inproc://same-node-join-late-" + System.nanoTime())
                .setRoutingId(RoutingId.from("same-node-join-late"));
        var objects = node.objects().server();
        objects.addEntrySpot(Entry.class);
        objects.addSpotFactory("room", Room.class, factory -> factory.disableRelocation());
        objects.addActorFactory(
                "player", Player.class, Factory.class, factory -> factory.disableRelocation());

        try (var drops = new NoticeDrops();
                var runtime = ZLinkFrameworkRuntimeTestAccess.start(options)) {
            await(ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime));
            assertTrue(await(ZLinkFrameworkRuntimeTestAccess.renewOwnerLeaseOnce(runtime)));
            await(runtime.spotManager().getOrCreate(ROOM, "room").submit());
            assertInstanceOf(
                    ZLinkActorCreateResult.Created.class,
                    await(runtime.actorManager().create(ACTOR, "player").submit()));

            var actors = ZLinkFrameworkRuntimeTestAccess.actors(runtime);
            var player = assertInstanceOf(Player.class, actors.actorById(ACTOR));
            var sourceLane = await(actors.actorRelocationLaneAsync(ACTOR));
            var client = runtime.actorClient();
            assertEquals(
                    "scheduled",
                    await(
                            client.requestToActor(ACTOR, new Join(failCompletion))
                                    .timeout(REQUEST_DEADLINE)
                                    .submit(String.class)));
            assertInstanceOf(ZLinkActorJoinCompletion.Accepted.class, await(player.completion));
            // Post a lifecycle turn behind the source Join barrier to observe its settlement.
            await(sourceLane.enqueueLifecycleBarrier(() -> DONE));
            assertEquals(ROOM, player.context().spotId().orElseThrow());

            long started = System.nanoTime();
            String reply = null;
            Throwable failure = null;
            try {
                reply =
                        await(
                                client.requestToActor(ACTOR, new Probe("late"))
                                        .timeout(REQUEST_DEADLINE)
                                        .submit(String.class));
            } catch (ExecutionException error) {
                failure = error.getCause();
            }
            Duration elapsed = Duration.ofNanos(System.nanoTime() - started);
            if (failCompletion) {
                assertNotNull(
                        failure,
                        () ->
                                "Late request replied after "
                                        + elapsed
                                        + ", handler calls="
                                        + player.requests.get());
                var rejected = assertInstanceOf(ZLinkFrameworkException.class, failure);
                assertAll(
                        () ->
                                assertEquals(
                                        ZLinkFrameworkErrorKind.UNAVAILABLE,
                                        rejected.kind(),
                                        failureMessage(elapsed, player)),
                        () ->
                                assertTrue(
                                        elapsed.compareTo(Duration.ofSeconds(1)) < 0,
                                        failureMessage(elapsed, player)),
                        () -> assertEquals(0, player.requests.get()));
                await(client.sendToActor(ACTOR, new Notice()).submit());
                assertTrue(await(drops.notice).contains("reason=activation_rejected"));
                assertEquals(0, player.notices.get());
            } else {
                assertEquals("late", reply);
                assertEquals(1, player.requests.get());
            }
        }
    }

    private static String failureMessage(Duration elapsed, Player player) {
        return "elapsed=" + elapsed + ", handler calls=" + player.requests.get();
    }

    private static <T> T await(CompletionStage<T> stage) throws Exception {
        return stage.toCompletableFuture().get(4, TimeUnit.SECONDS);
    }

    private static final class NoticeDrops extends Handler implements AutoCloseable {
        private final Logger logger =
                Logger.getLogger(
                        "systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer");
        private final CompletableFuture<String> notice = new CompletableFuture<>();

        NoticeDrops() {
            logger.addHandler(this);
        }

        @Override
        public void publish(LogRecord record) {
            String message = record.getMessage();
            if (message != null
                    && message.contains("event_id=zlink.dispatch_error")
                    && message.contains("packet=Notice ")
                    && message.contains("action=drop")) {
                notice.complete(message);
            }
        }

        @Override
        public void flush() {}

        @Override
        public void close() {
            logger.removeHandler(this);
        }
    }

    public record Join(boolean failCompletion) {}

    public record Probe(String value) {}

    public record Notice() {}

    public static final class Player implements ZLinkActor {
        private final ZLinkActorContext context;
        private final CompletableFuture<ZLinkActorJoinCompletion> completion =
                new CompletableFuture<>();
        private final AtomicInteger requests = new AtomicInteger();
        private final AtomicInteger notices = new AtomicInteger();
        private boolean failCompletion;

        Player(ZLinkActorContext context) {
            this.context = context;
        }

        @Override
        public ZLinkActorContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onJoinCompleted(ZLinkActorJoinCompletion result) {
            completion.complete(result);
            return failCompletion
                    ? CompletableFuture.failedFuture(
                            new IllegalStateException("Join completion failed"))
                    : DONE;
        }
    }

    public static final class Factory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new Player(context));
        }
    }

    public record Entry(ZLinkEntrySpotContext context) implements ZLinkEntrySpot<Player> {
        @Override
        public CompletionStage<Void> onJoinedActor(Player actor) {
            return DONE;
        }

        @Override
        public CompletionStage<Void> onLeaveActor(Player actor) {
            return DONE;
        }

        @Override
        public void configure() {
            context.handlers().addHandler(JoinHandler.class);
        }
    }

    public record Room(ZLinkSpotContext context) implements ZLinkSpot<Player> {
        @Override
        public CompletionStage<Void> onJoinedActor(Player actor) {
            return DONE;
        }

        @Override
        public CompletionStage<Void> onLeaveActor(Player actor) {
            return DONE;
        }

        @Override
        public void configure() {
            context.handlers().addHandler(ProbeHandler.class);
            context.handlers().addHandler(NoticeHandler.class);
        }

        @Override
        public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
                String actorId, ZLinkMessage request) {
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());
        }
    }

    public static final class JoinHandler
            implements ZLinkEntrySpotActorRequestHandler<Entry, Player, Join, String> {
        @Override
        public CompletionStage<String> handle(
                Entry spot, Player actor, ZLinkMessageContext context, Join request) {
            actor.failCompletion = request.failCompletion();
            actor.context().joinSpot(ROOM).defer();
            return CompletableFuture.completedFuture("scheduled");
        }
    }

    public static final class ProbeHandler
            implements ZLinkSpotActorRequestHandler<Room, Player, Probe, String> {
        @Override
        public CompletionStage<String> handle(
                Room spot, Player actor, ZLinkMessageContext context, Probe request) {
            actor.requests.incrementAndGet();
            return CompletableFuture.completedFuture(request.value());
        }
    }

    public static final class NoticeHandler
            implements ZLinkSpotActorSendHandler<Room, Player, Notice> {
        @Override
        public CompletionStage<Void> handle(
                Room spot, Player actor, ZLinkMessageContext context, Notice notice) {
            actor.notices.incrementAndGet();
            return DONE;
        }
    }
}
