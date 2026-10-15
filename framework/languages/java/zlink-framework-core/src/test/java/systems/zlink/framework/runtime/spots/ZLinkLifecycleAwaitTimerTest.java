package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.CsvSource;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.*;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.*;

import java.time.Duration;
import java.util.UUID;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicReference;
import java.util.logging.*;

public final class ZLinkLifecycleAwaitTimerTest {
    public static Probe probe;
    public static final Duration PERIOD = Duration.ofMillis(25);
    public static final long COMPLETION_SECONDS = 5;
    public static final ZLinkTimerOptions TIMER_OPTIONS =
            new ZLinkTimerOptions(ZLinkTimerOverrunPolicy.SKIP_LATE_TICKS, 1, false);
    private static final long OBSERVATION_MILLIS = 500;

    @ParameterizedTest
    @CsvSource({"false,false", "true,false", "false,true", "true,true"})
    void joinedAwaitRetainsGateUnlessExplicitlyYielded(boolean explicitMode, boolean yield)
            throws Exception {
        runScenario(Room.class, explicitMode, yield);
    }

    public static void runScenario(
            Class<? extends ZLinkSpot<Actor>> roomType, boolean explicitMode, boolean yield)
            throws Exception {
        probe = new Probe(yield);
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
        var objects =
                options.addRouteMesh("probe")
                        .listen("inproc://lifecycle-await-" + UUID.randomUUID())
                        .objects()
                        .server();
        objects.addEntrySpot(Entry.class);
        objects.addSpotFactory(
                "room",
                roomType,
                factory -> {
                    factory.disableRelocation();
                    if (explicitMode) factory.executionMode(ZLinkUserSpotExecutionMode.SPOT_WIDE);
                });
        objects.addActorFactory(
                "actor", Actor.class, Factory.class, factory -> factory.disableRelocation());
        var logger =
                Logger.getLogger(
                        "systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer");
        var oldLevel = logger.getLevel();
        var log =
                new FileHandler(
                        "build/lifecycle-await-"
                                + roomType.getName()
                                + "-"
                                + explicitMode
                                + "-"
                                + yield
                                + ".flow");
        logger.addHandler(log);
        logger.setLevel(Level.ALL);
        try (var runtime = ZLinkFrameworkRuntimeTestAccess.start(options)) {
            runtime.spotManager()
                    .getOrCreate("room-a", "room")
                    .submit()
                    .toCompletableFuture()
                    .get(COMPLETION_SECONDS, TimeUnit.SECONDS);
            runtime.actorManager()
                    .create("actor-a", "actor")
                    .submit()
                    .toCompletableFuture()
                    .get(COMPLETION_SECONDS, TimeUnit.SECONDS);
            runtime.actorClient()
                    .requestToActor("actor-a", new Move())
                    .submit(Boolean.class)
                    .toCompletableFuture()
                    .get(COMPLETION_SECONDS, TimeUnit.SECONDS);
            assertTrue(probe.entered.await(COMPLETION_SECONDS, TimeUnit.SECONDS));
            boolean overlapped = probe.overlap.await(OBSERVATION_MILLIS, TimeUnit.MILLISECONDS);
            probe.release.countDown();
            assertTrue(probe.joined.await(COMPLETION_SECONDS, TimeUnit.SECONDS));
            assertTrue(probe.after.await(COMPLETION_SECONDS, TimeUnit.SECONDS));
            assertEquals(
                    yield,
                    overlapped,
                    "Timer admission during joined callback requires explicit Yield");
        } finally {
            probe.release.countDown();
            logger.removeHandler(log);
            logger.setLevel(oldLevel);
            log.close();
        }
    }

    public static final class Probe {
        public final boolean yield;
        public final CountDownLatch entered = new CountDownLatch(1);
        public final CountDownLatch release = new CountDownLatch(1);
        public final CountDownLatch overlap = new CountDownLatch(1);
        public final CountDownLatch joined = new CountDownLatch(1);
        public final CountDownLatch after = new CountDownLatch(1);
        public final AtomicReference<Phase> phase = new AtomicReference<>(Phase.INITIAL);

        Probe(boolean yield) {
            this.yield = yield;
        }
    }

    public enum Phase {
        INITIAL,
        WAITING,
        COMPLETED
    }

    public record Move() {}

    public static final class Actor implements ZLinkActor {
        private final ZLinkActorContext context;

        public Actor(ZLinkActorContext context) {
            this.context = context;
        }

        public ZLinkActorContext context() {
            return context;
        }
    }

    public static final class Factory implements ZLinkActorFactory {
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new Actor(context));
        }
    }

    public static final class Entry implements ZLinkEntrySpot<Actor> {
        private final ZLinkEntrySpotContext context;

        public Entry(ZLinkEntrySpotContext context) {
            this.context = context;
        }

        public ZLinkEntrySpotContext context() {
            return context;
        }

        public void configure() {
            context.handlers().addHandler(MoveHandler.class);
        }

        public CompletionStage<Void> onJoinedActor(Actor actor) {
            return CompletableFuture.completedFuture(null);
        }

        public CompletionStage<Void> onLeaveActor(Actor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class MoveHandler
            implements ZLinkEntrySpotActorRequestHandler<Entry, Actor, Move, Boolean> {
        public CompletionStage<Boolean> handle(
                Entry entry, Actor actor, ZLinkMessageContext context, Move request) {
            actor.context().joinSpot("room-a").defer();
            return CompletableFuture.completedFuture(true);
        }
    }

    public static final class Room implements ZLinkSpot<Actor> {
        private final ZLinkSpotContext context;

        public Room(ZLinkSpotContext context) {
            this.context = context;
        }

        public ZLinkSpotContext context() {
            return context;
        }

        public CompletionStage<Void> onInitialize() {
            return context.addTimer("probe", PERIOD, Tick.class, TIMER_OPTIONS)
                    .thenApply(timer -> null);
        }

        public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
                String actorId, ZLinkMessage request) {
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());
        }

        public CompletionStage<Void> onJoinedActor(Actor actor) {
            probe.phase.set(Phase.WAITING);
            var worker =
                    context.runIoWorker(
                            cancellation -> {
                                probe.entered.countDown();
                                if (!probe.release.await(COMPLETION_SECONDS, TimeUnit.SECONDS))
                                    throw new AssertionError("Worker release was not signalled");
                                return CompletableFuture.completedFuture(true);
                            });
            return (probe.yield ? worker.yield() : worker.submit())
                    .thenAccept(
                            value -> {
                                probe.phase.set(Phase.COMPLETED);
                                probe.joined.countDown();
                            });
        }

        public CompletionStage<Void> onLeaveActor(Actor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class Tick implements ZLinkSpotTimerHandler<Room> {
        public CompletionStage<Void> handle(Room spot, ZLinkTimerTick tick) {
            if (probe.phase.get() == Phase.WAITING) probe.overlap.countDown();
            if (probe.phase.get() == Phase.COMPLETED) probe.after.countDown();
            return CompletableFuture.completedFuture(null);
        }
    }
}
