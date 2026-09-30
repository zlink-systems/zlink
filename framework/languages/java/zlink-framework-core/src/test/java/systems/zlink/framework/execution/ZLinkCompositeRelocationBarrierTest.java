package systems.zlink.framework.execution;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.runtime.internal.relocation.ZLinkCompositeRelocationBarrier;

import java.util.LinkedHashMap;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executor;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

final class ZLinkCompositeRelocationBarrierTest {
    @Test
    void sealAndAbortCompleteAfterTransitionStateClears() throws Exception {
        ZLinkCompositeRelocationBarrier barrier = new ZLinkCompositeRelocationBarrier();
        var transition = ZLinkCompositeRelocationBarrier.class.getDeclaredField("transition");
        transition.setAccessible(true);
        var seal =
                barrier.trySeal(
                                lanes(
                                        new ZLinkSerialExecutionQueue(),
                                        new ZLinkSerialExecutionQueue(),
                                        new ZLinkSerialExecutionQueue()))
                        .toCompletableFuture()
                        .get(3, TimeUnit.SECONDS)
                        .orElseThrow();
        assertNull(transition.get(barrier));
        assertTrue(barrier.abort(seal).toCompletableFuture().get(3, TimeUnit.SECONDS));
        assertNull(transition.get(barrier));
    }

    @Test
    void transitionDependentOperationsExposeAsynchronousCompletion() throws Exception {
        assertEquals(
                CompletionStage.class,
                ZLinkCompositeRelocationBarrier.class
                        .getMethod("abort", ZLinkCompositeRelocationBarrier.Seal.class)
                        .getReturnType());
        ZLinkCompositeRelocationBarrier barrier = new ZLinkCompositeRelocationBarrier();
        CompletableFuture<Void> pending = new CompletableFuture<>();
        var transition = ZLinkCompositeRelocationBarrier.class.getDeclaredField("transition");
        transition.setAccessible(true);
        transition.set(barrier, pending);

        CompletableFuture<Boolean> abort = barrier.abort(null).toCompletableFuture();
        assertFalse(abort.isDone());
        transition.set(barrier, null);
        pending.complete(null);
        assertFalse(abort.get(3, TimeUnit.SECONDS));
    }

    @Test
    void sealsSpotActorAndTimerAsOneGeneration() throws Exception {
        ZLinkSerialExecutionQueue spot = new ZLinkSerialExecutionQueue();
        ZLinkSerialExecutionQueue actor = new ZLinkSerialExecutionQueue();
        ZLinkSerialExecutionQueue timer = new ZLinkSerialExecutionQueue();
        ZLinkCompositeRelocationBarrier barrier = new ZLinkCompositeRelocationBarrier();

        var seal =
                barrier.trySeal(lanes(spot, actor, timer))
                        .toCompletableFuture()
                        .join()
                        .orElseThrow();
        assertEquals(1L, seal.generation());
        assertEquals(List.of("spot", "actor:a", "timer:t"), seal.laneIds());

        CompletableFuture<Void> spotHeld =
                spot.enqueueRelocatable(
                                new byte[] {1},
                                () ->
                                        CompletableFuture.failedFuture(
                                                new AssertionError("held Spot ingress ran")),
                                () -> {},
                                null)
                        .toCompletableFuture();
        CompletableFuture<Void> actorHeld =
                actor.enqueueRelocatable(
                                new byte[] {2},
                                () ->
                                        CompletableFuture.failedFuture(
                                                new AssertionError("held Actor ingress ran")),
                                () -> {},
                                null)
                        .toCompletableFuture();
        CompletableFuture<Void> timerHeld =
                timer.enqueueRelocatable(
                                new byte[] {3},
                                () ->
                                        CompletableFuture.failedFuture(
                                                new AssertionError("held timer ingress ran")),
                                () -> {},
                                null)
                        .toCompletableFuture();

        assertEquals(
                "captured",
                barrier.runCapture(seal, () -> CompletableFuture.completedFuture("captured"))
                        .toCompletableFuture()
                        .get(3, TimeUnit.SECONDS));

        var committed = barrier.commit(seal).toCompletableFuture().join().orElseThrow();
        CompletableFuture.allOf(spotHeld, actorHeld, timerHeld).get(3, TimeUnit.SECONDS);
        assertEquals(1, committed.get("spot").size());
        assertEquals(1, committed.get("actor:a").size());
        assertEquals(1, committed.get("timer:t").size());
        assertTrue(barrier.commit(seal).toCompletableFuture().join().isEmpty());
    }

    @Test
    void failedLaneRollsBackEveryEarlierSeal() throws Exception {
        ManualExecutor spotExecutor = new ManualExecutor();
        ManualExecutor actorExecutor = new ManualExecutor();
        ZLinkSerialExecutionQueue spot =
                new ZLinkSerialExecutionQueue(spotExecutor, ZLinkExecutionLanePolicy.generic());
        ZLinkSerialExecutionQueue actor =
                new ZLinkSerialExecutionQueue(actorExecutor, ZLinkExecutionLanePolicy.generic());
        ZLinkSerialExecutionQueue timer = new ZLinkSerialExecutionQueue();
        ZLinkCompositeRelocationBarrier barrier = new ZLinkCompositeRelocationBarrier();
        CompletableFuture<Void> actorActive = new CompletableFuture<>();
        actor.enqueue(() -> actorActive, null);
        actorExecutor.take().run();

        assertTrue(
                barrier.trySeal(lanes(spot, actor, timer)).toCompletableFuture().join().isEmpty());
        CompletableFuture<Void> spotIngress =
                spot.enqueue(() -> CompletableFuture.completedFuture(null), null)
                        .toCompletableFuture();
        spotExecutor.take().run();
        spotIngress.get(3, TimeUnit.SECONDS);
        spot.awaitQuiescence().toCompletableFuture().get(3, TimeUnit.SECONDS);

        actorActive.complete(null);
        actor.awaitQuiescence().toCompletableFuture().get(3, TimeUnit.SECONDS);
        var seal =
                barrier.trySeal(lanes(spot, actor, timer))
                        .toCompletableFuture()
                        .join()
                        .orElseThrow();
        assertTrue(barrier.abort(seal).toCompletableFuture().join());
        assertFalse(barrier.abort(seal).toCompletableFuture().join());
    }

    @Test
    void yieldedActorContinuationPreventsPartialAggregateSeal() throws Exception {
        ZLinkSerialExecutionQueue spot = new ZLinkSerialExecutionQueue();
        ZLinkSerialExecutionQueue actor = new ZLinkSerialExecutionQueue();
        ZLinkSerialExecutionQueue timer = new ZLinkSerialExecutionQueue();
        ZLinkCompositeRelocationBarrier barrier = new ZLinkCompositeRelocationBarrier();
        CompletableFuture<Void> remote = new CompletableFuture<>();
        CompletableFuture<Void> yielded = new CompletableFuture<>();

        CompletableFuture<Void> dispatch =
                actor.enqueue(
                                () -> {
                                    var continuation =
                                            ZLinkSerialExecutionQueue.yieldCurrent(remote);
                                    yielded.complete(null);
                                    return continuation;
                                },
                                null)
                        .toCompletableFuture();
        yielded.get(3, TimeUnit.SECONDS);

        assertTrue(
                barrier.trySeal(lanes(spot, actor, timer)).toCompletableFuture().join().isEmpty());
        remote.complete(null);
        dispatch.get(3, TimeUnit.SECONDS);
        actor.awaitQuiescence().toCompletableFuture().get(3, TimeUnit.SECONDS);

        var seal =
                barrier.trySeal(lanes(spot, actor, timer))
                        .toCompletableFuture()
                        .join()
                        .orElseThrow();
        assertThrows(
                IllegalStateException.class,
                () -> barrier.runCapture(null, () -> CompletableFuture.completedFuture(null)));
        assertTrue(barrier.abort(seal).toCompletableFuture().join());
    }

    @Test
    void turnBoundarySealWaitsForActiveTurnAndCapturesAcceptedQueue() throws Exception {
        ZLinkSerialExecutionQueue spot = new ZLinkSerialExecutionQueue();
        ZLinkSerialExecutionQueue actor = new ZLinkSerialExecutionQueue();
        ZLinkSerialExecutionQueue timer = new ZLinkSerialExecutionQueue();
        ZLinkCompositeRelocationBarrier barrier = new ZLinkCompositeRelocationBarrier();
        CompletableFuture<Void> active = new CompletableFuture<>();
        CompletableFuture<Void> started = new CompletableFuture<>();

        actor.enqueue(
                () -> {
                    started.complete(null);
                    return active;
                },
                null);
        started.get(3, TimeUnit.SECONDS);
        AtomicBoolean acceptedRan = new AtomicBoolean();
        CompletableFuture<Void> accepted =
                actor.enqueueRelocatable(
                                new byte[] {7},
                                () -> {
                                    acceptedRan.set(true);
                                    return CompletableFuture.completedFuture(null);
                                },
                                () -> {},
                                null)
                        .toCompletableFuture();

        CompletableFuture<Optional<ZLinkCompositeRelocationBarrier.Seal>> sealing =
                barrier.sealAtTurnBoundary(lanes(spot, actor, timer), () -> false)
                        .toCompletableFuture();

        assertFalse(sealing.isDone());
        active.complete(null);
        var seal = sealing.get(3, TimeUnit.SECONDS).orElseThrow();
        assertEquals(
                1,
                barrier.captured(seal)
                        .toCompletableFuture()
                        .join()
                        .orElseThrow()
                        .get("actor:a")
                        .size());
        assertFalse(acceptedRan.get());
        assertTrue(barrier.abort(seal).toCompletableFuture().join());
        accepted.get(3, TimeUnit.SECONDS);
        assertTrue(acceptedRan.get());
    }

    @Test
    void cancelledTurnBoundaryRestoresAcceptedQueueWithoutPartialSeal() throws Exception {
        ManualExecutor spotExecutor = new ManualExecutor();
        ManualExecutor actorExecutor = new ManualExecutor();
        ManualExecutor timerExecutor = new ManualExecutor();
        ZLinkSerialExecutionQueue spot =
                new ZLinkSerialExecutionQueue(spotExecutor, ZLinkExecutionLanePolicy.generic());
        ZLinkSerialExecutionQueue actor =
                new ZLinkSerialExecutionQueue(actorExecutor, ZLinkExecutionLanePolicy.generic());
        ZLinkSerialExecutionQueue timer =
                new ZLinkSerialExecutionQueue(timerExecutor, ZLinkExecutionLanePolicy.generic());
        ZLinkCompositeRelocationBarrier barrier = new ZLinkCompositeRelocationBarrier();
        CompletableFuture<Void> active = new CompletableFuture<>();
        AtomicBoolean cancelled = new AtomicBoolean();

        actor.enqueue(() -> active, null);
        actorExecutor.take().run();
        CompletableFuture<Void> accepted =
                actor.enqueueRelocatable(
                                new byte[] {9},
                                () -> CompletableFuture.completedFuture(null),
                                () -> {},
                                null)
                        .toCompletableFuture();
        CompletableFuture<Optional<ZLinkCompositeRelocationBarrier.Seal>> sealing =
                barrier.sealAtTurnBoundary(lanes(spot, actor, timer), cancelled::get)
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

        CompletableFuture<Void> quiescent = actor.awaitQuiescence().toCompletableFuture();
        CompletableFuture<Void> completionObserved = new CompletableFuture<>();
        CompletableFuture<Void> releaseCompletion = new CompletableFuture<>();
        CompletableFuture<Void> completionCallback =
                accepted.thenRun(
                        () -> {
                            // CompletableFuture runs this dependent before invokeInline can
                            // release the gate; the test observes that interval from outside it.
                            completionObserved.complete(null);
                            releaseCompletion.join();
                        });
        CompletableFuture<Void> acceptedDrain = CompletableFuture.runAsync(actorExecutor.take());
        try {
            completionObserved.get(3, TimeUnit.SECONDS);
            accepted.get(3, TimeUnit.SECONDS);
            assertFalse(quiescent.isDone());
            assertTrue(
                    barrier.trySeal(lanes(spot, actor, timer))
                            .toCompletableFuture()
                            .join()
                            .isEmpty());
        } finally {
            releaseCompletion.complete(null);
        }
        completionCallback.get(3, TimeUnit.SECONDS);
        acceptedDrain.get(3, TimeUnit.SECONDS);
        quiescent.get(3, TimeUnit.SECONDS);
        assertTrue(
                barrier.trySeal(lanes(spot, actor, timer))
                        .toCompletableFuture()
                        .join()
                        .isPresent());
    }

    @Test
    void turnBoundarySealWaitsForYieldedTerminalContinuation() throws Exception {
        ZLinkSerialExecutionQueue spot = new ZLinkSerialExecutionQueue();
        ZLinkSerialExecutionQueue actor =
                new ZLinkSerialExecutionQueue(ZLinkExecutionLanePolicy.spotReturningGate());
        ZLinkSerialExecutionQueue timer = new ZLinkSerialExecutionQueue();
        ZLinkCompositeRelocationBarrier barrier = new ZLinkCompositeRelocationBarrier();
        CompletableFuture<Void> remote = new CompletableFuture<>();
        CompletableFuture<Void> yielded = new CompletableFuture<>();

        CompletableFuture<Void> dispatch =
                actor.enqueue(
                                () -> {
                                    CompletionStage<Void> continuation =
                                            ZLinkSerialExecutionQueue.yieldCurrent(remote);
                                    yielded.complete(null);
                                    return continuation;
                                },
                                null)
                        .toCompletableFuture();
        yielded.get(3, TimeUnit.SECONDS);

        CompletableFuture<Optional<ZLinkCompositeRelocationBarrier.Seal>> sealing =
                barrier.sealAtTurnBoundary(lanes(spot, actor, timer), () -> false)
                        .toCompletableFuture();
        assertFalse(sealing.isDone());

        remote.complete(null);
        dispatch.get(3, TimeUnit.SECONDS);
        var seal = sealing.get(3, TimeUnit.SECONDS).orElseThrow();
        assertTrue(barrier.abort(seal).toCompletableFuture().join());
    }

    @Test
    void turnBoundarySealCompletesWhenReturningActorReachesBeforeTimer() throws Exception {
        ZLinkSerialExecutionQueue spot = new ZLinkSerialExecutionQueue();
        ManualExecutor actorExecutor = new ManualExecutor();
        ZLinkSerialExecutionQueue actor =
                new ZLinkSerialExecutionQueue(
                        actorExecutor, ZLinkExecutionLanePolicy.spotReturningGate());
        ZLinkSerialExecutionQueue timer = new ZLinkSerialExecutionQueue();
        ZLinkCompositeRelocationBarrier barrier = new ZLinkCompositeRelocationBarrier();
        CompletableFuture<Void> timerStarted = new CompletableFuture<>();
        CompletableFuture<Void> releaseTimer = new CompletableFuture<>();
        timer.enqueue(
                () -> {
                    timerStarted.complete(null);
                    return releaseTimer;
                },
                null);
        timerStarted.get(3, TimeUnit.SECONDS);

        CompletableFuture<Optional<ZLinkCompositeRelocationBarrier.Seal>> sealing =
                barrier.sealAtTurnBoundary(lanes(spot, actor, timer), () -> false)
                        .toCompletableFuture();
        actorExecutor.take().run();
        assertFalse(sealing.isDone());

        releaseTimer.complete(null);
        var seal = sealing.get(3, TimeUnit.SECONDS).orElseThrow();
        assertTrue(barrier.abort(seal).toCompletableFuture().get(3, TimeUnit.SECONDS));
    }

    private static final class ManualExecutor implements Executor {
        private final LinkedBlockingQueue<Runnable> pending = new LinkedBlockingQueue<>();

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
        LinkedHashMap<String, ZLinkSerialExecutionQueue> lanes = new LinkedHashMap<>();
        lanes.put("spot", spot);
        lanes.put("actor:a", actor);
        lanes.put("timer:t", timer);
        return lanes;
    }
}
