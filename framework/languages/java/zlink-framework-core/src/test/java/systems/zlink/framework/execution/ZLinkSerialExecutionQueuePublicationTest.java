package systems.zlink.framework.execution;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue;

import java.util.OptionalLong;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

final class ZLinkSerialExecutionQueuePublicationTest {
    @Test
    void admissionSignalObservesQueuedOwnership() throws Exception {
        try (var jobs =
                new ZLinkApplicationJobQueue(
                        ZLinkApplicationJobQueueProfile.BALANCED,
                        OptionalLong.of(10),
                        new ZLinkApplicationJobQueue.ProcessorCandidates(1, null, null, null))) {
            var tasks = new LinkedBlockingQueue<Runnable>();
            var serial = new ZLinkSerialExecutionQueue(tasks::add, ZLinkExecutionLanePolicy.spot());
            var admission = new CompletableFuture<Void>();
            var observed =
                    admission.thenRun(
                            () -> {
                                assertEquals(0, jobs.snapshot().reservedSupplyPermits());
                                assertEquals(1, jobs.snapshot().queuedApplicationJobs());
                            });
            try (var ignored =
                    ZLinkApplicationJobContext.enter(jobs.acquire().toCompletableFuture().join())) {
                serial.enqueueMessage(
                        new Object(),
                        0,
                        () -> CompletableFuture.completedFuture(null),
                        () -> {},
                        admission);
            }
            observed.get(5, TimeUnit.SECONDS);
            Runnable drain = tasks.poll(5, TimeUnit.SECONDS);
            assertTrue(drain != null);
            drain.run();
            serial.awaitQuiescence().toCompletableFuture().get(5, TimeUnit.SECONDS);
            assertEquals(0, jobs.snapshot().permitsInUse());
            serial.close();
        }
    }

    @Test
    void rejectedIngressKeepsItsReservationForAnotherOwner() throws Exception {
        try (var jobs =
                new ZLinkApplicationJobQueue(
                        ZLinkApplicationJobQueueProfile.BALANCED,
                        OptionalLong.of(10),
                        new ZLinkApplicationJobQueue.ProcessorCandidates(1, null, null, null))) {
            var tasks = new LinkedBlockingQueue<Runnable>();
            var oldOwner =
                    new ZLinkSerialExecutionQueue(tasks::add, ZLinkExecutionLanePolicy.spot());
            var newOwner =
                    new ZLinkSerialExecutionQueue(tasks::add, ZLinkExecutionLanePolicy.spot());
            oldOwner.sealClosingAdmission();
            var permit = jobs.acquire().toCompletableFuture().join();
            try (var ignored = ZLinkApplicationJobContext.enter(permit)) {
                assertFalse(oldOwner.tryEnqueue(() -> CompletableFuture.completedFuture(null)));
                assertTrue(ZLinkApplicationJobContext.hasTransferableQueuedOwnership());
                assertEquals(1, jobs.snapshot().reservedSupplyPermits());
                assertEquals(0, jobs.snapshot().queuedApplicationJobs());
                newOwner.enqueue(() -> CompletableFuture.completedFuture(null), null);
            }
            Runnable drain = tasks.poll(5, TimeUnit.SECONDS);
            assertTrue(drain != null);
            drain.run();
            newOwner.awaitQuiescence().toCompletableFuture().get(5, TimeUnit.SECONDS);
            assertEquals(0, jobs.snapshot().permitsInUse());
            oldOwner.close();
            newOwner.close();
        }
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void reservationPublicationAndIngressOwnershipDoNotHoldEachOthersOwner(boolean terminal)
            throws Exception {
        var jobs =
                new ZLinkApplicationJobQueue(
                        ZLinkApplicationJobQueueProfile.BALANCED,
                        OptionalLong.of(10),
                        new ZLinkApplicationJobQueue.ProcessorCandidates(1, null, null, null));
        var tasks = new LinkedBlockingQueue<Runnable>();
        var serial = new ZLinkSerialExecutionQueue(tasks::add, ZLinkExecutionLanePolicy.spot());
        var ingress = jobs.acquire().toCompletableFuture().join();
        var posting = new CountDownLatch(1);
        var proceed = new CountDownLatch(1);
        var posted = new CompletableFuture<Void>();
        var enqueued = new CompletableFuture<Void>();
        if (terminal) {
            try (var ignored = ZLinkApplicationJobContext.enter(ingress)) {
                serial.enqueue(() -> CompletableFuture.completedFuture(null), null);
            }
        }
        Thread.ofPlatform()
                .daemon(true)
                .start(
                        () -> {
                            try {
                                jobs.acquireAndPublish(
                                        task -> {
                                            posting.countDown();
                                            try {
                                                assertTrue(proceed.await(5, TimeUnit.SECONDS));
                                            } catch (InterruptedException interrupted) {
                                                Thread.currentThread().interrupt();
                                                throw new IllegalStateException(interrupted);
                                            }
                                            return serial.enqueue(
                                                    () -> {
                                                        task.run();
                                                        return CompletableFuture.completedFuture(
                                                                null);
                                                    },
                                                    null);
                                        },
                                        permit -> {
                                            permit.close();
                                            return CompletableFuture.completedFuture(null);
                                        });
                                posted.complete(null);
                            } catch (RuntimeException | Error failure) {
                                posted.completeExceptionally(failure);
                            }
                        });
        assertTrue(posting.await(5, TimeUnit.SECONDS));
        Thread receiver =
                Thread.ofPlatform()
                        .daemon(true)
                        .start(
                                () -> {
                                    try {
                                        if (terminal) {
                                            Runnable drain = tasks.poll(5, TimeUnit.SECONDS);
                                            assertTrue(drain != null);
                                            drain.run();
                                        } else {
                                            try (var ignored =
                                                    ZLinkApplicationJobContext.enter(ingress)) {
                                                serial.enqueue(
                                                        () ->
                                                                CompletableFuture.completedFuture(
                                                                        null),
                                                        null);
                                            }
                                        }
                                        enqueued.complete(null);
                                    } catch (InterruptedException interrupted) {
                                        Thread.currentThread().interrupt();
                                        enqueued.completeExceptionally(interrupted);
                                    } catch (RuntimeException | Error failure) {
                                        enqueued.completeExceptionally(failure);
                                    }
                                });
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (receiver.getState() != Thread.State.BLOCKED && System.nanoTime() < deadline) {
            Thread.onSpinWait();
        }
        assertEquals(Thread.State.BLOCKED, receiver.getState());
        proceed.countDown();
        posted.get(5, TimeUnit.SECONDS);
        enqueued.get(5, TimeUnit.SECONDS);
        var quiescent = serial.awaitQuiescence().toCompletableFuture();
        while (!quiescent.isDone()) {
            Runnable task = tasks.poll(5, TimeUnit.SECONDS);
            if (task != null) task.run();
            else quiescent.get(5, TimeUnit.SECONDS);
        }
        quiescent.get(5, TimeUnit.SECONDS);
        assertEquals(0, jobs.snapshot().permitsInUse());
        serial.close();
        jobs.close();
    }
}
