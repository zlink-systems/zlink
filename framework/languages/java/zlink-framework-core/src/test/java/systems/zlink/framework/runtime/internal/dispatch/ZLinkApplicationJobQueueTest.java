package systems.zlink.framework.runtime.internal.dispatch;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.OptionalLong;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.errors.ConfigResult;
import systems.zlink.contracts.errors.ZlinkConfigException;
import systems.zlink.contracts.sockets.ReceiveFlowState;
import systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.monitoring.ZLinkApplicationJobQueuePressureState;

final class ZLinkApplicationJobQueueTest {
    @Test
    void resolvesTheExactProfileMatrixAndManualOverride() {
        for (int processors : List.of(4, 8, 16)) {
            var candidates = candidates(processors);
            assertEquals(
                32L * processors,
                resolved(ZLinkApplicationJobQueueProfile.COMPACT, candidates));
            assertEquals(
                64L * processors,
                resolved(ZLinkApplicationJobQueueProfile.LOW_LATENCY, candidates));
            assertEquals(
                128L * processors,
                resolved(ZLinkApplicationJobQueueProfile.BALANCED, candidates));
            assertEquals(
                256L * processors,
                resolved(ZLinkApplicationJobQueueProfile.THROUGHPUT, candidates));
        }

        var manual = ZLinkApplicationJobQueue.resolveCapacity(
            ZLinkApplicationJobQueueProfile.COMPACT,
            OptionalLong.of(Integer.MAX_VALUE),
            candidates(16));
        assertEquals(16, manual.effectiveProcessorCount());
        assertEquals(Integer.MAX_VALUE, manual.effectiveLimit());
    }

    @Test
    void usesTheMinimumKnownPositiveProcessorCandidateAndRejectsInvalidCapacity() {
        var resolved = ZLinkApplicationJobQueue.resolveCapacity(
            ZLinkApplicationJobQueueProfile.BALANCED,
            OptionalLong.empty(),
            new ZLinkApplicationJobQueue.ProcessorCandidates(16, 12, 7, 8));
        assertEquals(7, resolved.effectiveProcessorCount());
        assertEquals(7L * 128L, resolved.effectiveLimit());

        assertThrows(
            ZLinkConfigurationException.class,
            () -> ZLinkApplicationJobQueue.resolveCapacity(
                ZLinkApplicationJobQueueProfile.BALANCED,
                OptionalLong.of(0),
                candidates(1)));
        assertThrows(
            ZLinkConfigurationException.class,
            () -> ZLinkApplicationJobQueue.resolveCapacity(
                ZLinkApplicationJobQueueProfile.THROUGHPUT,
                OptionalLong.empty(),
                candidates(Integer.MAX_VALUE)));
    }

    @Test
    void tracksReservedQueuedAndInUseUntilTheHandlerFirstInstruction() {
        ZLinkApplicationJobQueue queue = queue(1);
        ZLinkApplicationJobQueue.Permit permit = queue.acquire().toCompletableFuture().join();

        assertQueue(queue, 1, 0, 1, 1, 0);
        permit.queued();
        assertQueue(queue, 0, 1, 1, 1, 0);

        permit.handlerStarted();
        assertQueue(queue, 0, 0, 0, 1, 0);
        permit.handlerStarted();
        permit.close();
        assertQueue(queue, 0, 0, 0, 1, 0);
    }

    @Test
    void handsReleasedCapacityToTheOldestLiveWaiterWithoutBarging() {
        ZLinkApplicationJobQueue queue = queue(1);
        ZLinkApplicationJobQueue.Permit active = queue.acquire().toCompletableFuture().join();
        List<Integer> order = new ArrayList<>();
        CompletableFuture<ZLinkApplicationJobQueue.Permit> first =
            queue.acquire().toCompletableFuture();
        CompletableFuture<ZLinkApplicationJobQueue.Permit> cancelledOldest =
            queue.acquire().toCompletableFuture();
        CompletableFuture<ZLinkApplicationJobQueue.Permit> third =
            queue.acquire().toCompletableFuture();
        first.thenAccept(permit -> order.add(1));
        cancelledOldest.thenAccept(permit -> order.add(2));
        third.thenAccept(permit -> order.add(3));

        assertEquals(3, queue.snapshot().capacityWaiters());
        assertTrue(cancelledOldest.cancel(false));
        assertEquals(2, queue.snapshot().capacityWaiters());

        active.handlerStarted();
        ZLinkApplicationJobQueue.Permit firstPermit = first.join();
        assertEquals(List.of(1), order);
        assertFalse(third.isDone());

        CompletableFuture<ZLinkApplicationJobQueue.Permit> barger =
            queue.acquire().toCompletableFuture();
        firstPermit.handlerStarted();
        ZLinkApplicationJobQueue.Permit thirdPermit = third.join();
        assertEquals(List.of(1, 3), order);
        assertFalse(barger.isDone());

        thirdPermit.handlerStarted();
        barger.join().handlerStarted();
        assertEquals(0, queue.snapshot().permitsInUse());
    }

    @Test
    void capacityWaitIsCancellableAndResetKeepsCurrentWhileRebasingTheEpoch() {
        AtomicLong now = new AtomicLong(1_000L);
        ZLinkApplicationJobQueue queue = new ZLinkApplicationJobQueue(
            ZLinkApplicationJobQueueProfile.BALANCED,
            OptionalLong.of(1),
            candidates(8),
            now::get);
        ZLinkApplicationJobQueue.Permit active = queue.acquire().toCompletableFuture().join();
        CompletableFuture<ZLinkApplicationJobQueue.Permit> waiter =
            queue.acquire().toCompletableFuture();
        now.addAndGet(5_000_000L);
        assertTrue(waiter.cancel(false));

        var before = queue.snapshot();
        assertEquals(1, before.capacityWaitCount());
        assertEquals(Duration.ofMillis(5), before.capacityWaitDuration());
        assertEquals(1, before.peakPermitsInUse());

        queue.resetMetrics();
        var after = queue.snapshot();
        assertEquals(1, after.permitsInUse());
        assertEquals(1, after.peakPermitsInUse());
        assertEquals(0, after.capacityWaitCount());
        assertEquals(Duration.ZERO, after.capacityWaitDuration());

        active.close();
        assertEquals(0, queue.snapshot().permitsInUse());
    }

    @Test
    void capacityWaitDurationStaysInTheEpochWhereTheWaitStarted() {
        AtomicLong now = new AtomicLong(1_000L);
        ZLinkApplicationJobQueue queue = new ZLinkApplicationJobQueue(
            ZLinkApplicationJobQueueProfile.BALANCED,
            OptionalLong.of(1),
            candidates(1),
            now::get);
        ZLinkApplicationJobQueue.Permit active =
            queue.acquire().toCompletableFuture().join();
        CompletableFuture<ZLinkApplicationJobQueue.Permit> waiter =
            queue.acquire().toCompletableFuture();
        assertEquals(1, queue.snapshot().capacityWaitCount());

        now.set(2_000L);
        queue.resetMetrics();
        now.set(3_000L);
        active.close();
        ZLinkApplicationJobQueue.Permit granted = waiter.join();

        assertEquals(0, queue.snapshot().capacityWaitCount());
        assertEquals(Duration.ZERO, queue.snapshot().capacityWaitDuration());
        granted.close();
    }

    @Test
    void closeReleasesParkedWaitersExactlyOnceAndStaysIdempotent() {
        ZLinkApplicationJobQueue queue = queue(1);
        ZLinkApplicationJobQueue.Permit held =
            queue.acquire().toCompletableFuture().join();
        CompletableFuture<ZLinkApplicationJobQueue.Permit> parked =
            queue.acquire().toCompletableFuture();
        assertFalse(parked.isDone());
        assertEquals(1, queue.snapshot().capacityWaiters());

        //  Spec 33 §8 — shutdown releases parked waiters exactly once.
        queue.close();
        assertTrue(parked.isCancelled());
        assertEquals(0, queue.snapshot().capacityWaiters());

        //  The still-held permit returns exactly once after shutdown;
        //  repeated releases cannot double-return capacity.
        held.close();
        held.close();
        assertQueue(queue, 0, 0, 0, 1, 0);

        //  Shutdown is idempotent and later acquires fail promptly instead
        //  of parking forever.
        queue.close();
        assertQueue(queue, 0, 0, 0, 1, 0);
        assertTrue(
            queue.acquire().toCompletableFuture().isCompletedExceptionally());
    }

    @Test
    void oneToManyChildrenNeverMaterializeBeyondSecuredPermits() {
        //  Spec 33 §5/§8 — 1:N children acquire one permit each, lazily and
        //  in order; no more children materialize than secured permits.
        ZLinkApplicationJobQueue queue = queue(1);
        List<Integer> materialized = new ArrayList<>();

        ZLinkApplicationJobQueue.Permit firstChild =
            queue.acquire().toCompletableFuture().join();
        firstChild.queued();
        materialized.add(1);
        CompletableFuture<ZLinkApplicationJobQueue.Permit> secondChild =
            queue.acquire().toCompletableFuture();
        assertFalse(secondChild.isDone());
        assertEquals(List.of(1), materialized);

        firstChild.handlerStarted();
        ZLinkApplicationJobQueue.Permit secondPermit = secondChild.join();
        secondPermit.queued();
        materialized.add(2);
        CompletableFuture<ZLinkApplicationJobQueue.Permit> thirdChild =
            queue.acquire().toCompletableFuture();
        assertFalse(thirdChild.isDone());

        secondPermit.handlerStarted();
        ZLinkApplicationJobQueue.Permit thirdPermit = thirdChild.join();
        thirdPermit.queued();
        materialized.add(3);
        thirdPermit.handlerStarted();

        assertEquals(List.of(1, 2, 3), materialized);
        assertEquals(1, queue.snapshot().peakPermitsInUse());
        assertEquals(0, queue.snapshot().permitsInUse());
    }

    @Test
    void appliesOneHystereticPressureTransitionForReservedAndQueuedPermits() {
        AtomicLong now = new AtomicLong(1_000L);
        ZLinkApplicationJobQueue queue = new ZLinkApplicationJobQueue(
            ZLinkApplicationJobQueueProfile.BALANCED,
            OptionalLong.of(10),
            candidates(1),
            80,
            60,
            now::get);
        List<ReceiveFlowState> applied = new ArrayList<>();
        var registration = queue.registerReceiveFlowTarget(applied::add);
        List<ZLinkApplicationJobQueue.Permit> permits = new ArrayList<>();
        for (int index = 0; index < 8; index++) {
            permits.add(queue.acquire().toCompletableFuture().join());
        }

        var paused = queue.snapshot();
        assertEquals(8, paused.pausePermitCount());
        assertEquals(6, paused.resumePermitCount());
        assertEquals(8, paused.reservedSupplyPermits());
        assertEquals(0, paused.queuedApplicationJobs());
        assertEquals(8, paused.permitsInUse());
        assertEquals(ZLinkApplicationJobQueuePressureState.PAUSED,
            paused.pressureState());
        assertEquals(List.of(ReceiveFlowState.RUNNING, ReceiveFlowState.PAUSED),
            applied);

        permits.getFirst().queued();
        assertEquals(7, queue.snapshot().reservedSupplyPermits());
        assertEquals(1, queue.snapshot().queuedApplicationJobs());
        assertEquals(List.of(ReceiveFlowState.RUNNING, ReceiveFlowState.PAUSED),
            applied);

        permits.get(1).close();
        assertEquals(ZLinkApplicationJobQueuePressureState.PAUSED,
            queue.snapshot().pressureState());
        permits.get(2).close();
        assertEquals(ZLinkApplicationJobQueuePressureState.RUNNING,
            queue.snapshot().pressureState());
        assertEquals(List.of(
            ReceiveFlowState.RUNNING,
            ReceiveFlowState.PAUSED,
            ReceiveFlowState.RUNNING), applied);

        registration.close();
        permits.forEach(ZLinkApplicationJobQueue.Permit::close);
    }

    @Test
    void deregistrationPreventsLateSocketStateApplication() {
        ZLinkApplicationJobQueue queue = new ZLinkApplicationJobQueue(
            ZLinkApplicationJobQueueProfile.BALANCED,
            OptionalLong.of(1),
            candidates(1),
            100,
            0);
        List<ReceiveFlowState> applied = new ArrayList<>();
        var registration = queue.registerReceiveFlowTarget(applied::add);
        ZLinkApplicationJobQueue.Permit permit =
            queue.acquire().toCompletableFuture().join();
        registration.close();
        permit.close();

        assertEquals(List.of(ReceiveFlowState.RUNNING, ReceiveFlowState.PAUSED),
            applied);
    }

    @Test
    void registrationQueuesAnInterveningTransitionAndSkipsTheSameSequence() {
        ZLinkApplicationJobQueue queue = new ZLinkApplicationJobQueue(
            ZLinkApplicationJobQueueProfile.BALANCED,
            OptionalLong.of(1), candidates(1), 100, 0);
        ZLinkApplicationJobReceiveFlowController controller =
            new ZLinkApplicationJobReceiveFlowController(queue);
        List<ReceiveFlowState> applied = new ArrayList<>();
        AtomicReference<ZLinkApplicationJobQueue.Permit> held =
            new AtomicReference<>();
        var registration = controller.register(state -> {
            applied.add(state);
            if (state == ReceiveFlowState.RUNNING) {
                held.set(queue.acquire().toCompletableFuture().join());
                // This is deliberately inside the initial setter. The target
                // has already been registered but its initial snapshot has
                // not completed, matching the registration/transition race.
                controller.onPressureTransition(queue.pressureSnapshot());
            }
        });

        assertEquals(List.of(ReceiveFlowState.RUNNING, ReceiveFlowState.PAUSED),
            applied);
        controller.onPressureTransition(queue.pressureSnapshot());
        assertEquals(List.of(ReceiveFlowState.RUNNING, ReceiveFlowState.PAUSED),
            applied);

        registration.close();
        held.get().close();
    }

    @Test
    void initialReceiveFlowConfigurationFailureIsReportedAndNotRetained() {
        ZLinkApplicationJobQueue queue = new ZLinkApplicationJobQueue(
            ZLinkApplicationJobQueueProfile.BALANCED,
            OptionalLong.of(1), candidates(1), 100, 0);
        AtomicLong calls = new AtomicLong();

        assertThrows(ZlinkConfigException.class,
            () -> queue.registerReceiveFlowTarget(state -> {
                calls.incrementAndGet();
                throw new ZlinkConfigException(ConfigResult.INVALID_STATE);
            }));
        assertEquals(1, calls.get());
        assertEquals(1, queue.pressureMetrics().flowStateConfigFailureCount());

        ZLinkApplicationJobQueue.Permit permit =
            queue.acquire().toCompletableFuture().join();
        permit.close();
        assertEquals(1, calls.get());
    }

    @Test
    void receiveFlowRegistrationAfterQueueCloseFailsBeforeSocketPublication() {
        ZLinkApplicationJobQueue queue = new ZLinkApplicationJobQueue(
            ZLinkApplicationJobQueueProfile.BALANCED,
            OptionalLong.of(1), candidates(1), 100, 0);
        AtomicLong calls = new AtomicLong();

        queue.close();

        assertThrows(IllegalStateException.class,
            () -> queue.registerReceiveFlowTarget(state ->
                calls.incrementAndGet()));
        assertEquals(0, calls.get());
    }

    @Test
    void queueCloseFencesRegistrationBeforeCancelledWaiterCallbacksRun() {
        ZLinkApplicationJobQueue queue = new ZLinkApplicationJobQueue(
            ZLinkApplicationJobQueueProfile.BALANCED,
            OptionalLong.of(1), candidates(1), 100, 0);
        ZLinkApplicationJobQueue.Permit held =
            queue.acquire().toCompletableFuture().join();
        CompletableFuture<ZLinkApplicationJobQueue.Permit> waiting =
            queue.acquire().toCompletableFuture();
        AtomicLong calls = new AtomicLong();
        AtomicReference<ZLinkApplicationJobReceiveFlowController.Registration>
            registration = new AtomicReference<>();
        AtomicReference<Throwable> registrationFailure = new AtomicReference<>();
        waiting.whenComplete((ignored, failure) -> {
            try {
                registration.set(queue.registerReceiveFlowTarget(state ->
                    calls.incrementAndGet()));
            } catch (Throwable error) {
                registrationFailure.set(error);
            }
        });

        queue.close();

        assertTrue(waiting.isCancelled());
        assertNull(registration.get());
        assertInstanceOf(IllegalStateException.class,
            registrationFailure.get());
        assertEquals(0, calls.get());
        held.close();
    }

    @Test
    void queueCloseDoesNotWaitForApplyAndBlocksTheNextStaleTarget()
        throws Exception {
        ZLinkApplicationJobQueue queue = new ZLinkApplicationJobQueue(
            ZLinkApplicationJobQueueProfile.BALANCED,
            OptionalLong.of(1), candidates(1), 100, 0);
        CountDownLatch firstPauseEntered = new CountDownLatch(1);
        CountDownLatch releaseFirstPause = new CountDownLatch(1);
        AtomicLong pauseCalls = new AtomicLong();
        java.util.function.Consumer<ReceiveFlowState> apply = state -> {
            if (state != ReceiveFlowState.PAUSED
                || pauseCalls.incrementAndGet() != 1) {
                return;
            }
            firstPauseEntered.countDown();
            try {
                releaseFirstPause.await();
            } catch (InterruptedException interruption) {
                Thread.currentThread().interrupt();
                throw new AssertionError(interruption);
            }
        };
        var first = queue.registerReceiveFlowTarget(apply::accept);
        var second = queue.registerReceiveFlowTarget(apply::accept);
        CompletableFuture<ZLinkApplicationJobQueue.Permit> acquire =
            CompletableFuture.supplyAsync(() ->
                queue.acquire().toCompletableFuture().join());
        assertTrue(firstPauseEntered.await(1, TimeUnit.SECONDS));

        CompletableFuture<Void> close =
            CompletableFuture.runAsync(queue::close);
        CompletableFuture<Void> unregister = null;
        try {
            close.get(1, TimeUnit.SECONDS);
            CountDownLatch unregisterStarted = new CountDownLatch(1);
            unregister = CompletableFuture.runAsync(() -> {
                unregisterStarted.countDown();
                first.close();
                second.close();
            });
            assertTrue(unregisterStarted.await(1, TimeUnit.SECONDS));
            CompletableFuture<Void> currentUnregister = unregister;
            assertThrows(TimeoutException.class, () ->
                currentUnregister.get(100, TimeUnit.MILLISECONDS));
        } finally {
            releaseFirstPause.countDown();
        }

        ZLinkApplicationJobQueue.Permit permit =
            acquire.get(1, TimeUnit.SECONDS);
        permit.close();
        unregister.get(1, TimeUnit.SECONDS);
        assertEquals(1, pauseCalls.get());
    }

    @Test
    void receiveFlowRegistrationClosedDuringInitialStateApplicationFails() {
        ZLinkApplicationJobQueue queue = new ZLinkApplicationJobQueue(
            ZLinkApplicationJobQueueProfile.BALANCED,
            OptionalLong.of(1), candidates(1), 100, 0);
        ZLinkApplicationJobReceiveFlowController controller =
            new ZLinkApplicationJobReceiveFlowController(queue);
        List<ReceiveFlowState> applied = new ArrayList<>();

        assertThrows(IllegalStateException.class,
            () -> controller.register(state -> {
                applied.add(state);
                controller.close();
            }));

        assertEquals(List.of(ReceiveFlowState.RUNNING), applied);
        queue.close();
    }

    @Test
    void resetKeepsCurrentPressureButClearsMetricOnlyPressureCounters() {
        AtomicLong now = new AtomicLong(100L);
        ZLinkApplicationJobQueue queue = new ZLinkApplicationJobQueue(
            ZLinkApplicationJobQueueProfile.BALANCED,
            OptionalLong.of(1), candidates(1), 100, 0, now::get);
        queue.registerReceiveFlowTarget(state -> {
            if (state == ReceiveFlowState.PAUSED) {
                throw new ZlinkConfigException(ConfigResult.INVALID_STATE);
            }
        });
        ZLinkApplicationJobQueue.Permit held =
            queue.acquire().toCompletableFuture().join();
        now.addAndGet(10L);

        var before = queue.pressureMetrics();
        assertEquals(1, before.pausedTransitionCount());
        assertEquals(1, before.flowStateConfigFailureCount());
        assertEquals(Duration.ofNanos(10L), before.currentPauseDuration());
        assertEquals(Duration.ofNanos(10L), before.cumulativePauseDuration());

        queue.resetMetrics();
        var reset = queue.pressureMetrics();
        assertEquals(ZLinkApplicationJobQueuePressureState.PAUSED,
            reset.pressureState());
        assertEquals(Duration.ofNanos(10L), reset.currentPauseDuration());
        assertEquals(Duration.ZERO, reset.cumulativePauseDuration());
        assertEquals(0, reset.pausedTransitionCount());
        assertEquals(0, reset.runningTransitionCount());
        assertEquals(0, reset.flowStateConfigFailureCount());

        now.addAndGet(5L);
        held.close();
        var resumed = queue.pressureMetrics();
        assertEquals(1, resumed.runningTransitionCount());
        assertEquals(Duration.ofNanos(5L), resumed.cumulativePauseDuration());
    }

    @Test
    void validatesThresholdRangesAndStartupHysteresisOrdering() {
        assertThrows(
            ZLinkConfigurationException.class,
            () -> new ZLinkApplicationJobQueue(
                ZLinkApplicationJobQueueProfile.BALANCED,
                OptionalLong.of(1), candidates(1), 0, 0));
        assertThrows(
            ZLinkConfigurationException.class,
            () -> new ZLinkApplicationJobQueue(
                ZLinkApplicationJobQueueProfile.BALANCED,
                OptionalLong.of(1), candidates(1), 80, 80));
        assertThrows(
            ZLinkConfigurationException.class,
            () -> new ZLinkApplicationJobQueue(
                ZLinkApplicationJobQueueProfile.BALANCED,
                OptionalLong.of(1), candidates(1), 100, 100));
    }

    private static long resolved(
        ZLinkApplicationJobQueueProfile profile,
        ZLinkApplicationJobQueue.ProcessorCandidates candidates) {
        return ZLinkApplicationJobQueue.resolveCapacity(
            profile,
            OptionalLong.empty(),
            candidates).effectiveLimit();
    }

    private static ZLinkApplicationJobQueue queue(long limit) {
        return new ZLinkApplicationJobQueue(
            ZLinkApplicationJobQueueProfile.BALANCED,
            OptionalLong.of(limit),
            candidates(8));
    }

    private static ZLinkApplicationJobQueue.ProcessorCandidates candidates(int value) {
        return new ZLinkApplicationJobQueue.ProcessorCandidates(value, null, null, null);
    }

    private static void assertQueue(
        ZLinkApplicationJobQueue queue,
        long reserved,
        long queued,
        long inUse,
        long peak,
        long waiters) {
        var snapshot = queue.snapshot();
        assertEquals(reserved, snapshot.reservedSupplyPermits());
        assertEquals(queued, snapshot.queuedApplicationJobs());
        assertEquals(inUse, snapshot.permitsInUse());
        assertEquals(peak, snapshot.peakPermitsInUse());
        assertEquals(waiters, snapshot.capacityWaiters());
    }
}
