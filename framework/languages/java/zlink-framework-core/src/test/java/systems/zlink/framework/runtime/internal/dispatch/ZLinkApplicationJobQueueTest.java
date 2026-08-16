package systems.zlink.framework.runtime.internal.dispatch;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.OptionalLong;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.atomic.AtomicLong;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile;
import systems.zlink.framework.errors.ZLinkConfigurationException;

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
