package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.CsvSource;

import systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode;
import systems.zlink.framework.execution.ZLinkExecutionLanePolicy;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.internal.relocation.ZLinkRetainedSerialQueueCommit;

import java.lang.reflect.Field;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.Executors;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkSpotWideConsumerOwnershipTest {
    @Test
    void externalRetainedCutReturnsWhileSharedGateIsOccupied() throws Exception {
        var spot = new ZLinkSerialExecutionQueue(Runnable::run, ZLinkExecutionLanePolicy.spot());
        var serials =
                new ZLinkSpotSerialExecutor(
                        spot,
                        Runnable::run,
                        Runnable::run,
                        ZLinkUserSpotExecutionMode.SPOT_WIDE,
                        false);
        var actor = serials.claimActorQueue("actor").toCompletableFuture().get(5, TimeUnit.SECONDS);
        var mailbox = actor.relocationLane();
        var seal =
                mailbox.trySealRelocation()
                        .toCompletableFuture()
                        .get(5, TimeUnit.SECONDS)
                        .orElseThrow();
        var retained =
                ZLinkRetainedSerialQueueCommit.retain(mailbox, seal)
                        .toCompletableFuture()
                        .get(5, TimeUnit.SECONDS)
                        .orElseThrow();
        var release = new CompletableFuture<Void>();
        var entered = new CompletableFuture<Void>();
        var occupied =
                spot.enqueue(
                        () -> {
                            entered.complete(null);
                            return release;
                        },
                        null);
        entered.get(5, TimeUnit.SECONDS);
        try (var callers = Executors.newVirtualThreadPerTaskExecutor()) {
            var invocation = callers.submit(retained::cut);
            try {
                var cut = invocation.get(1, TimeUnit.SECONDS);
                assertFalse(cut.toCompletableFuture().isDone());
            } finally {
                release.complete(null);
            }
            invocation.get(5, TimeUnit.SECONDS).toCompletableFuture().get(5, TimeUnit.SECONDS);
            assertTrue(retained.abort().toCompletableFuture().get(5, TimeUnit.SECONDS));
            occupied.toCompletableFuture().get(5, TimeUnit.SECONDS);
        } finally {
            release.complete(null);
            serials.close();
        }
    }

    @Test
    void externalRelocationSealReturnsWhileSharedGateIsOccupied() throws Exception {
        var spot = new ZLinkSerialExecutionQueue(Runnable::run, ZLinkExecutionLanePolicy.spot());
        var serials =
                new ZLinkSpotSerialExecutor(
                        spot,
                        Runnable::run,
                        Runnable::run,
                        ZLinkUserSpotExecutionMode.SPOT_WIDE,
                        false);
        serials.claimActorQueue("actor").toCompletableFuture().get(5, TimeUnit.SECONDS);
        var release = new CompletableFuture<Void>();
        var entered = new CompletableFuture<Void>();
        var occupied =
                spot.enqueue(
                        () -> {
                            entered.complete(null);
                            return release;
                        },
                        null);
        entered.get(5, TimeUnit.SECONDS);
        try (var callers = Executors.newVirtualThreadPerTaskExecutor()) {
            var invocation = callers.submit(() -> serials.trySealActorRelocation("actor"));
            try {
                var sealed = invocation.get(1, TimeUnit.SECONDS);
                assertFalse(sealed.toCompletableFuture().isDone());
            } finally {
                release.complete(null);
            }
            occupied.toCompletableFuture().get(5, TimeUnit.SECONDS);
            assertTrue(
                    invocation
                            .get(5, TimeUnit.SECONDS)
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS)
                            .isPresent());
        } finally {
            release.complete(null);
            serials.close();
        }
    }

    @Test
    void concurrentPublicationAcrossIdleTransitionsDoesNotStrandRecords() throws Exception {
        var serials =
                new ZLinkSpotSerialExecutor(
                        new ZLinkSerialExecutionQueue(ZLinkExecutionLanePolicy.spot()),
                        Runnable::run,
                        Runnable::run,
                        ZLinkUserSpotExecutionMode.SPOT_WIDE,
                        false);
        var actor = serials.claimActorQueue("actor").toCompletableFuture().get(5, TimeUnit.SECONDS);
        var calls = new AtomicInteger();
        @SuppressWarnings("unchecked")
        CompletionStage<Void>[] stages = new CompletionStage[400];
        try (var producers = Executors.newVirtualThreadPerTaskExecutor()) {
            var first =
                    producers.submit(
                            () -> {
                                for (int index = 0; index < 200; index++) {
                                    stages[index] =
                                            serials.executeActor(
                                                    actor,
                                                    0,
                                                    () -> {
                                                        calls.incrementAndGet();
                                                        return CompletableFuture.completedFuture(
                                                                null);
                                                    },
                                                    null);
                                }
                            });
            var second =
                    producers.submit(
                            () -> {
                                for (int index = 200; index < 400; index++) {
                                    stages[index] =
                                            serials.executeSpot(
                                                    0,
                                                    () -> {
                                                        calls.incrementAndGet();
                                                        return CompletableFuture.completedFuture(
                                                                null);
                                                    });
                                }
                            });
            first.get(5, TimeUnit.SECONDS);
            second.get(5, TimeUnit.SECONDS);
            CompletableFuture.allOf(
                            java.util.Arrays.stream(stages)
                                    .map(CompletionStage::toCompletableFuture)
                                    .toArray(CompletableFuture[]::new))
                    .get(10, TimeUnit.SECONDS);
            assertEquals(400, calls.get());
        } finally {
            serials.close();
        }
    }

    @Test
    void rejectedSharedExecutorSettlesAnAcceptedActorRecord() throws Exception {
        var rejectAfterAdmission = new CompletableFuture<Void>();
        var serials =
                new ZLinkSpotSerialExecutor(
                        new ZLinkSerialExecutionQueue(
                                ignored -> {
                                    rejectAfterAdmission.join();
                                    throw new RejectedExecutionException("test rejection");
                                },
                                ZLinkExecutionLanePolicy.spot()),
                        Runnable::run,
                        Runnable::run,
                        ZLinkUserSpotExecutionMode.SPOT_WIDE,
                        false);
        var actor = serials.claimActorQueue("actor").toCompletableFuture().get(5, TimeUnit.SECONDS);
        var admission = new CompletableFuture<Void>();
        var settled =
                serials.executeActor(
                        actor, 0, () -> CompletableFuture.completedFuture(null), admission);
        try {
            admission.get(5, TimeUnit.SECONDS);
            rejectAfterAdmission.complete(null);
            assertThrows(
                    ExecutionException.class,
                    () -> settled.toCompletableFuture().get(5, TimeUnit.SECONDS));
        } finally {
            rejectAfterAdmission.complete(null);
        }
    }

    @ParameterizedTest
    @CsvSource({"SPOT_WIDE,false", "SPOT_WIDE,true", "PER_ACTOR,true"})
    void actorClaimRequiresItsExecutionGate(ZLinkUserSpotExecutionMode mode, boolean occupiedGate)
            throws Exception {
        var tasks = new LinkedBlockingQueue<Runnable>();
        var spot = new ZLinkSerialExecutionQueue(tasks::add, ZLinkExecutionLanePolicy.spot());
        var serials = new ZLinkSpotSerialExecutor(spot, Runnable::run, tasks::add, mode, false);
        var first = serials.claimActorQueue("first").toCompletableFuture().get(5, TimeUnit.SECONDS);
        var second =
                serials.claimActorQueue("second").toCompletableFuture().get(5, TimeUnit.SECONDS);
        var release = new CompletableFuture<Void>();
        var entered = new CompletableFuture<Void>();
        var calls = new AtomicInteger();
        var occupied =
                spot.enqueue(
                        () -> {
                            entered.complete(null);
                            return release;
                        },
                        null);
        var initial = tasks.poll(5, TimeUnit.SECONDS);
        assertTrue(initial != null);
        initial.run();
        entered.get(5, TimeUnit.SECONDS);
        if (!occupiedGate) release.complete(null);
        Field active = ZLinkSerialExecutionQueue.class.getDeclaredField("active");
        active.setAccessible(true);
        try {
            var terminals = new java.util.ArrayList<CompletableFuture<Void>>();
            for (var actor : List.of(first, second)) {
                var admitted = new CompletableFuture<Void>();
                terminals.add(
                        serials.executeActor(
                                        actor,
                                        0,
                                        () -> {
                                            synchronized (actor.relocationLane()) {
                                                assertTrue(
                                                        activeRecord(active, actor.relocationLane())
                                                                != null);
                                            }
                                            calls.incrementAndGet();
                                            return CompletableFuture.completedFuture(null);
                                        },
                                        admitted)
                                .toCompletableFuture());
                admitted.get(5, TimeUnit.SECONDS);
            }
            // Run every consumer task the runtime published while the Spot turn is occupied.
            // The bounded poll also permits a passive mailbox to publish no consumer task.
            Runnable task;
            while ((task = tasks.poll(100, TimeUnit.MILLISECONDS)) != null) task.run();
            assertEquals(
                    occupiedGate && mode == ZLinkUserSpotExecutionMode.SPOT_WIDE ? 0 : 2,
                    calls.get());
            for (var actor : List.of(first, second)) {
                synchronized (actor.relocationLane()) {
                    assertNull(
                            active.get(actor.relocationLane()),
                            "Actor dequeue/claim occurred while another turn owned the Spot gate");
                }
            }
            release.complete(null);
            while (!CompletableFuture.allOf(terminals.toArray(CompletableFuture[]::new)).isDone()) {
                task = tasks.poll(5, TimeUnit.SECONDS);
                assertTrue(task != null);
                task.run();
            }
            CompletableFuture.allOf(terminals.toArray(CompletableFuture[]::new))
                    .get(5, TimeUnit.SECONDS);
            assertEquals(2, calls.get());
        } finally {
            release.complete(null);
            occupied.toCompletableFuture().get(5, TimeUnit.SECONDS);
            serials.close();
            Runnable task;
            while ((task = tasks.poll(100, TimeUnit.MILLISECONDS)) != null) task.run();
        }
    }

    private static Object activeRecord(Field field, ZLinkSerialExecutionQueue queue) {
        try {
            return field.get(queue);
        } catch (IllegalAccessException failure) {
            throw new AssertionError(failure);
        }
    }
}
