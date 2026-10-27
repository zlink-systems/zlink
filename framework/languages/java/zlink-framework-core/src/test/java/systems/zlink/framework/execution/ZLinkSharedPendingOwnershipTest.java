package systems.zlink.framework.execution;

import static org.junit.jupiter.api.Assertions.*;
import static org.junit.jupiter.api.Assumptions.assumeTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue;

import java.lang.reflect.Field;
import java.util.ArrayDeque;
import java.util.Deque;
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
        var queue = new ZLinkSerialExecutionQueue(tasks::add, ZLinkExecutionLanePolicy.spot());
        new ZLinkSerialExecutionQueue.SharedSpotGate(queue);
        var pending = new PausingDeque();
        Field field = ZLinkSerialExecutionQueue.class.getDeclaredField("lifecyclePending");
        field.setAccessible(true);
        field.set(queue, pending);
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

    static Object read(Object owner, String name) throws Exception {
        Field field = owner.getClass().getDeclaredField(name);
        field.setAccessible(true);
        return field.get(owner);
    }

    private static void materialize(ZLinkSerialExecutionQueue queue) throws Exception {
        var method = ZLinkSerialExecutionQueue.class.getDeclaredMethod("materializePublished");
        method.setAccessible(true);
        method.invoke(queue);
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
                                            materialize(actor);
                                            var pending =
                                                    (Deque<?>) read(actor, "applicationPending");
                                            if (pending.size() != 2)
                                                throw new AssertionError("prefix not pending");
                                            terminals.get(1).cancel(false);
                                            if (pending.size() != 1)
                                                throw new AssertionError(
                                                        "tail cancellation not removed");
                                            var previous =
                                                    (ZLinkApplicationJobContext.QueuedOwnership)
                                                            read(
                                                                    pending.peekLast(),
                                                                    "applicationJobOwnership");
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
                                            var published =
                                                    (Deque<?>) read(actor, "publishedApplication");
                                            var posted =
                                                    (ZLinkApplicationJobContext.QueuedOwnership)
                                                            read(
                                                                    published.peekLast(),
                                                                    "applicationJobOwnership");
                                            assertTrue(
                                                    posted.isDurable(),
                                                    "inheritance is decided at publication");
                                            assertNull(
                                                    read(posted, "reservation"),
                                                    "publication settles the permit before gate import");
                                            materialize(actor);
                                            var next =
                                                    (ZLinkApplicationJobContext.QueuedOwnership)
                                                            read(
                                                                    pending.peekLast(),
                                                                    "applicationJobOwnership");
                                            assertNull(
                                                    read(next, "reservation"),
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
                                    ((Deque<?>) read(actor, "applicationPending")).isEmpty());
                            try (var scope = ZLinkApplicationJobContext.enter(held)) {
                                actor.enqueueRelocatable(
                                        new byte[] {2},
                                        () -> CompletableFuture.completedFuture(null),
                                        () -> {},
                                        null);
                            }
                            var published = (Deque<?>) read(actor, "publishedApplication");
                            var ownership =
                                    (ZLinkApplicationJobContext.QueuedOwnership)
                                            read(published.peekLast(), "applicationJobOwnership");
                            assertEquals(
                                    change == TailChange.IMPORT,
                                    ownership.isDurable(),
                                    "publication uses the tail after the owning gate mutation");
                            if (change == TailChange.IMPORT)
                                assertNull(
                                        read(ownership, "reservation"),
                                        "durable transfer returns the initial permit");
                            else
                                assertNotNull(
                                        read(ownership, "reservation"),
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
                                            try {
                                                materialize(actor);
                                            } catch (Exception failure) {
                                                throw new RuntimeException(failure);
                                            }
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

    private static final class PausingDeque extends ArrayDeque<Object> {
        final CountDownLatch iterating = new CountDownLatch(1);
        final CountDownLatch removed = new CountDownLatch(1);
        final CountDownLatch releaseGate = new CountDownLatch(1);
        final AtomicInteger removals = new AtomicInteger();

        @Override
        public Iterator<Object> iterator() {
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

                public Object next() {
                    return delegate.next();
                }
            };
        }

        @Override
        public Object removeFirst() {
            Object result = super.removeFirst();
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
