package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.spots.ZLinkTimer;
import systems.zlink.framework.spots.ZLinkTimerOptions;
import systems.zlink.framework.spots.ZLinkTimerOverrunPolicy;
import systems.zlink.framework.spots.ZLinkTimerTick;

import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Delayed;
import java.util.concurrent.FutureTask;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;

final class ZLinkRelocatedTimerRebindTest {
    private static final String RESTORED_KEY = "restored";
    private static final Duration PERIOD = Duration.ofDays(1);
    private static final Instant STARTED_AT = Instant.parse("2026-10-16T00:00:00Z");
    private static final Instant NEXT_AT = Instant.parse("2026-10-20T00:00:00Z");
    private static final ZLinkTimerOptions OPTIONS =
            new ZLinkTimerOptions(ZLinkTimerOverrunPolicy.SKIP_LATE_TICKS, 1, false);

    @Test
    void initializationReattachesRestoredCursorAndHandleWithDifferentPeriodAndOptions() {
        for (boolean pending : List.of(false, true)) {
            try (var scheduler = new ManualScheduler();
                    var registry = registry(scheduler)) {
                var snapshot = snapshot(pending);
                registry.stageRestore(snapshot);
                ZLinkTimer first = initializeAdd(registry, RESTORED_KEY, TickHandler.class);
                ZLinkTimer second = initializeAdd(registry, RESTORED_KEY, TickHandler.class);
                assertSame(first, second);
                assertFalse(first.isDisposed());
                assertEquals(snapshot, registry.freeze());
                assertTrue(scheduler.tasks.isEmpty(), "no scheduling before Ready");
            }
        }
    }

    @Test
    void pendingTickRunsExactlyOnceWithRestoredIndexesAfterPublication() {
        try (var scheduler = new ManualScheduler()) {
            var deliveries = new ArrayList<ZLinkTimerTick>();
            var dispatched = new CompletableFuture<Void>();
            try (var registry =
                    new ZLinkSpotTimerRegistry(
                            "target",
                            scheduler,
                            ignored -> new TickHandler(deliveries),
                            List.of(),
                            null,
                            "test",
                            (name, callback) -> {
                                var result = callback.get();
                                result.whenComplete(
                                        (ignored, failure) -> {
                                            if (failure == null) dispatched.complete(null);
                                            else dispatched.completeExceptionally(failure);
                                        });
                                return result;
                            })) {
                registry.setSpot(new Object());
                var snapshot = snapshot(true);
                registry.stageRestore(snapshot);
                initializeAdd(registry, RESTORED_KEY, TickHandler.class);
                assertTrue(deliveries.isEmpty());
                registry.publishStagedRestore();
                dispatched.join();
                assertEquals(
                        List.of(snapshot.timers().getFirst().pendingTick().orElseThrow().tick()),
                        deliveries);
                assertTrue(registry.hasActiveTimers());
            }
        }
    }

    @Test
    void handlerTypeMismatchFailsInitializationWithoutReplacingRestoredTimer() {
        try (var scheduler = new ManualScheduler();
                var registry = registry(scheduler)) {
            var snapshot = snapshot(false);
            registry.stageRestore(snapshot);
            var failed =
                    assertThrows(
                            CompletionException.class,
                            () -> initializeAdd(registry, RESTORED_KEY, OtherHandler.class));
            assertInstanceOf(ZLinkConfigurationException.class, failed.getCause());
            assertEquals(snapshot, registry.freeze());
        }
    }

    @Test
    void invalidArgumentsStillFailBeforeRestoredReattachment() {
        try (var scheduler = new ManualScheduler();
                var registry = registry(scheduler)) {
            registry.stageRestore(snapshot(false));
            assertThrows(
                    CompletionException.class,
                    () ->
                            registry.runRelocationInitialization(
                                            () ->
                                                    registry.add(
                                                            RESTORED_KEY,
                                                            Duration.ZERO,
                                                            TickHandler.class,
                                                            OPTIONS))
                                    .toCompletableFuture()
                                    .join());
            assertThrows(
                    CompletionException.class,
                    () ->
                            registry.runRelocationInitialization(
                                            () ->
                                                    registry.add(
                                                            RESTORED_KEY,
                                                            PERIOD,
                                                            TickHandler.class,
                                                            new ZLinkTimerOptions(
                                                                    ZLinkTimerOverrunPolicy
                                                                            .CATCH_UP_BOUNDED,
                                                                    0,
                                                                    false)))
                                    .toCompletableFuture()
                                    .join());
        }
    }

    @Test
    void newKeyRetainsGenerationReplacementAndWaitsForReady() {
        try (var scheduler = new ManualScheduler();
                var registry = registry(scheduler)) {
            registry.stageRestore(snapshot(false));
            var first = initializeAdd(registry, "new", TickHandler.class);
            var second = initializeAdd(registry, "new", OtherHandler.class);
            assertNotSame(first, second);
            assertTrue(registry.hasActiveTimers());
            assertTrue(first.isDisposed());
            assertFalse(second.isDisposed());
            assertTrue(scheduler.tasks.isEmpty());
            registry.publishStagedRestore();
            assertEquals(2, scheduler.tasks.size());
        }
    }

    @Test
    void otherCallbackBeforeReadyUsesOrdinaryGenerationRule() {
        try (var scheduler = new ManualScheduler();
                var registry = registry(scheduler)) {
            registry.stageRestore(snapshot(false));
            var restored = initializeAdd(registry, RESTORED_KEY, TickHandler.class);
            var replaced =
                    registry.add(RESTORED_KEY, PERIOD, OtherHandler.class, OPTIONS)
                            .toCompletableFuture()
                            .join();
            assertNotSame(restored, replaced);
            assertTrue(registry.hasActiveTimers());
            assertTrue(restored.isDisposed());
            assertTrue(scheduler.tasks.isEmpty());
            registry.publishStagedRestore();
            assertEquals(1, scheduler.tasks.size());
        }
    }

    @Test
    void readyEndsReattachmentAndQueuedOldGenerationCannotRun() {
        try (var scheduler = new ManualScheduler()) {
            var deliveries = new ArrayList<ZLinkTimerTick>();
            var queued = new ArrayList<java.util.function.Supplier<CompletionStage<Void>>>();
            try (var registry =
                    new ZLinkSpotTimerRegistry(
                            "target",
                            scheduler,
                            ignored -> new TickHandler(deliveries),
                            List.of(),
                            null,
                            "test",
                            (name, callback) -> {
                                queued.add(callback);
                                return CompletableFuture.completedFuture(null);
                            })) {
                registry.stageRestore(snapshot(true));
                var restored = initializeAdd(registry, RESTORED_KEY, TickHandler.class);
                registry.publishStagedRestore();
                assertEquals(1, queued.size());
                var replacement =
                        registry.add(RESTORED_KEY, PERIOD, TickHandler.class, OPTIONS)
                                .toCompletableFuture()
                                .join();
                assertNotSame(restored, replacement);
                assertTrue(restored.isDisposed());
                queued.getFirst().get().toCompletableFuture().join();
                assertTrue(deliveries.isEmpty(), "old generation must not invoke handler");
                var replaced = registry.freeze().timers().getFirst();
                assertEquals(0, replaced.schedule().deliveryIndex());
                assertEquals(0, replaced.schedule().lastScheduledIndex());
                assertTrue(replaced.pendingTick().isEmpty());
            }
        }
    }

    @Test
    void asynchronousInitializationKeepsReattachmentUntilItsTerminal() {
        try (var scheduler = new ManualScheduler();
                var registry = registry(scheduler)) {
            registry.stageRestore(snapshot(false));
            var release = new CompletableFuture<Void>();
            var initialized = registry.runRelocationInitialization(() -> release);
            var first =
                    registry.add(RESTORED_KEY, PERIOD, TickHandler.class, OPTIONS)
                            .toCompletableFuture()
                            .join();
            var second =
                    registry.add(RESTORED_KEY, PERIOD, TickHandler.class, OPTIONS)
                            .toCompletableFuture()
                            .join();
            assertSame(first, second);
            release.complete(null);
            initialized.toCompletableFuture().join();
            var replacement =
                    registry.add(RESTORED_KEY, PERIOD, TickHandler.class, OPTIONS)
                            .toCompletableFuture()
                            .join();
            assertNotSame(first, replacement);
        }
    }

    @Test
    void sourceFreezeContinuesToRejectRegistration() {
        try (var scheduler = new ManualScheduler();
                var registry = registry(scheduler)) {
            registry.freeze();
            assertThrows(
                    CompletionException.class,
                    () ->
                            registry.add("new", PERIOD, TickHandler.class, OPTIONS)
                                    .toCompletableFuture()
                                    .join());
        }
    }

    private static ZLinkTimer initializeAdd(
            ZLinkSpotTimerRegistry registry, String key, Class<?> type) {
        return registry.runRelocationInitialization(
                        () ->
                                registry.add(
                                        key,
                                        Duration.ofSeconds(2),
                                        type,
                                        new ZLinkTimerOptions(
                                                ZLinkTimerOverrunPolicy.DELAY_NEXT_TICK, 1, true)))
                .toCompletableFuture()
                .join();
    }

    private static ZLinkSpotTimerRegistry registry(ManualScheduler scheduler) {
        return registry(scheduler, new ArrayList<>());
    }

    private static ZLinkSpotTimerRegistry registry(
            ManualScheduler scheduler, List<ZLinkTimerTick> deliveries) {
        var registry =
                new ZLinkSpotTimerRegistry(
                        "target",
                        scheduler,
                        type ->
                                type == TickHandler.class
                                        ? new TickHandler(deliveries)
                                        : new OtherHandler(),
                        List.of(),
                        null,
                        "test",
                        (name, callback) -> callback.get());
        registry.setSpot(new Object());
        return registry;
    }

    static ZLinkSpotTimerRegistry.FrozenTimers snapshot(boolean pending) {
        var state =
                new ZLinkSpotTimerSchedule.State(RESTORED_KEY, PERIOD, OPTIONS, STARTED_AT, 7, 11);
        var tick =
                new ZLinkTimerTick(
                        RESTORED_KEY,
                        7,
                        12,
                        PERIOD,
                        NEXT_AT,
                        NEXT_AT,
                        PERIOD.multipliedBy(12),
                        PERIOD.multipliedBy(12),
                        Duration.ZERO,
                        0);
        return new ZLinkSpotTimerRegistry.FrozenTimers(
                List.of(
                        new ZLinkSpotTimerRegistry.TimerSnapshot(
                                RESTORED_KEY,
                                TickHandler.class,
                                state,
                                pending ? Optional.empty() : Optional.of(NEXT_AT),
                                pending
                                        ? Optional.of(
                                                new ZLinkSpotTimerSchedule.PendingTick(12, tick))
                                        : Optional.empty())));
    }

    public static final class TickHandler {
        private final List<ZLinkTimerTick> deliveries;

        public TickHandler() {
            this(new ArrayList<>());
        }

        TickHandler(List<ZLinkTimerTick> deliveries) {
            this.deliveries = deliveries;
        }

        public CompletionStage<Void> handle(Object spot, ZLinkTimerTick tick) {
            deliveries.add(tick);
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class OtherHandler {
        public CompletionStage<Void> handle(Object spot, ZLinkTimerTick tick) {
            return CompletableFuture.completedFuture(null);
        }
    }

    private static final class ManualScheduler extends ScheduledThreadPoolExecutor {
        private final List<ScheduledFuture<?>> tasks = new ArrayList<>();

        ManualScheduler() {
            super(1);
        }

        @Override
        public ScheduledFuture<?> schedule(Runnable command, long delay, TimeUnit unit) {
            var task = new ManualFuture(command);
            tasks.add(task);
            return task;
        }
    }

    private static final class ManualFuture extends FutureTask<Void>
            implements ScheduledFuture<Void> {
        ManualFuture(Runnable command) {
            super(command, null);
        }

        @Override
        public long getDelay(TimeUnit unit) {
            return 0;
        }

        @Override
        public int compareTo(Delayed other) {
            return 0;
        }
    }
}
