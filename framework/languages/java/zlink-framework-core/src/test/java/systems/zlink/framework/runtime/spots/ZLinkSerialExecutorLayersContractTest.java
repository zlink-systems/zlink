package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assertions.fail;

import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.execution.ZLinkExecutionLanePolicy;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.streams
    .ZLinkSessionSerialExecutorContractProbe;

final class ZLinkSerialExecutorLayersContractTest {
    private static final Duration TEST_TIMEOUT = Duration.ofSeconds(10);

    @Test
    void submissionEntrypointsSelectTheirQueuesWithoutAQueueArgument()
        throws Exception {
        try (Fixture fixture = fixture(ZLinkUserSpotExecutionMode.PER_ACTOR)) {
            List<String> executed = new CopyOnWriteArrayList<>();
            CompletableFuture.allOf(
                fixture.serial.executeSpot(0, () -> completed(executed, "spot"))
                    .toCompletableFuture(),
                fixture.serial.executeActor(
                        "actor-a", 0, () -> completed(executed, "actor"))
                    .toCompletableFuture(),
                fixture.serial.executeTimer(
                        "tick", ignored -> completed(executed, "timer"))
                    .toCompletableFuture(),
                fixture.serial.executeLifecycle(
                        () -> completed(executed, "lifecycle"))
                    .toCompletableFuture())
                .get(3, TimeUnit.SECONDS);

            assertEquals(
                List.of("actor", "lifecycle", "spot", "timer"),
                executed.stream().sorted().toList());
            assertEquals(
                List.of("application", "control", "final", "infrastructure"),
                ZLinkSessionSerialExecutorContractProbe
                    .executeEverySubmissionPath(fixture.executor)
                    .stream().sorted().toList());
        }
    }

    @Test
    void perActorRunsTwoActorsAtTheSameTime() throws Exception {
        try (Fixture fixture = fixture(ZLinkUserSpotExecutionMode.PER_ACTOR)) {
            CompletableFuture<Void> release = new CompletableFuture<>();
            CompletableFuture<Void> actorAStarted = new CompletableFuture<>();
            CompletableFuture<Void> actorBStarted = new CompletableFuture<>();

            CompletionStage<Void> actorA = fixture.serial.executeActor(
                "actor-a", () -> {
                    actorAStarted.complete(null);
                    return release;
                });
            CompletionStage<Void> actorB = fixture.serial.executeActor(
                "actor-b", () -> {
                    actorBStarted.complete(null);
                    return release;
                });

            CompletableFuture.allOf(actorAStarted, actorBStarted)
                .get(3, TimeUnit.SECONDS);
            assertFalse(actorA.toCompletableFuture().isDone());
            assertFalse(actorB.toCompletableFuture().isDone());
            release.complete(null);
            await(actorA, actorB);
        }
    }

    @Test
    void theSameActorKeepsFifoInBothExecutionModes() throws Exception {
        for (ZLinkUserSpotExecutionMode mode : List.of(
                ZLinkUserSpotExecutionMode.PER_ACTOR,
                ZLinkUserSpotExecutionMode.SPOT_WIDE)) {
            try (Fixture fixture = fixture(mode)) {
                List<String> order = new CopyOnWriteArrayList<>();
                CompletableFuture<Void> release = new CompletableFuture<>();
                CompletableFuture<Void> firstStarted = new CompletableFuture<>();
                AtomicBoolean secondStarted = new AtomicBoolean();

                CompletionStage<Void> first = fixture.serial.executeActor(
                    "actor-a", () -> {
                        order.add("first:start");
                        firstStarted.complete(null);
                        return release.thenRun(() -> order.add("first:end"));
                    });
                firstStarted.get(3, TimeUnit.SECONDS);
                CompletionStage<Void> second = fixture.serial.executeActor(
                    "actor-a", () -> {
                        secondStarted.set(true);
                        order.add("second");
                        return CompletableFuture.completedFuture(null);
                    });

                assertFalse(secondStarted.get(), mode.name());
                release.complete(null);
                await(first, second);
                assertEquals(
                    List.of("first:start", "first:end", "second"),
                    order,
                    mode.name());
            }
        }
    }

    @Test
    void perActorTimerNamesOverlapAndEachNameKeepsFifo() throws Exception {
        try (Fixture fixture = fixture(ZLinkUserSpotExecutionMode.PER_ACTOR)) {
            List<String> order = new CopyOnWriteArrayList<>();
            CompletableFuture<Void> release = new CompletableFuture<>();
            CompletableFuture<Void> tickStarted = new CompletableFuture<>();
            CompletableFuture<Void> beatStarted = new CompletableFuture<>();
            AtomicBoolean secondTickStarted = new AtomicBoolean();

            CompletionStage<Void> firstTick = fixture.serial.executeTimer(
                "tick", ignored -> {
                    order.add("tick:first:start");
                    tickStarted.complete(null);
                    return release.thenRun(() -> order.add("tick:first:end"));
                });
            tickStarted.get(3, TimeUnit.SECONDS);
            CompletionStage<Void> secondTick = fixture.serial.executeTimer(
                "tick", ignored -> {
                    secondTickStarted.set(true);
                    order.add("tick:second");
                    return CompletableFuture.completedFuture(null);
                });
            CompletionStage<Void> beat = fixture.serial.executeTimer(
                "beat", ignored -> {
                    order.add("beat");
                    beatStarted.complete(null);
                    return CompletableFuture.completedFuture(null);
                });

            beatStarted.get(3, TimeUnit.SECONDS);
            assertFalse(secondTickStarted.get());
            release.complete(null);
            await(firstTick, secondTick, beat);
            assertEquals(
                List.of(
                    "tick:first:start", "beat", "tick:first:end", "tick:second"),
                order);
        }
    }

    @Test
    void actorMailboxCapacityRejectsOnlyTheFullActorInBothModes()
        throws Exception {
        for (ZLinkUserSpotExecutionMode mode : List.of(
                ZLinkUserSpotExecutionMode.PER_ACTOR,
                ZLinkUserSpotExecutionMode.SPOT_WIDE)) {
            try (Fixture fixture = fixture(mode)) {
                CompletableFuture<Void> release = new CompletableFuture<>();
                CompletableFuture<Void> started = new CompletableFuture<>();
                List<CompletionStage<Void>> accepted = new ArrayList<>();
                accepted.add(fixture.serial.executeActor("actor-full", () -> {
                    started.complete(null);
                    return release;
                }));
                started.get(3, TimeUnit.SECONDS);
                for (int index = 1;
                     index < ZLinkSerialExecutionQueue
                         .DEFAULT_APPLICATION_MESSAGE_CAPACITY;
                     index++) {
                    CompletionStage<Void> pending = fixture.serial.executeActor(
                        "actor-full", () -> CompletableFuture.completedFuture(null));
                    assertFalse(
                        pending.toCompletableFuture().isCompletedExceptionally(),
                        mode.name() + " accepted index " + index);
                    accepted.add(pending);
                }

                assertCapacityExceeded(fixture.serial.executeActor(
                    "actor-full", () -> CompletableFuture.completedFuture(null)));
                CompletionStage<Void> otherActor = fixture.serial.executeActor(
                    "actor-open", () -> CompletableFuture.completedFuture(null));
                assertFalse(
                    otherActor.toCompletableFuture().isCompletedExceptionally(),
                    mode.name());

                release.complete(null);
                accepted.add(otherActor);
                CompletableFuture.allOf(accepted.stream()
                        .map(CompletionStage::toCompletableFuture)
                        .toArray(CompletableFuture[]::new))
                    .get(TEST_TIMEOUT.toSeconds(), TimeUnit.SECONDS);
            }
        }
    }

    @Test
    void spotWideLargeActorPayloadIsRejectedBeforeASmallPayload()
        throws Exception {
        try (Fixture fixture = fixture(ZLinkUserSpotExecutionMode.SPOT_WIDE)) {
            long largePayload = 40L * 1024 * 1024;
            CompletableFuture<Void> release = new CompletableFuture<>();
            CompletableFuture<Void> started = new CompletableFuture<>();
            CompletionStage<Void> first = fixture.serial.executeActor(
                "actor-a", largePayload, () -> {
                    started.complete(null);
                    return release;
                });
            started.get(3, TimeUnit.SECONDS);

            assertCapacityExceeded(fixture.serial.executeActor(
                "actor-a",
                largePayload,
                () -> CompletableFuture.completedFuture(null)));
            CompletionStage<Void> small = fixture.serial.executeActor(
                "actor-a", 1, () -> CompletableFuture.completedFuture(null));
            assertFalse(small.toCompletableFuture().isCompletedExceptionally());

            release.complete(null);
            await(first, small);
        }
    }

    @Test
    void spotWideUpperQueueFillsByCountForSmallAndLargePayloads()
        throws Exception {
        for (long payloadBytes : List.of(1L, 10_000_000L)) {
            try (Fixture fixture = fixture(
                    ZLinkUserSpotExecutionMode.SPOT_WIDE,
                    1,
                    1,
                    1,
                    2,
                    Duration.ofSeconds(1))) {
                CompletableFuture<Void> release = new CompletableFuture<>();
                CompletableFuture<Void> started = new CompletableFuture<>();
                CompletionStage<Void> first = fixture.serial.executeActor(
                    "actor-a", 1, () -> {
                        started.complete(null);
                        return release;
                    });
                started.get(3, TimeUnit.SECONDS);

                assertCapacityExceeded(fixture.serial.executeActor(
                    "actor-b",
                    payloadBytes,
                    () -> CompletableFuture.completedFuture(null)));
                release.complete(null);
                first.toCompletableFuture().get(3, TimeUnit.SECONDS);
            }
        }
    }

    @Test
    void ownerTimeBudgetCutsAnOverloadedOwnerBeforeItsLastRecord()
        throws Exception {
        ExecutorService executor = Executors.newSingleThreadExecutor();
        ZLinkSerialExecutionQueue overloaded = queue(
            executor, 32, 4096, 1, 1024, 1, 8, Duration.ofMillis(1));
        ZLinkSerialExecutionQueue other = queue(
            executor, 32, 4096, 1, 1024, 1, 8, Duration.ofMillis(1));
        try {
            List<String> order = new CopyOnWriteArrayList<>();
            CompletableFuture<Void> firstStarted = new CompletableFuture<>();
            CompletableFuture<Void> releaseFirst = new CompletableFuture<>();
            List<CompletionStage<Void>> turns = new ArrayList<>();
            turns.add(overloaded.enqueue(() -> {
                order.add("overloaded:0");
                firstStarted.complete(null);
                return releaseFirst.thenRun(() -> blockFor(Duration.ofMillis(2)));
            }));
            for (int index = 1; index < 8; index++) {
                int captured = index;
                turns.add(overloaded.enqueue(() -> {
                    order.add("overloaded:" + captured);
                    blockFor(Duration.ofMillis(2));
                    return CompletableFuture.completedFuture(null);
                }));
            }
            firstStarted.get(3, TimeUnit.SECONDS);
            turns.add(other.enqueue(() -> {
                order.add("other");
                return CompletableFuture.completedFuture(null);
            }));

            releaseFirst.complete(null);
            CompletableFuture.allOf(turns.stream()
                    .map(CompletionStage::toCompletableFuture)
                    .toArray(CompletableFuture[]::new))
                .get(3, TimeUnit.SECONDS);
            assertTrue(
                order.indexOf("other") < order.indexOf("overloaded:7"),
                order.toString());
        } finally {
            overloaded.close();
            other.close();
            executor.shutdownNow();
        }
    }

    @Test
    void spotWideYieldRetainsTheActorClaimWhileOtherHandlersProgress()
        throws Exception {
        try (Fixture fixture = fixture(ZLinkUserSpotExecutionMode.SPOT_WIDE)) {
            List<String> events = new CopyOnWriteArrayList<>();
            CompletableFuture<Void> remote = new CompletableFuture<>();
            CompletableFuture<Void> firstStarted = new CompletableFuture<>();
            CompletableFuture<Void> otherActorRan = new CompletableFuture<>();
            CompletableFuture<Void> spotRan = new CompletableFuture<>();
            CompletableFuture<Void> timerRan = new CompletableFuture<>();
            AtomicBoolean sameActorNextRan = new AtomicBoolean();

            CompletionStage<Void> first = fixture.serial.executeActor(
                "actor-a", () -> {
                    events.add("actor-a:start");
                    firstStarted.complete(null);
                    return ZLinkSerialExecutionQueue.yieldCurrent(remote)
                        .thenRun(() -> events.add("actor-a:resume"));
                });
            firstStarted.get(3, TimeUnit.SECONDS);
            CompletionStage<Void> sameActorNext = fixture.serial.executeActor(
                "actor-a", () -> {
                    sameActorNextRan.set(true);
                    events.add("actor-a:next");
                    return CompletableFuture.completedFuture(null);
                });
            CompletionStage<Void> otherActor = fixture.serial.executeActor(
                "actor-b", () -> {
                    events.add("actor-b");
                    otherActorRan.complete(null);
                    return CompletableFuture.completedFuture(null);
                });
            CompletionStage<Void> spot = fixture.serial.executeSpot(0, () -> {
                events.add("spot");
                spotRan.complete(null);
                return CompletableFuture.completedFuture(null);
            });
            CompletionStage<Void> timer = fixture.serial.executeTimer(
                "tick", ignored -> {
                    events.add("timer");
                    timerRan.complete(null);
                    return CompletableFuture.completedFuture(null);
                });

            CompletableFuture.allOf(otherActorRan, spotRan, timerRan)
                .get(3, TimeUnit.SECONDS);
            assertFalse(sameActorNextRan.get(), events.toString());

            remote.complete(null);
            await(first, sameActorNext, otherActor, spot, timer);
            assertTrue(
                events.indexOf("actor-a:resume")
                    < events.indexOf("actor-a:next"),
                events.toString());
        }
    }

    private static Fixture fixture(ZLinkUserSpotExecutionMode mode) {
        ExecutorService executor = Executors.newVirtualThreadPerTaskExecutor();
        return new Fixture(
            executor,
            new ZLinkSpotSerialExecutor(
                new ZLinkSerialExecutionQueue(
                    executor, ZLinkExecutionLanePolicy.spot()),
                new ZLinkSerialExecutionQueue(
                    executor, ZLinkExecutionLanePolicy.spot()),
                executor,
                mode,
                false));
    }

    private static Fixture fixture(
        ZLinkUserSpotExecutionMode mode,
        int applicationMessageCapacity,
        long applicationByteCapacity,
        long fixedWorkByteCost,
        int lifecycleBurstLimit,
        Duration ownerTimeBudget) {
        ExecutorService executor = Executors.newVirtualThreadPerTaskExecutor();
        ZLinkSerialExecutionQueue spotQueue = queue(
            executor,
            applicationMessageCapacity,
            applicationByteCapacity,
            8,
            1024,
            fixedWorkByteCost,
            lifecycleBurstLimit,
            ownerTimeBudget);
        return new Fixture(
            executor,
            new ZLinkSpotSerialExecutor(
                spotQueue,
                new ZLinkSerialExecutionQueue(
                    executor, ZLinkExecutionLanePolicy.spot()),
                executor,
                mode,
                false));
    }

    private static ZLinkSerialExecutionQueue queue(
        ExecutorService executor,
        int applicationMessageCapacity,
        long applicationByteCapacity,
        int lifecycleMessageCapacity,
        long lifecycleByteCapacity,
        long fixedWorkByteCost,
        int lifecycleBurstLimit,
        Duration ownerTimeBudget) {
        return new ZLinkSerialExecutionQueue(
            executor,
            ZLinkExecutionLanePolicy.spot(),
            applicationMessageCapacity,
            applicationByteCapacity,
            lifecycleMessageCapacity,
            lifecycleByteCapacity,
            fixedWorkByteCost,
            lifecycleBurstLimit,
            ownerTimeBudget);
    }

    private static CompletableFuture<Void> completed(
        List<String> executed,
        String entrypoint) {
        executed.add(entrypoint);
        return CompletableFuture.completedFuture(null);
    }

    @SafeVarargs
    private static void await(CompletionStage<Void>... stages) throws Exception {
        CompletableFuture.allOf(java.util.Arrays.stream(stages)
                .map(CompletionStage::toCompletableFuture)
                .toArray(CompletableFuture[]::new))
            .get(3, TimeUnit.SECONDS);
    }

    private static void assertCapacityExceeded(CompletionStage<Void> rejected)
        throws Exception {
        try {
            rejected.toCompletableFuture().get(3, TimeUnit.SECONDS);
            fail("submission was accepted");
        } catch (ExecutionException failure) {
            ZLinkFrameworkException capacity = assertInstanceOf(
                ZLinkFrameworkException.class,
                failure.getCause());
            assertEquals(ZLinkFrameworkErrorKind.CAPACITY_EXCEEDED, capacity.kind());
        }
    }

    private static void blockFor(Duration duration) {
        long deadline = System.nanoTime() + duration.toNanos();
        while (System.nanoTime() < deadline) {
            Thread.onSpinWait();
        }
    }

    private record Fixture(
        ExecutorService executor,
        ZLinkSpotSerialExecutor serial) implements AutoCloseable {
        @Override
        public void close() {
            serial.close();
            executor.shutdownNow();
        }
    }
}
