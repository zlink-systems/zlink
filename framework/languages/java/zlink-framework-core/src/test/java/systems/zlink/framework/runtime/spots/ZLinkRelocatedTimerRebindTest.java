package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.spots.ZLinkTimer;
import systems.zlink.framework.spots.ZLinkTimerOptions;
import systems.zlink.framework.spots.ZLinkTimerOverrunPolicy;
import systems.zlink.framework.spots.ZLinkTimerTick;

import java.nio.file.Files;
import java.nio.file.Path;
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
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.function.Supplier;

final class ZLinkRelocatedTimerRebindTest {
    private static final String RESTORED_KEY = "restored";
    private static final Duration PERIOD = Duration.ofDays(1);
    private static final Instant STARTED_AT = Instant.now();
    private static final Instant NEXT_AT = STARTED_AT.plus(PERIOD.multipliedBy(12));
    private static final ZLinkTimerOptions OPTIONS =
            new ZLinkTimerOptions(ZLinkTimerOverrunPolicy.SKIP_LATE_TICKS, 1, false);

    @Test
    void selectedPendingTickEncodesTheGoldenCompletedCursor() throws Exception {
        var fixture = goldenTimer();
        var pending = goldenPending();
        var period = Duration.ofMillis(fixture.path("periodMilliseconds").asLong());
        var scheduledAt =
                Instant.ofEpochMilli(pending.path("scheduledAtUnixMilliseconds").asLong());
        var schedule =
                new ZLinkSpotTimerSchedule(
                        new ZLinkSpotTimerSchedule.State(
                                fixture.path("name").asText(),
                                period,
                                OPTIONS,
                                scheduledAt.minus(
                                        period.multipliedBy(
                                                pending.path("scheduledIndex").asLong())),
                                fixture.path("lastCompletedDeliveryIndex").asLong(),
                                fixture.path("lastCompletedScheduledIndex").asLong()));
        var selected =
                schedule.nextTick(
                        period.multipliedBy(pending.path("scheduledIndex").asLong()).toNanos(),
                        scheduledAt);
        var frozen =
                new ZLinkSpotTimerRegistry.FrozenTimers(
                        List.of(
                                new ZLinkSpotTimerRegistry.TimerSnapshot(
                                        fixture.path("name").asText(),
                                        TickHandler.class,
                                        schedule.snapshot(),
                                        Optional.empty(),
                                        Optional.of(selected))));
        var canonical =
                ZLinkSpotTimerRelocationEnvelope.canonicalize(
                                ZLinkSpotTimerRelocationEnvelope.encode(frozen))
                        .getFirst();

        assertEquals(
                fixture.path("lastCompletedDeliveryIndex").asLong(),
                canonical.lastCompletedDeliveryIndex());
        assertEquals(
                fixture.path("lastCompletedScheduledIndex").asLong(),
                canonical.lastCompletedScheduledIndex());
        assertEquals(pending.path("deliveryIndex").asLong(), canonical.pending().deliveryIndex());
        assertEquals(pending.path("scheduledIndex").asLong(), canonical.pending().scheduledIndex());
    }

    @Test
    void goldenPendingDecodeAdvancesTheCompletedCursorBeforeTheFollowingTick() throws Exception {
        var fixture = goldenTimer();
        var pending = goldenPending();
        var canonical =
                new ZLinkSpotTimerRelocationEnvelope.CanonicalTimer(
                        fixture.path("name").asText(),
                        TickHandler.class.getName(),
                        fixture.path("periodMilliseconds").asLong(),
                        1,
                        fixture.path("maxCatchUpTicks").asLong(),
                        fixture.path("stopOnUnhandledException").asBoolean(),
                        fixture.path("lastCompletedDeliveryIndex").asLong(),
                        fixture.path("lastCompletedScheduledIndex").asLong(),
                        fixture.path("nextScheduledAtUnixMilliseconds").asLong(),
                        new ZLinkSpotTimerRelocationEnvelope.CanonicalPending(
                                pending.path("deliveryIndex").asLong(),
                                        pending.path("scheduledIndex").asLong(),
                                pending.path("scheduledAtUnixMilliseconds").asLong(),
                                        pending.path("skippedTicks").asLong()));
        var encoded = ZLinkSpotTimerRelocationEnvelope.encodeCanonical(List.of(canonical));
        assertEquals(List.of(canonical), ZLinkSpotTimerRelocationEnvelope.canonicalize(encoded));
        var restored =
                ZLinkSpotTimerRelocationEnvelope.decode(encoded, ignored -> TickHandler.class)
                        .timers()
                        .getFirst();
        var schedule = new ZLinkSpotTimerSchedule(restored.schedule());
        schedule.markDelivered(restored.pendingTick().orElseThrow());
        var next = schedule.nextTick(0, Instant.EPOCH).tick();

        assertEquals(pending.path("deliveryIndex").asLong() + 1, next.deliveryIndex());
        assertEquals(pending.path("scheduledIndex").asLong() + 1, next.scheduledIndex());
        assertEquals(0, next.skippedTicks());
    }

    @Test
    void relocatedHandlerSeesTheSamePendingAndFollowingIndexesAsSourceContinuation()
            throws Exception {
        for (var policy : ZLinkTimerOverrunPolicy.values()) {
            var period = Duration.ofDays(1);
            var options = new ZLinkTimerOptions(policy, 3, false);
            var schedule =
                    new ZLinkSpotTimerSchedule(
                            new ZLinkSpotTimerSchedule.State(
                                    RESTORED_KEY, period, options, Instant.now(), 4, 4));
            var selected = schedule.nextTick(period.multipliedBy(9).toNanos(), Instant.now());
            var sourceState =
                    new ZLinkSpotTimerRegistry.FrozenTimers(
                            List.of(
                                    new ZLinkSpotTimerRegistry.TimerSnapshot(
                                            RESTORED_KEY,
                                            TickHandler.class,
                                            schedule.snapshot(),
                                            Optional.empty(),
                                            Optional.of(selected))));
            var canonical =
                    ZLinkSpotTimerRelocationEnvelope.canonicalize(
                            ZLinkSpotTimerRelocationEnvelope.encode(sourceState));
            var movedState =
                    ZLinkSpotTimerRelocationEnvelope.decode(
                            ZLinkSpotTimerRelocationEnvelope.encodeCanonical(canonical),
                            ignored -> TickHandler.class);
            var source = deliveredIndexes(sourceState);
            var moved = deliveredIndexes(movedState);
            assertEquals(
                    source.stream()
                            .map(
                                    tick ->
                                            List.of(
                                                    tick.deliveryIndex(),
                                                    tick.scheduledIndex(),
                                                    tick.skippedTicks()))
                            .toList(),
                    moved.stream()
                            .map(
                                    tick ->
                                            List.of(
                                                    tick.deliveryIndex(),
                                                    tick.scheduledIndex(),
                                                    tick.skippedTicks()))
                            .toList(),
                    policy.name());
            assertEquals(
                    List.of(5L, 6L),
                    moved.stream().map(ZLinkTimerTick::deliveryIndex).toList(),
                    policy.name());
            assertEquals(selected.tick().scheduledIndex(), moved.getFirst().scheduledIndex());
            assertEquals(selected.tick().skippedTicks(), moved.getFirst().skippedTicks());
            assertEquals(moved.getFirst().scheduledIndex() + 1, moved.getLast().scheduledIndex());
            assertEquals(0, moved.getLast().skippedTicks());
        }
    }

    private static List<ZLinkTimerTick> deliveredIndexes(ZLinkSpotTimerRegistry.FrozenTimers state)
            throws InterruptedException {
        var deliveries = new ArrayList<ZLinkTimerTick>();
        var callbacks = new LinkedBlockingQueue<Supplier<CompletionStage<Void>>>();
        try (var scheduler = new ManualScheduler();
                var registry =
                        new ZLinkSpotTimerRegistry(
                                "continuation",
                                scheduler,
                                ignored -> new TickHandler(deliveries),
                                List.of(),
                                "test",
                                (name, callback) -> {
                                    callbacks.add(callback);
                                    return CompletableFuture.completedFuture(null);
                                })) {
            registry.setSpot(new Object());
            registry.stageRestore(state);
            registry.publishStagedRestore();
            for (int index = 0; index < 2; index++) {
                if (index != 0 || state.timers().getFirst().pendingTick().isEmpty()) {
                    var due = scheduler.due.poll(3, TimeUnit.SECONDS);
                    assertNotNull(due, "next schedule must be published after completion");
                    due.run();
                }
                var callback = callbacks.poll(3, TimeUnit.SECONDS);
                assertNotNull(callback, "selected tick must reach dispatch");
                var result = callback.get().toCompletableFuture();
                result.join();
            }
        }
        return deliveries;
    }

    private static JsonNode goldenTimer() throws Exception {
        return goldenFixture().path("decoded").path("timerRegistrations").get(0);
    }

    private static JsonNode goldenPending() throws Exception {
        return goldenFixture().path("decoded").path("pendingTimerTicks").get(0);
    }

    private static JsonNode goldenFixture() throws Exception {
        var current = Path.of(System.getProperty("user.dir")).toAbsolutePath();
        while (current != null) {
            var path = current.resolve("runtime/protocol/golden/relocation-envelope-v1.json");
            if (Files.isRegularFile(path))
                return new ObjectMapper().readTree(Files.readString(path));
            current = current.getParent();
        }
        throw new IllegalStateException("shared relocation envelope golden fixture was not found");
    }

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
    void handlerTypeMismatchFailsOnlyRegistrationWithoutReplacingRestoredTimer() {
        try (var scheduler = new ManualScheduler();
                var registry = registry(scheduler)) {
            var snapshot = snapshot(false);
            registry.stageRestore(snapshot);
            var failed =
                    assertThrows(
                            CompletionException.class,
                            () -> initializeAdd(registry, RESTORED_KEY, OtherHandler.class));
            assertEquals(
                    ZLinkFrameworkErrorKind.INVALID_OPERATION,
                    assertInstanceOf(ZLinkFrameworkException.class, failed.getCause()).kind());
            assertEquals(snapshot, registry.freeze());
        }
    }

    @Test
    void ignoredMismatchDoesNotPoisonInitializationOrChangeTheRestoredHandle() {
        try (var scheduler = new ManualScheduler();
                var registry = registry(scheduler)) {
            var snapshot = snapshot(false);
            registry.stageRestore(snapshot);
            ZLinkTimer restored = initializeAdd(registry, RESTORED_KEY, TickHandler.class);
            var rejected = new CompletableFuture<ZLinkTimer>();
            registry.runRelocationInitialization(
                            () -> {
                                registry.add(RESTORED_KEY, PERIOD, OtherHandler.class, OPTIONS)
                                        .whenComplete(
                                                (timer, failure) -> {
                                                    if (failure == null) rejected.complete(timer);
                                                    else rejected.completeExceptionally(failure);
                                                })
                                        .handle((timer, failure) -> null)
                                        .toCompletableFuture()
                                        .join();
                                return CompletableFuture.completedFuture(null);
                            })
                    .toCompletableFuture()
                    .join();
            var failure = assertThrows(CompletionException.class, rejected::join);
            assertEquals(
                    ZLinkFrameworkErrorKind.INVALID_OPERATION,
                    assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
            assertSame(restored, initializeAdd(registry, RESTORED_KEY, TickHandler.class));
            assertFalse(restored.isDisposed());
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
                        8,
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

    static final class ManualScheduler extends ScheduledThreadPoolExecutor {
        private final List<ScheduledFuture<?>> tasks = new ArrayList<>();
        final LinkedBlockingQueue<Runnable> due = new LinkedBlockingQueue<>();

        ManualScheduler() {
            super(1);
        }

        @Override
        public ScheduledFuture<?> schedule(Runnable command, long delay, TimeUnit unit) {
            var task = new ManualFuture(command);
            tasks.add(task);
            due.add(task);
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
