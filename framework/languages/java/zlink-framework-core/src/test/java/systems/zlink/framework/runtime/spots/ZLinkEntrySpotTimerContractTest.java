package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.ZLinkFrameworkRegistrationTestAccess;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotSubscriptionHandler;
import systems.zlink.framework.spots.ZLinkSpotTimerHandler;
import systems.zlink.framework.spots.ZLinkTimerTick;

import java.time.Duration;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkEntrySpotTimerContractTest {
    private static final String MESH = "entry-timer";
    private static final String CHANNEL = "events";
    private static final String TOPIC = "increment";
    private static final Duration PERIOD = Duration.ofSeconds(1);

    @Test
    void firstRegistrationRunsTheCallback() throws Exception {
        var probe = new Probe();
        try (var scheduler = new ZLinkRelocatedTimerRebindTest.ManualScheduler();
                var serial = new ZLinkRelocatedTimerLifecycleTest.DirectExecutor();
                var runtime = start(probe, scheduler, serial)) {
            var entry = await(probe.entry);
            await(entry.addTimer("pulse", PERIOD, CurrentTimer.class, null));

            runNextTick(scheduler);

            assertEquals(List.of("current"), List.copyOf(probe.callbacks));
        }
    }

    @Test
    void registeringTheSameNameReplacesThePreviousCallback() throws Exception {
        var probe = new Probe();
        try (var scheduler = new ZLinkRelocatedTimerRebindTest.ManualScheduler();
                var serial = new ZLinkRelocatedTimerLifecycleTest.DirectExecutor();
                var runtime = start(probe, scheduler, serial)) {
            var entry = await(probe.entry);
            await(entry.addTimer("pulse", PERIOD, PreviousTimer.class, null));
            await(entry.addTimer("pulse", PERIOD, CurrentTimer.class, null));

            // Advance both originally scheduled ticks only after the replacement is installed.
            runNextTick(scheduler);
            runNextTick(scheduler);

            assertEquals(List.of("current"), List.copyOf(probe.callbacks));
        }
    }

    @Test
    void timerWaitsForTheEntrySpotHandlerToComplete() throws Exception {
        var probe = new Probe();
        try (var scheduler = new ZLinkRelocatedTimerRebindTest.ManualScheduler();
                var serial = new ZLinkRelocatedTimerLifecycleTest.DirectExecutor();
                var runtime = start(probe, scheduler, serial)) {
            var entry = await(probe.entry);
            await(entry.addTimer("pulse", PERIOD, CounterTimer.class, null));
            boolean ranWhileHandlerWasPending;
            try {
                await(entry.outbound().publish(CHANNEL, TOPIC, new Increment(1)).submit());
                await(probe.handlerEntered);
                // With the inline executor, eligible timer work runs before this tick returns.
                runNextTick(scheduler);
                ranWhileHandlerWasPending = probe.timerRan.isDone();
            } finally {
                probe.releaseHandler.complete(null);
            }

            await(probe.timerRan);
            assertFalse(
                    ranWhileHandlerWasPending, "timer ran before the Entry Spot handler completed");
            assertEquals(2, probe.counter.get());
        }
    }

    private static void runNextTick(ZLinkRelocatedTimerRebindTest.ManualScheduler scheduler)
            throws Exception {
        var work = scheduler.due.poll(5, TimeUnit.SECONDS);
        assertNotNull(work, "no scheduled Spot timer tick");
        work.run();
    }

    private static <T> T await(CompletionStage<T> stage) throws Exception {
        return stage.toCompletableFuture().get(5, TimeUnit.SECONDS);
    }

    private static ZLinkFrameworkRuntime start(
            Probe probe,
            ZLinkRelocatedTimerRebindTest.ManualScheduler scheduler,
            ZLinkRelocatedTimerLifecycleTest.DirectExecutor serial) {
        var options = ZLinkFrameworkRegistrationTestAccess.optionsWithSerialExecutor(serial);
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        var mesh = options.addRouteMesh(MESH).listen("inproc://" + MESH + "-" + UUID.randomUUID());
        mesh.channelName(CHANNEL).server();
        mesh.objects().server().addEntrySpot(Entry.class);
        return ZLinkFrameworkRuntimeTestAccess.startHostWithConstruction(
                options,
                new ZLinkJavaBackendAdapterFactory(),
                ZLinkHandlerActivator.services().add(Probe.class, probe),
                null,
                new ZLinkFrameworkRuntimeTestAccess.Construction()
                        .spotTimerExecutor(scheduler)
                        .spotHandlerExecutor(serial)
                        .spotTimerStateLaneFactory(() -> new ZLinkStateLane(serial)));
    }

    public static final class Probe {
        private final CompletableFuture<ZLinkEntrySpotContext> entry = new CompletableFuture<>();
        private final ConcurrentLinkedQueue<String> callbacks = new ConcurrentLinkedQueue<>();
        private final CompletableFuture<Void> handlerEntered = new CompletableFuture<>();
        private final CompletableFuture<Void> releaseHandler = new CompletableFuture<>();
        private final CompletableFuture<Void> timerRan = new CompletableFuture<>();
        private final AtomicInteger counter = new AtomicInteger();
    }

    public record Entry(ZLinkEntrySpotContext context, Probe probe)
            implements ZLinkEntrySpot<ZLinkActor> {
        @Override
        public void configure() {
            context.handlers().addSubscribe(IncrementHandler.class, CHANNEL, TOPIC);
            probe.entry.complete(context);
        }

        @Override
        public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public record Increment(int amount) {}

    public static final class IncrementHandler
            implements ZLinkSpotSubscriptionHandler<Entry, Increment> {
        @Override
        public CompletionStage<Void> handle(Entry spot, Increment request) {
            var probe = spot.probe;
            int updated = probe.counter.get() + request.amount();
            var terminal = probe.releaseHandler.thenRun(() -> probe.counter.set(updated));
            probe.handlerEntered.complete(null);
            return terminal;
        }
    }

    public record PreviousTimer(Probe probe) implements ZLinkSpotTimerHandler<ZLinkSpot<?>> {
        @Override
        public CompletionStage<Void> handle(ZLinkSpot<?> spot, ZLinkTimerTick tick) {
            probe.callbacks.add("previous");
            return CompletableFuture.completedFuture(null);
        }
    }

    public record CurrentTimer(Probe probe) implements ZLinkSpotTimerHandler<ZLinkSpot<?>> {
        @Override
        public CompletionStage<Void> handle(ZLinkSpot<?> spot, ZLinkTimerTick tick) {
            probe.callbacks.add("current");
            return CompletableFuture.completedFuture(null);
        }
    }

    public record CounterTimer(Probe probe) implements ZLinkSpotTimerHandler<ZLinkSpot<?>> {
        @Override
        public CompletionStage<Void> handle(ZLinkSpot<?> spot, ZLinkTimerTick tick) {
            probe.counter.incrementAndGet();
            probe.timerRan.complete(null);
            return CompletableFuture.completedFuture(null);
        }
    }
}
