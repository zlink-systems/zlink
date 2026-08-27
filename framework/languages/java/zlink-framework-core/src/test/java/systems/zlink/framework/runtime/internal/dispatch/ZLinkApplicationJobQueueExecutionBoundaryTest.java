package systems.zlink.framework.runtime.internal.dispatch;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.OptionalLong;
import java.util.concurrent.CompletableFuture;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile;
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue;
import systems.zlink.framework.execution.ZLinkExecutionLanePolicy;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker;

final class ZLinkApplicationJobQueueExecutionBoundaryTest {
    @Test
    void reservationCrossesSerialBacklogAndReturnsBeforeFirstApplicationInstruction() {
        ZLinkApplicationJobQueue queue = queue(1);
        ZLinkAsyncSerialQueue serial = new ZLinkAsyncSerialQueue(
            Runnable::run, ZLinkExecutionLanePolicy.generic());
        CompletableFuture<Void> handlerContinuation = new CompletableFuture<>();
        ZLinkApplicationJobQueue.Permit first = queue.acquire().toCompletableFuture().join();

        try (var ignored = ZLinkApplicationJobContext.enter(first)) {
            serial.enqueue(() -> {
                assertEquals(1, queue.snapshot().queuedApplicationJobs());
                ZLinkApplicationJobContext.beforeFirstApplicationInstruction();
                assertEquals(0, queue.snapshot().permitsInUse());
                return handlerContinuation;
            });
        } finally {
            first.abandonReservation();
        }

        ZLinkApplicationJobQueue.Permit second =
            queue.acquire().toCompletableFuture().join();
        assertEquals(1, queue.snapshot().permitsInUse());
        assertFalse(handlerContinuation.isDone());
        second.close();
        handlerContinuation.complete(null);
    }

    @Test
    void transferredQueuedJobOutlivesItsParentCallAndReleasesExactlyOnce() {
        ZLinkApplicationJobQueue queue = queue(1);
        ZLinkApplicationJobContext.QueuedOwnership ownership;
        ZLinkApplicationJobQueue.Permit permit =
            queue.acquire().toCompletableFuture().join();

        try (var ignored = ZLinkApplicationJobContext.enter(permit)) {
            ownership = ZLinkApplicationJobContext.transferToQueuedJob();
        } finally {
            permit.abandonReservation();
        }
        assertTrue(ownership != null);

        //  The parent call has returned and its finally ran, yet the queued
        //  job still owns the permit (spec 33 §4/§8 — an asynchronous
        //  activation that outlives its parent call retains the admission it
        //  was handed; the post-handoff abandonReservation is a no-op).
        assertEquals(1, queue.snapshot().queuedApplicationJobs());
        assertEquals(1, queue.snapshot().permitsInUse());
        assertEquals(0, queue.snapshot().reservedSupplyPermits());
        CompletableFuture<ZLinkApplicationJobQueue.Permit> waiter =
            queue.acquire().toCompletableFuture();
        assertFalse(waiter.isDone());

        //  The later executor turn re-enters the transferred ownership and
        //  returns capacity immediately before the first application
        //  instruction, handing it to the parked waiter.
        try (var ignored = ZLinkApplicationJobContext.enterQueued(ownership)) {
            ZLinkApplicationJobContext.beforeFirstApplicationInstruction();
        }
        assertEquals(0, queue.snapshot().queuedApplicationJobs());
        assertTrue(waiter.isDone());

        //  Closing the ownership after handler entry cannot double-release:
        //  the waiter's permit remains the only one in use.
        ownership.close();
        assertEquals(1, queue.snapshot().permitsInUse());
        waiter.join().close();
        assertEquals(0, queue.snapshot().permitsInUse());
    }

    @Test
    void rejectedOrNonDispatchedReservationIsReturnedWithoutAHiddenBacklog() {
        ZLinkApplicationJobQueue queue = queue(1);
        ZLinkApplicationJobQueue.Permit permit =
            queue.acquire().toCompletableFuture().join();

        try (var ignored = ZLinkApplicationJobContext.enter(permit)) {
            assertTrue(ZLinkApplicationJobContext.current().isPresent());
        } finally {
            permit.abandonReservation();
        }

        assertEquals(0, queue.snapshot().permitsInUse());
        assertTrue(queue.acquire().toCompletableFuture().isDone());
    }

    @Test
    void commonJavaAndKotlinInvocationBoundaryReturnsCapacityAutomatically()
        throws Exception {
        ZLinkApplicationJobQueue queue = queue(1);
        ZLinkApplicationJobQueue.Permit permit =
            queue.acquire().toCompletableFuture().join();
        permit.queued();
        FirstInstructionHandler handler = new FirstInstructionHandler(queue);

        try (var ignored = ZLinkApplicationJobContext.enterQueued(permit)) {
            CompletableFuture<?> continuation = ZLinkHandlerMethodInvoker.invoke(
                handler,
                FirstInstructionHandler.class.getMethod("handle"),
                new Object[0]).toCompletableFuture();
            assertFalse(continuation.isDone());
        }

        assertEquals(0, queue.snapshot().permitsInUse());
        assertTrue(queue.acquire().toCompletableFuture().isDone());
    }

    private static ZLinkApplicationJobQueue queue(long limit) {
        return new ZLinkApplicationJobQueue(
            ZLinkApplicationJobQueueProfile.BALANCED,
            OptionalLong.of(limit),
            new ZLinkApplicationJobQueue.ProcessorCandidates(1, null, null, null));
    }

    public static final class FirstInstructionHandler {
        private final ZLinkApplicationJobQueue queue;

        public FirstInstructionHandler(ZLinkApplicationJobQueue queue) {
            this.queue = queue;
        }

        public CompletableFuture<Void> handle() {
            assertEquals(0, queue.snapshot().permitsInUse());
            return new CompletableFuture<>();
        }
    }
}
