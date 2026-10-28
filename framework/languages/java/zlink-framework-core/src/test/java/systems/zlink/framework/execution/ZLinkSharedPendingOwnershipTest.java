package systems.zlink.framework.execution;

import static org.junit.jupiter.api.Assertions.*;
import static org.junit.jupiter.api.Assumptions.assumeTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile;
import systems.zlink.framework.runtime.internal.dispatch.QueuedOwnershipProbe;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue;

import java.util.ArrayDeque;
import java.util.Iterator;
import java.util.List;
import java.util.OptionalLong;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

final class ZLinkSharedPendingOwnershipTest {
    @Test
    void closeOnSharedGateControlTurnReportsIncompleteRelocationBoundary() throws Exception {
        assumeTrue(ZLinkSharedPendingOwnershipTest.class.desiredAssertionStatus());
        var tasks = new LinkedBlockingQueue<Runnable>();
        var spot = new ZLinkSerialExecutionQueue(tasks::add, ZLinkExecutionLanePolicy.spot());
        var gate = new ZLinkSerialExecutionQueue.SharedSpotGate(spot);
        var actor =
                new ZLinkSerialExecutionQueue(tasks::add, ZLinkExecutionLanePolicy.actorDelivery());
        actor.bindSharedSpotGate(gate);
        var reservation = gate.reserveBoundary(List.of(actor)).toCompletableFuture();
        tasks.poll(5, TimeUnit.SECONDS).run();
        var boundary = reservation.join().get(actor);
        try {
            var close =
                    gate.controlAsync(
                                    () -> {
                                        actor.close();
                                        return null;
                                    })
                            .toCompletableFuture();
            tasks.poll(5, TimeUnit.SECONDS).run();
            assertInstanceOf(
                    AssertionError.class,
                    assertThrows(CompletionException.class, close::join).getCause());
        } finally {
            boundary.release();
            tasks.poll(5, TimeUnit.SECONDS).run();
        }
        boundary.finished().toCompletableFuture().get(5, TimeUnit.SECONDS);
        actor.close();
        spot.close();
    }

    @Test
    void closeOutsideSharedGateReportsIncompleteRelocationBoundaryOnWorker() throws Exception {
        assumeTrue(ZLinkSharedPendingOwnershipTest.class.desiredAssertionStatus());
        var tasks = new LinkedBlockingQueue<Runnable>();
        var spot = new ZLinkSerialExecutionQueue(tasks::add, ZLinkExecutionLanePolicy.spot());
        var gate = new ZLinkSerialExecutionQueue.SharedSpotGate(spot);
        var actor =
                new ZLinkSerialExecutionQueue(tasks::add, ZLinkExecutionLanePolicy.actorDelivery());
        actor.bindSharedSpotGate(gate);
        var reservation = gate.reserveBoundary(List.of(actor)).toCompletableFuture();
        tasks.poll(5, TimeUnit.SECONDS).run();
        var boundary = reservation.join().get(actor);
        try {
            assertDoesNotThrow(actor::close);
            var workerTurn = tasks.poll(5, TimeUnit.SECONDS);
            assertNotNull(workerTurn);
            assertThrows(AssertionError.class, workerTurn::run);
        } finally {
            boundary.release();
            var cleanup = tasks.poll(5, TimeUnit.SECONDS);
            if (cleanup != null) cleanup.run();
        }
        boundary.finished().toCompletableFuture().get(5, TimeUnit.SECONDS);
        actor.close();
        spot.close();
    }

    @Test
    void closeWithFinishedSharedRelocationBoundaryDoesNotThrow() throws Exception {
        assumeTrue(ZLinkSharedPendingOwnershipTest.class.desiredAssertionStatus());
        var tasks = new LinkedBlockingQueue<Runnable>();
        var spot = new ZLinkSerialExecutionQueue(tasks::add, ZLinkExecutionLanePolicy.spot());
        var gate = new ZLinkSerialExecutionQueue.SharedSpotGate(spot);
        var actor =
                new ZLinkSerialExecutionQueue(tasks::add, ZLinkExecutionLanePolicy.actorDelivery());
        actor.bindSharedSpotGate(gate);
        var reservation = gate.reserveBoundary(List.of(actor)).toCompletableFuture();
        tasks.poll(5, TimeUnit.SECONDS).run();
        var boundary = reservation.join().get(actor);
        boundary.release();
        tasks.poll(5, TimeUnit.SECONDS).run();
        boundary.finished().toCompletableFuture().get(5, TimeUnit.SECONDS);
        assertDoesNotThrow(actor::close);
        actor.close();
        spot.close();
    }

    @Test
    void gateTakeNextInvalidatesProducerIterator() throws Exception {
        var tasks = new LinkedBlockingQueue<Runnable>();
        var pending = new PausingDeque();
        var queue =
                new ZLinkSerialExecutionQueue(
                        tasks::add, ZLinkExecutionLanePolicy.spot(), () -> pending);
        new ZLinkSerialExecutionQueue.SharedSpotGate(queue);
        queue.enqueueBarrierNext(() -> CompletableFuture.completedFuture(null), null);
        queue.enqueueBarrierNext(() -> CompletableFuture.completedFuture(null), null);
        var produced = new CompletableFuture<Void>();
        Thread producer =
                Thread.ofPlatform()
                        .daemon(true)
                        .start(
                                () -> {
                                    try {
                                        queue.enqueueLifecycleAdmission(
                                                () -> CompletableFuture.completedFuture(null));
                                        produced.complete(null);
                                        pending.iterating.countDown();
                                    } catch (Throwable failure) {
                                        produced.completeExceptionally(failure);
                                    }
                                });
        assertTrue(pending.iterating.await(5, TimeUnit.SECONDS));
        var drain = tasks.poll(5, TimeUnit.SECONDS);
        assertNotNull(drain);
        Thread gate = Thread.ofPlatform().daemon(true).start(drain);
        try {
            assertTrue(pending.removed.await(5, TimeUnit.SECONDS));
            produced.get(5, TimeUnit.SECONDS);
            assertEquals(1, pending.removals.get(), "actual takeNext removed a lifecycle head");
        } finally {
            pending.removed.countDown();
            pending.releaseGate.countDown();
            producer.join(5000);
            gate.join(5000);
        }
        assertFalse(producer.isAlive());
        assertFalse(gate.isAlive());
        queue.close();
    }

    @Test
    void cancelledDurableTailLeavesInheritanceFromSurvivingTail() throws Exception {
        var tasks = new LinkedBlockingQueue<Runnable>();
        var spot = new ZLinkSerialExecutionQueue(tasks::add, ZLinkExecutionLanePolicy.spot());
        var gate = new ZLinkSerialExecutionQueue.SharedSpotGate(spot);
        var actor =
                new ZLinkSerialExecutionQueue(tasks::add, ZLinkExecutionLanePolicy.actorDelivery());
        actor.bindSharedSpotGate(gate);
        try (var jobs =
                new ZLinkApplicationJobQueue(
                        ZLinkApplicationJobQueueProfile.BALANCED,
                        OptionalLong.of(1),
                        new ZLinkApplicationJobQueue.ProcessorCandidates(1, null, null, null))) {
            var held =
                    jobs.acquire(ZLinkApplicationJobQueue.Origin.REMOTE)
                            .toCompletableFuture()
                            .join();
            var checked =
                    actor.underSharedSpotGateAsync(
                                    () -> {
                                        try {
                                            var terminals =
                                                    actor.admitDurablePrefix(
                                                                    List.of(
                                                                            new ZLinkSerialExecutionQueue
                                                                                    .QueuedRecord(
                                                                                    1,
                                                                                    new byte[] {1}),
                                                                            new ZLinkSerialExecutionQueue
                                                                                    .QueuedRecord(
                                                                                    2,
                                                                                    new byte[] {
                                                                                        2
                                                                                    })),
                                                                    (record, terminal) ->
                                                                            () ->
                                                                                    CompletableFuture
                                                                                            .completedFuture(
                                                                                                    null),
                                                                    jobs)
                                                            .toCompletableFuture()
                                                            .join();
                                            actor.materializePublished();
                                            var pending = actor.applicationPendingSnapshot();
                                            if (pending.size() != 2)
                                                throw new AssertionError("prefix not pending");
                                            terminals.get(1).cancel(false);
                                            assertEquals(
                                                    2,
                                                    pending.size(),
                                                    "snapshot retains the original prefix");
                                            pending = actor.applicationPendingSnapshot();
                                            if (pending.size() != 1)
                                                throw new AssertionError(
                                                        "tail cancellation not removed");
                                            var previous = pending.getLast();
                                            if (!previous.isDurable())
                                                throw new AssertionError(
                                                        "surviving predecessor not durable");
                                            try (var scope =
                                                    ZLinkApplicationJobContext.enter(held)) {
                                                actor.enqueueRelocatable(
                                                        new byte[] {3},
                                                        () ->
                                                                CompletableFuture.completedFuture(
                                                                        null),
                                                        () -> {},
                                                        null);
                                            }
                                            var published = actor.publishedApplicationSnapshot();
                                            var posted = published.getLast();
                                            assertTrue(
                                                    posted.isDurable(),
                                                    "inheritance is decided at publication");
                                            assertFalse(
                                                    QueuedOwnershipProbe.hasReservation(posted),
                                                    "publication settles the permit before gate import");
                                            actor.materializePublished();
                                            var next = actor.applicationPendingSnapshot().getLast();
                                            assertFalse(
                                                    QueuedOwnershipProbe.hasReservation(next),
                                                    "inherited permit must be released");
                                            assertTrue(previous.isDurable());
                                            assertTrue(
                                                    next.isDurable(),
                                                    "C inherits A after B cancellation");
                                            terminals.getFirst().cancel(false);
                                            return null;
                                        } catch (Exception failure) {
                                            throw new RuntimeException(failure);
                                        }
                                    })
                            .toCompletableFuture();
            var task = tasks.poll(5, java.util.concurrent.TimeUnit.SECONDS);
            if (task == null) throw new AssertionError("gate not scheduled");
            task.run();
            checked.join();
            held.abandonReservation();
        }
    }

    @Test
    void cancellingLastPendingDurableClearsPublicationInheritance() throws Exception {
        verifyPublicationAfterTailChange(TailChange.CANCEL);
    }

    @Test
    void dequeuingLastPendingDurableClearsPublicationInheritance() throws Exception {
        verifyPublicationAfterTailChange(TailChange.DEQUEUE);
    }

    @Test
    void importedDurableTailTransfersOwnershipAtPublication() throws Exception {
        verifyPublicationAfterTailChange(TailChange.IMPORT);
    }

    private enum TailChange {
        IMPORT,
        CANCEL,
        DEQUEUE
    }

    private static void verifyPublicationAfterTailChange(TailChange change) throws Exception {
        var tasks = new LinkedBlockingQueue<Runnable>();
        var spot = new ZLinkSerialExecutionQueue(tasks::add, ZLinkExecutionLanePolicy.spot());
        var gate = new ZLinkSerialExecutionQueue.SharedSpotGate(spot);
        var actor =
                new ZLinkSerialExecutionQueue(tasks::add, ZLinkExecutionLanePolicy.actorDelivery());
        actor.bindSharedSpotGate(gate);
        try (var jobs =
                new ZLinkApplicationJobQueue(
                        ZLinkApplicationJobQueueProfile.BALANCED,
                        OptionalLong.of(2),
                        new ZLinkApplicationJobQueue.ProcessorCandidates(1, null, null, null))) {
            var held =
                    jobs.acquire(ZLinkApplicationJobQueue.Origin.REMOTE)
                            .toCompletableFuture()
                            .join();
            var checked = new CompletableFuture<Void>();
            Runnable publish =
                    () -> {
                        try {
                            assertEquals(
                                    change != TailChange.IMPORT,
                                    actor.applicationPendingSnapshot().isEmpty());
                            try (var scope = ZLinkApplicationJobContext.enter(held)) {
                                actor.enqueueRelocatable(
                                        new byte[] {2},
                                        () -> CompletableFuture.completedFuture(null),
                                        () -> {},
                                        null);
                            }
                            var published = actor.publishedApplicationSnapshot();
                            var ownership = published.getLast();
                            assertEquals(
                                    change == TailChange.IMPORT,
                                    ownership.isDurable(),
                                    "publication uses the tail after the owning gate mutation");
                            if (change == TailChange.IMPORT)
                                assertFalse(
                                        QueuedOwnershipProbe.hasReservation(ownership),
                                        "durable transfer returns the initial permit");
                            else
                                assertTrue(
                                        QueuedOwnershipProbe.hasReservation(ownership),
                                        "ordinary work retains its initial permit");
                            checked.complete(null);
                        } catch (Throwable failure) {
                            checked.completeExceptionally(failure);
                        }
                    };
            var admitted =
                    actor.underSharedSpotGateAsync(
                                    () -> {
                                        var terminals =
                                                actor.admitDurablePrefix(
                                                                List.of(
                                                                        new ZLinkSerialExecutionQueue
                                                                                .QueuedRecord(
                                                                                1, new byte[] {1})),
                                                                (record, terminal) ->
                                                                        () -> {
                                                                            if (change
                                                                                    == TailChange
                                                                                            .DEQUEUE)
                                                                                publish.run();
                                                                            return CompletableFuture
                                                                                    .completedFuture(
                                                                                            null);
                                                                        },
                                                                jobs)
                                                        .toCompletableFuture()
                                                        .join();
                                        if (change != TailChange.DEQUEUE) {
                                            actor.materializePublished();
                                            if (change == TailChange.CANCEL)
                                                terminals.getFirst().cancel(false);
                                            publish.run();
                                        }
                                        return null;
                                    })
                            .toCompletableFuture();
            var task = tasks.poll(5, TimeUnit.SECONDS);
            assertNotNull(task);
            task.run();
            admitted.join();
            checked.get(5, TimeUnit.SECONDS);
            actor.close();
            spot.close();
            held.abandonReservation();
        }
    }

    private static final class PausingDeque extends ArrayDeque<ZLinkSerialExecutionQueue.Entry> {
        final CountDownLatch iterating = new CountDownLatch(1);
        final CountDownLatch removed = new CountDownLatch(1);
        final CountDownLatch releaseGate = new CountDownLatch(1);
        final AtomicInteger removals = new AtomicInteger();

        @Override
        public Iterator<ZLinkSerialExecutionQueue.Entry> iterator() {
            var delegate = super.iterator();
            return new Iterator<>() {
                boolean first = true;

                public boolean hasNext() {
                    boolean more = delegate.hasNext();
                    if (first && more) {
                        first = false;
                        iterating.countDown();
                        try {
                            if (!removed.await(5, TimeUnit.SECONDS))
                                throw new AssertionError("gate did not take head");
                        } catch (InterruptedException failure) {
                            throw new AssertionError(failure);
                        }
                    }
                    return more;
                }

                public ZLinkSerialExecutionQueue.Entry next() {
                    return delegate.next();
                }
            };
        }

        @Override
        public ZLinkSerialExecutionQueue.Entry removeFirst() {
            var result = super.removeFirst();
            removals.incrementAndGet();
            removed.countDown();
            try {
                if (!releaseGate.await(5, TimeUnit.SECONDS))
                    throw new AssertionError("test did not release gate");
            } catch (InterruptedException failure) {
                throw new AssertionError(failure);
            }
            return result;
        }
    }
}
