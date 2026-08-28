package systems.zlink.framework.runtime.internal.dispatch;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.OptionalLong;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.execution.ZLinkExecutionLanePolicy;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker;

final class ZLinkApplicationJobQueueExecutionBoundaryTest {
    @Test
    void reservationCrossesSerialBacklogAndReturnsBeforeFirstApplicationInstruction() {
        ZLinkApplicationJobQueue queue = queue(1);
        ZLinkSerialExecutionQueue serial = new ZLinkSerialExecutionQueue(
            Runnable::run, ZLinkExecutionLanePolicy.generic());
        CompletableFuture<Void> handlerContinuation = new CompletableFuture<>();
        ZLinkApplicationJobQueue.Permit first = queue.acquire().toCompletableFuture().join();

        try (var ignored = ZLinkApplicationJobContext.enter(first)) {
            serial.enqueue(() -> {
                assertTrue(ZLinkApplicationJobContext.current().isPresent());
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
    void transferredIngressIsAccountedWithoutSerialCapacityRejection()
        throws Exception {
        ZLinkApplicationJobQueue applicationJobs = queue(1);
        ZLinkSerialExecutionQueue serial = new ZLinkSerialExecutionQueue(
            Runnable::run,
            ZLinkExecutionLanePolicy.generic(),
            1,
            4,
            1,
            4,
            4,
            1,
            java.time.Duration.ofSeconds(1));
        CompletableFuture<Void> firstStarted = new CompletableFuture<>();
        CompletableFuture<Void> firstActive = new CompletableFuture<>();
        CompletableFuture<Void> transferredStarted = new CompletableFuture<>();
        CompletableFuture<Void> transferredActive = new CompletableFuture<>();

        serial.enqueue(() -> {
            firstStarted.complete(null);
            return firstActive;
        });
        firstStarted.get();

        ZLinkApplicationJobQueue.Permit permit =
            applicationJobs.acquire().toCompletableFuture().join();
        CompletionStage<Void> transferred;
        try (var ignored = ZLinkApplicationJobContext.enter(permit)) {
            transferred = serial.enqueueWithPayloadBytes(0, () -> {
                ZLinkApplicationJobContext.beforeFirstApplicationInstruction();
                transferredStarted.complete(null);
                return transferredActive;
            });
        } finally {
            permit.abandonReservation();
        }
        assertFalse(transferred.toCompletableFuture().isCompletedExceptionally());

        firstActive.complete(null);
        transferredStarted.get();
        assertFalse(serial.tryEnqueue(
            () -> CompletableFuture.completedFuture(null)));

        transferredActive.complete(null);
        transferred.toCompletableFuture().get();
        serial.awaitQuiescence().toCompletableFuture().get();
        assertTrue(serial.tryEnqueue(
            () -> CompletableFuture.completedFuture(null)));
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
