package systems.zlink.framework.execution;
import java.util.Optional;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.util.LinkedHashMap;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executor;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.runtime.internal.relocation
    .ZLinkCompositeRelocationBarrier;

final class ZLinkCompositeRelocationBarrierTest {
    @Test
    void sealsSpotActorAndTimerAsOneGeneration() throws Exception {
        ZLinkSerialExecutionQueue spot = new ZLinkSerialExecutionQueue();
        ZLinkSerialExecutionQueue actor = new ZLinkSerialExecutionQueue();
        ZLinkSerialExecutionQueue timer = new ZLinkSerialExecutionQueue();
        ZLinkCompositeRelocationBarrier barrier =
            new ZLinkCompositeRelocationBarrier();

        var seal = barrier.trySeal(lanes(spot, actor, timer))
            .orElseThrow();
        assertEquals(1L, seal.generation());
        assertEquals(
            List.of("spot", "actor:a", "timer:t"),
            seal.laneIds());

        CompletableFuture<Void> spotHeld = spot.enqueueRelocatable(
            new byte[] {1},
            () -> CompletableFuture.failedFuture(
                new AssertionError("held Spot ingress ran")))
            .toCompletableFuture();
        CompletableFuture<Void> actorHeld = actor.enqueueRelocatable(
            new byte[] {2},
            () -> CompletableFuture.failedFuture(
                new AssertionError("held Actor ingress ran")))
            .toCompletableFuture();
        CompletableFuture<Void> timerHeld = timer.enqueueRelocatable(
            new byte[] {3},
            () -> CompletableFuture.failedFuture(
                new AssertionError("held timer ingress ran")))
            .toCompletableFuture();

        assertEquals(
            "captured",
            barrier.runCapture(
                seal,
                () -> CompletableFuture.completedFuture("captured"))
                .toCompletableFuture()
                .get(3, TimeUnit.SECONDS));

        var committed = barrier.commit(seal).orElseThrow();
        CompletableFuture.allOf(
            spotHeld, actorHeld, timerHeld).get(3, TimeUnit.SECONDS);
        assertEquals(1, committed.get("spot").size());
        assertEquals(1, committed.get("actor:a").size());
        assertEquals(1, committed.get("timer:t").size());
        assertTrue(barrier.commit(seal).isEmpty());
    }

    @Test
    void failedLaneRollsBackEveryEarlierSeal() throws Exception {
        ManualExecutor spotExecutor = new ManualExecutor();
        ManualExecutor actorExecutor = new ManualExecutor();
        ZLinkSerialExecutionQueue spot = new ZLinkSerialExecutionQueue(
            spotExecutor, ZLinkExecutionLanePolicy.generic());
        ZLinkSerialExecutionQueue actor = new ZLinkSerialExecutionQueue(
            actorExecutor, ZLinkExecutionLanePolicy.generic());
        ZLinkSerialExecutionQueue timer = new ZLinkSerialExecutionQueue();
        ZLinkCompositeRelocationBarrier barrier =
            new ZLinkCompositeRelocationBarrier();
        CompletableFuture<Void> actorActive = new CompletableFuture<>();
        actor.enqueue(() -> actorActive);
        actorExecutor.take().run();

        assertTrue(barrier.trySeal(lanes(spot, actor, timer)).isEmpty());
        CompletableFuture<Void> spotIngress = spot.enqueue(
            () -> CompletableFuture.completedFuture(null))
            .toCompletableFuture();
        spotExecutor.take().run();
        spotIngress.get(3, TimeUnit.SECONDS);
        spot.awaitQuiescence().toCompletableFuture()
            .get(3, TimeUnit.SECONDS);

        actorActive.complete(null);
        actor.awaitQuiescence().toCompletableFuture()
            .get(3, TimeUnit.SECONDS);
        var seal = barrier.trySeal(lanes(spot, actor, timer))
            .orElseThrow();
        assertTrue(barrier.abort(seal));
        assertFalse(barrier.abort(seal));
    }

    @Test
    void yieldedActorContinuationPreventsPartialAggregateSeal()
        throws Exception {
        ZLinkSerialExecutionQueue spot = new ZLinkSerialExecutionQueue();
        ZLinkSerialExecutionQueue actor = new ZLinkSerialExecutionQueue();
        ZLinkSerialExecutionQueue timer = new ZLinkSerialExecutionQueue();
        ZLinkCompositeRelocationBarrier barrier =
            new ZLinkCompositeRelocationBarrier();
        CompletableFuture<Void> remote = new CompletableFuture<>();
        CompletableFuture<Void> yielded = new CompletableFuture<>();

        CompletableFuture<Void> dispatch = actor.enqueue(() -> {
            var continuation =
                ZLinkSerialExecutionQueue.yieldCurrent(remote);
            yielded.complete(null);
            return continuation;
        }).toCompletableFuture();
        yielded.get(3, TimeUnit.SECONDS);

        assertTrue(barrier.trySeal(lanes(spot, actor, timer)).isEmpty());
        remote.complete(null);
        dispatch.get(3, TimeUnit.SECONDS);
        actor.awaitQuiescence().toCompletableFuture()
            .get(3, TimeUnit.SECONDS);

        var seal = barrier.trySeal(lanes(spot, actor, timer))
            .orElseThrow();
        assertThrows(
            IllegalStateException.class,
            () -> barrier.runCapture(
                null,
                () -> CompletableFuture.completedFuture(null)));
        assertTrue(barrier.abort(seal));
    }

    @Test
    void turnBoundarySealWaitsForActiveTurnAndCapturesAcceptedQueue()
        throws Exception {
        ZLinkSerialExecutionQueue spot = new ZLinkSerialExecutionQueue();
        ZLinkSerialExecutionQueue actor = new ZLinkSerialExecutionQueue();
        ZLinkSerialExecutionQueue timer = new ZLinkSerialExecutionQueue();
        ZLinkCompositeRelocationBarrier barrier =
            new ZLinkCompositeRelocationBarrier();
        CompletableFuture<Void> active = new CompletableFuture<>();
        CompletableFuture<Void> started = new CompletableFuture<>();

        actor.enqueue(() -> {
            started.complete(null);
            return active;
        });
        started.get(3, TimeUnit.SECONDS);
        AtomicBoolean acceptedRan = new AtomicBoolean();
        CompletableFuture<Void> accepted = actor.enqueueRelocatable(
            new byte[] {7},
            () -> {
                acceptedRan.set(true);
                return CompletableFuture.completedFuture(null);
            })
            .toCompletableFuture();

        CompletableFuture<Optional<
            ZLinkCompositeRelocationBarrier.Seal>> sealing =
                barrier.sealAtTurnBoundary(
                    lanes(spot, actor, timer),
                    () -> false)
                .toCompletableFuture();

        assertFalse(sealing.isDone());
        active.complete(null);
        var seal = sealing.get(3, TimeUnit.SECONDS).orElseThrow();
        assertEquals(
            1,
            barrier.captured(seal).orElseThrow()
                .get("actor:a").size());
        assertFalse(acceptedRan.get());
        assertTrue(barrier.abort(seal));
        accepted.get(3, TimeUnit.SECONDS);
        assertTrue(acceptedRan.get());
    }

    @Test
    void cancelledTurnBoundaryRestoresAcceptedQueueWithoutPartialSeal()
        throws Exception {
        ManualExecutor spotExecutor = new ManualExecutor();
        ManualExecutor actorExecutor = new ManualExecutor();
        ManualExecutor timerExecutor = new ManualExecutor();
        ZLinkSerialExecutionQueue spot = new ZLinkSerialExecutionQueue(
            spotExecutor, ZLinkExecutionLanePolicy.generic());
        ZLinkSerialExecutionQueue actor = new ZLinkSerialExecutionQueue(
            actorExecutor, ZLinkExecutionLanePolicy.generic());
        ZLinkSerialExecutionQueue timer = new ZLinkSerialExecutionQueue(
            timerExecutor, ZLinkExecutionLanePolicy.generic());
        ZLinkCompositeRelocationBarrier barrier =
            new ZLinkCompositeRelocationBarrier();
        CompletableFuture<Void> active = new CompletableFuture<>();
        AtomicBoolean cancelled = new AtomicBoolean();

        actor.enqueue(() -> active);
        actorExecutor.take().run();
        CompletableFuture<Void> accepted = actor.enqueueRelocatable(
            new byte[] {9},
            () -> CompletableFuture.completedFuture(null))
            .toCompletableFuture();
        CompletableFuture<Optional<
            ZLinkCompositeRelocationBarrier.Seal>> sealing =
                barrier.sealAtTurnBoundary(
                    lanes(spot, actor, timer),
                    cancelled::get)
                .toCompletableFuture();

        spotExecutor.take().run();
        assertFalse(sealing.isDone());
        active.complete(null);
        actorExecutor.take().run();
        assertFalse(accepted.isDone());
        assertFalse(sealing.isDone());

        // Keep the timer boundary pending until the Actor boundary has suspended,
        // so restored work always needs a separately controlled executor task.
        cancelled.set(true);
        timerExecutor.take().run();
        assertTrue(sealing.get(3, TimeUnit.SECONDS).isEmpty());
        assertFalse(accepted.isDone());

        CompletableFuture<Void> quiescent =
            actor.awaitQuiescence().toCompletableFuture();
        CompletableFuture<Void> completionObserved = new CompletableFuture<>();
        CompletableFuture<Void> releaseCompletion = new CompletableFuture<>();
        CompletableFuture<Void> completionCallback = accepted.thenRun(() -> {
            // CompletableFuture runs this dependent before invokeInline can
            // release the gate; the test observes that interval from outside it.
            completionObserved.complete(null);
            releaseCompletion.join();
        });
        CompletableFuture<Void> acceptedDrain =
            CompletableFuture.runAsync(actorExecutor.take());
        try {
            completionObserved.get(3, TimeUnit.SECONDS);
            accepted.get(3, TimeUnit.SECONDS);
            assertFalse(quiescent.isDone());
            assertTrue(barrier.trySeal(lanes(spot, actor, timer)).isEmpty());
        } finally {
            releaseCompletion.complete(null);
        }
        completionCallback.get(3, TimeUnit.SECONDS);
        acceptedDrain.get(3, TimeUnit.SECONDS);
        quiescent.get(3, TimeUnit.SECONDS);
        assertTrue(barrier.trySeal(lanes(spot, actor, timer)).isPresent());
    }

    @Test
    void turnBoundarySealWaitsForYieldedTerminalContinuation()
        throws Exception {
        ZLinkSerialExecutionQueue spot = new ZLinkSerialExecutionQueue();
        ZLinkSerialExecutionQueue actor = new ZLinkSerialExecutionQueue(
            ZLinkExecutionLanePolicy.spotReturningGate());
        ZLinkSerialExecutionQueue timer = new ZLinkSerialExecutionQueue();
        ZLinkCompositeRelocationBarrier barrier =
            new ZLinkCompositeRelocationBarrier();
        CompletableFuture<Void> remote = new CompletableFuture<>();
        CompletableFuture<Void> yielded = new CompletableFuture<>();

        CompletableFuture<Void> dispatch = actor.enqueue(() -> {
            CompletionStage<Void> continuation =
                ZLinkSerialExecutionQueue.yieldCurrent(remote);
            yielded.complete(null);
            return continuation;
        }).toCompletableFuture();
        yielded.get(3, TimeUnit.SECONDS);

        CompletableFuture<Optional<
            ZLinkCompositeRelocationBarrier.Seal>> sealing =
                barrier.sealAtTurnBoundary(
                    lanes(spot, actor, timer),
                    () -> false)
                .toCompletableFuture();
        assertFalse(sealing.isDone());

        remote.complete(null);
        dispatch.get(3, TimeUnit.SECONDS);
        var seal = sealing.get(3, TimeUnit.SECONDS).orElseThrow();
        assertTrue(barrier.abort(seal));
    }

    private static final class ManualExecutor implements Executor {
        private final LinkedBlockingQueue<Runnable> pending =
            new LinkedBlockingQueue<>();

        @Override
        public void execute(Runnable command) {
            pending.add(command);
        }

        private Runnable take() throws InterruptedException {
            Runnable task = pending.poll(3, TimeUnit.SECONDS);
            assertTrue(task != null, "the lane must submit its next drain task");
            return task;
        }
    }

    private static LinkedHashMap<String, ZLinkSerialExecutionQueue> lanes(
        ZLinkSerialExecutionQueue spot,
        ZLinkSerialExecutionQueue actor,
        ZLinkSerialExecutionQueue timer) {
        LinkedHashMap<String, ZLinkSerialExecutionQueue> lanes =
            new LinkedHashMap<>();
        lanes.put("spot", spot);
        lanes.put("actor:a", actor);
        lanes.put("timer:t", timer);
        return lanes;
    }
}
