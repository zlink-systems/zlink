package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.ZLinkCompletionBridge;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executors;
import java.util.concurrent.ForkJoinPool;
import java.util.concurrent.ForkJoinTask;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.locks.LockSupport;

final class ZLinkServiceOperationRegistryTest {
    @Test
    void replyTakenBeforeDispatchCannotBeOverwrittenByCallerCancellation() throws Exception {
        try (var registry = new ZLinkServiceOperationRegistry(new ManualMaintenance())) {
            UUID id = ZLinkServiceOperationIds.next();
            var result =
                    registry.<String>submitTerminal(
                            id, Duration.ofHours(1), terminal -> null, ignored -> {});
            int workers = ForkJoinPool.getCommonPoolParallelism();
            var occupied = new CountDownLatch(workers);
            var release = new CountDownLatch(1);
            for (int worker = 0; worker < workers; worker++)
                ForkJoinPool.commonPool()
                        .execute(
                                () -> {
                                    occupied.countDown();
                                    await(release);
                                });
            try {
                assertTrue(occupied.await(5, TimeUnit.SECONDS));
                assertTrue(registry.complete(id, "reply"));
                assertEquals(0, registry.pendingCount());
                assertFalse(result.isDone(), "dispatcher delivery is held by the occupied pool");
                assertFalse(result.cancel(false), "the reply already took C1 terminal authority");
            } finally {
                release.countDown();
            }
            assertEquals("reply", result.get(5, TimeUnit.SECONDS));
        }
    }

    @Test
    void cancellationWhileOwnerRegistrationIsQueuedPreventsSubmission() throws Exception {
        try (var registry = new ZLinkServiceOperationRegistry(new ManualMaintenance())) {
            var entered = new CountDownLatch(1);
            var release = new CountDownLatch(1);
            var lane = ownerLane(registry);
            var submissions = new AtomicInteger();
            lane.runAsync(
                    () -> {
                        entered.countDown();
                        await(release);
                    });
            try {
                assertTrue(entered.await(5, TimeUnit.SECONDS));
                var receiver =
                        ZLinkServiceOperationRegistry.<String>prepareOperation(
                                ZLinkServiceOperationIds.next());
                var registration =
                        lane.runAsync(
                                () ->
                                        registry.submitTerminal(
                                                receiver.id(),
                                                Duration.ofHours(1),
                                                terminal -> {
                                                    submissions.incrementAndGet();
                                                    return null;
                                                },
                                                ignored -> {},
                                                receiver));
                assertTrue(receiver.cancel(false));
                release.countDown();
                registration.toCompletableFuture().get(5, TimeUnit.SECONDS);
                assertTrue(receiver.isCancelled());
                assertEquals(0, submissions.get());
                assertEquals(0, registry.pendingCount());
            } finally {
                release.countDown();
            }
        }
    }

    @Test
    void cancellationDuringRegistrationHandoffPreventsSubmission() throws Exception {
        var entered = new CountDownLatch(1);
        var release = new CountDownLatch(1);
        var armed = new java.util.concurrent.atomic.AtomicBoolean();
        var submissions = new AtomicInteger();
        try (var registry =
                new ZLinkServiceOperationRegistry(
                        new ManualMaintenance(),
                        new IllegalStateException("closed"),
                        () -> {
                            if (armed.get()
                                    && StackWalker.getInstance()
                                            .walk(
                                                    frames ->
                                                            frames.anyMatch(
                                                                    frame ->
                                                                            frame.getMethodName()
                                                                                    .equals(
                                                                                            "registerEntry")))) {
                                entered.countDown();
                                await(release);
                            }
                            return 0;
                        })) {
            var receiver =
                    ZLinkServiceOperationRegistry.<String>prepareOperation(
                            ZLinkServiceOperationIds.next());
            armed.set(true);
            var registration =
                    ownerLane(registry)
                            .runAsync(
                                    () ->
                                            registry.submitTerminal(
                                                    receiver.id(),
                                                    Duration.ofHours(1),
                                                    terminal -> {
                                                        submissions.incrementAndGet();
                                                        return null;
                                                    },
                                                    ignored -> {},
                                                    receiver));
            try {
                assertTrue(entered.await(5, TimeUnit.SECONDS));
                assertTrue(receiver.cancel(false), "handoff does not drop cancellation");
            } finally {
                release.countDown();
            }
            registration.toCompletableFuture().get(5, TimeUnit.SECONDS);
            assertTrue(receiver.isCancelled());
            assertEquals(0, submissions.get());
            assertEquals(0, registry.pendingCount());
            assertEquals(1, wheelEntryCount(registry));
            ZLinkCompletionBridge.await(registry.expire(TimeUnit.MILLISECONDS.toNanos(256)));
            assertEquals(0, wheelEntryCount(registry));
        } finally {
            release.countDown();
        }
    }

    @Test
    void rejectedOwnerRegistrationCompletesThePreparedReceiver() throws Exception {
        var registry = new ZLinkServiceOperationRegistry(new ManualMaintenance());
        {
            ownerLane(registry).closeAsync().toCompletableFuture().get(5, TimeUnit.SECONDS);
            var receiver =
                    ZLinkServiceOperationRegistry.<String>prepareOperation(
                            ZLinkServiceOperationIds.next());
            registry.submitTerminal(
                    receiver.id(), Duration.ofHours(1), terminal -> null, ignored -> {}, receiver);
            var failure =
                    assertThrows(
                            java.util.concurrent.ExecutionException.class,
                            () -> receiver.get(5, TimeUnit.SECONDS));
            assertInstanceOf(IllegalStateException.class, failure.getCause());
        }
    }

    @Test
    void hugeTimeoutDoesNotExpireWhenTheRelativeNanosecondDeadlineWraps() {
        AtomicLong clock = new AtomicLong();
        try (var registry =
                new ZLinkServiceOperationRegistry(
                        new ManualMaintenance(), new IllegalStateException("closed"), clock::get)) {
            clock.set(TimeUnit.MILLISECONDS.toNanos(1));
            var request =
                    ZLinkCompletionBridge.await(
                            registry.register(Duration.ofSeconds(Long.MAX_VALUE)));
            clock.set(TimeUnit.MILLISECONDS.toNanos(257));
            assertEquals(0, ZLinkCompletionBridge.await(registry.expire(clock.get())));
            assertFalse(request.isDone());
            assertEquals(1, registry.pendingCount());
        }
    }

    @Test
    void cancellationRemovesOneThousandPendingRequestsWithoutOwnerCleanupTurns() {
        var maintenance = new ManualMaintenance();
        try (var registry = new ZLinkServiceOperationRegistry(maintenance)) {
            var requests = new ArrayList<ZLinkServiceOperationRegistry.Operation<Object>>();
            for (int request = 0; request < 1_000; request++)
                requests.add(ZLinkCompletionBridge.await(registry.register(Duration.ofHours(1))));
            assertEquals(1_000, registry.pendingCount());
            for (var request : requests) assertTrue(request.cancel(false));
            assertEquals(0, registry.pendingCount());
            assertEquals(1, maintenance.starts);
        }
    }

    @Test
    void retainedReservationDoesNotReadTheClockAgain() throws Exception {
        AtomicLong clock = new AtomicLong();
        AtomicInteger reads = new AtomicInteger();
        var maintenance = new ManualMaintenance();
        try (var registry =
                new ZLinkServiceOperationRegistry(
                        maintenance,
                        new IllegalStateException("closed"),
                        () -> {
                            reads.incrementAndGet();
                            return clock.get();
                        })) {
            var first = ZLinkCompletionBridge.await(registry.register(Duration.ofSeconds(30)));
            assertTrue(registry.complete(first.id(), 1));
            int before = reads.get();
            var second = ZLinkCompletionBridge.await(registry.register(Duration.ofSeconds(30)));
            assertEquals(
                    2,
                    reads.get() - before,
                    "deadline capture and index insertion read time; a retained visit needs no"
                            + " clock read");
            assertTrue(registry.complete(second.id(), 2));
        }
    }

    @Test
    void registrationKeepsDeadlineWhileWaitingForOwner() throws Exception {
        AtomicLong clock = new AtomicLong();
        var maintenance = new ManualMaintenance();
        var registry =
                new ZLinkServiceOperationRegistry(
                        maintenance, new IllegalStateException("closed"), clock::get);
        CountDownLatch entered = new CountDownLatch(1);
        CountDownLatch release = new CountDownLatch(1);
        var lane = ownerLane(registry);
        lane.runAsync(
                () -> {
                    entered.countDown();
                    await(release);
                });
        try {
            assertTrue(entered.await(1, TimeUnit.SECONDS));
            var registered = registry.register(Duration.ofMillis(10));
            clock.set(TimeUnit.MILLISECONDS.toNanos(20));
            release.countDown();
            var request = ZLinkCompletionBridge.await(registered);
            assertEquals(1, ZLinkCompletionBridge.await(registry.expire(clock.get())));
            assertThrows(CompletionException.class, request::join);
            assertEquals(0, registry.pendingCount());
        } finally {
            release.countDown();
            registry.close();
        }
    }

    @Test
    void longDeadlineSlotsDoNotAdvanceManagementVisits() throws Exception {
        AtomicLong clock = new AtomicLong();
        var maintenance = new ManualMaintenance();
        try (var registry =
                new ZLinkServiceOperationRegistry(
                        maintenance, new IllegalStateException("closed"), clock::get)) {
            for (int request = 0; request < 1_000; request++)
                ZLinkCompletionBridge.await(registry.register(Duration.ofMillis(10_000 + request)));
            assertEquals(256, registry.maintenanceTickForTest());
            clock.set(TimeUnit.MILLISECONDS.toNanos(256));
            assertEquals(0, ZLinkCompletionBridge.await(registry.expire(clock.get())));
            assertEquals(1_000, registry.pendingCount());
        }
    }

    @Test
    void completedLongDeadlineEntriesLeaveTheIndexWithinOneRotation() throws Exception {
        AtomicLong clock = new AtomicLong();
        var maintenance = new ManualMaintenance();
        try (var registry =
                new ZLinkServiceOperationRegistry(
                        maintenance, new IllegalStateException("closed"), clock::get)) {
            for (int request = 0; request < 1_000; request++) {
                var operation =
                        ZLinkCompletionBridge.await(
                                registry.register(Duration.ofMillis(10_000 + request)));
                assertTrue(registry.complete(operation.id(), request));
            }
            assertEquals(0, registry.pendingCount());
            clock.set(TimeUnit.MILLISECONDS.toNanos(256));
            ZLinkCompletionBridge.await(registry.expire(clock.get()));
            for (Object entry : registry.deadlineHeadsForTest()) assertNull(entry);
        }
    }

    @Test
    void repeatedCloseReturnsWhileTheFirstCloseJoinsMaintenance() throws Exception {
        CountDownLatch workerExited = new CountDownLatch(1);
        CountDownLatch releaseWorker = new CountDownLatch(1);
        CountDownLatch repeatedCloseReturned = new CountDownLatch(1);
        AtomicReference<Thread> worker = new AtomicReference<>();
        var registry =
                new ZLinkServiceOperationRegistry(
                        command -> {
                            Thread thread =
                                    Thread.ofPlatform()
                                            .unstarted(
                                                    () -> {
                                                        command.run();
                                                        workerExited.countDown();
                                                        await(releaseWorker);
                                                    });
                            worker.set(thread);
                            return thread;
                        });
        ZLinkCompletionBridge.await(registry.register(Duration.ofHours(1)));
        Thread firstClose = Thread.ofPlatform().start(registry::close);
        Thread repeatedClose = null;
        try {
            assertTrue(workerExited.await(1, TimeUnit.SECONDS));
            repeatedClose =
                    Thread.ofPlatform()
                            .start(
                                    () -> {
                                        registry.close();
                                        repeatedCloseReturned.countDown();
                                    });
            assertTrue(repeatedCloseReturned.await(1, TimeUnit.SECONDS));
            assertTrue(firstClose.isAlive());
            assertEquals(0, registry.pendingCount());
            assertThrows(
                    IllegalStateException.class,
                    () -> ZLinkCompletionBridge.await(registry.register(Duration.ofSeconds(1))));
        } finally {
            releaseWorker.countDown();
            firstClose.join();
            if (repeatedClose != null) repeatedClose.join();
            registry.close();
        }
        assertEquals(Thread.State.TERMINATED, worker.get().getState());
    }

    @Test
    void parkedWorkerWithQueuedVisitIsNotIndefinitelyIdle() throws Exception {
        var worker = new AtomicReference<Thread>();
        var clock = new AtomicLong();
        var entered = new CountDownLatch(1);
        var release = new CountDownLatch(1);
        try (var registry =
                new ZLinkServiceOperationRegistry(
                        command -> {
                            var thread = Thread.ofVirtual().unstarted(command);
                            worker.set(thread);
                            return thread;
                        },
                        new IllegalStateException("closed"),
                        clock::get)) {
            var occupied =
                    CompletableFuture.runAsync(
                            () ->
                                    registry.ownerLaneForTest()
                                            .awaitTurn(
                                                    () -> {
                                                        var operation =
                                                                ZLinkCompletionBridge.await(
                                                                        registry.register(
                                                                                Duration.ofMillis(
                                                                                        1)));
                                                        clock.set(TimeUnit.MILLISECONDS.toNanos(2));
                                                        assertTrue(
                                                                registry.discard(operation.id()));
                                                        entered.countDown();
                                                        await(release);
                                                        return null;
                                                    }));
            try {
                assertTrue(entered.await(5, TimeUnit.SECONDS));
                long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
                while (registry.maintenanceTickForTest() != -2
                        || worker.get().getState() != Thread.State.WAITING
                        || java.util.concurrent.locks.LockSupport.getBlocker(worker.get())
                                != registry) {
                    if (System.nanoTime() - deadline >= 0)
                        throw new AssertionError("visit was not queued");
                    Thread.yield();
                }
                assertFalse(
                        isParkedIndefinitely(worker.get(), registry),
                        "a queued owner visit must finish before idle clock reads are sampled");
            } finally {
                release.countDown();
                occupied.get(5, TimeUnit.SECONDS);
            }
            parkedIndefinitely(worker.get(), registry);
        }
    }

    @Test
    void maintenanceParksWithoutClockPollingAndCloseJoinsItsWorker() throws Exception {
        AtomicReference<Thread> worker = new AtomicReference<>();
        AtomicLong clock = new AtomicLong();
        AtomicInteger clockReads = new AtomicInteger();
        var registry =
                new ZLinkServiceOperationRegistry(
                        command -> {
                            Thread thread = Thread.ofVirtual().unstarted(command);
                            assertTrue(worker.compareAndSet(null, thread));
                            return thread;
                        },
                        new IllegalStateException("closed"),
                        () -> {
                            clockReads.incrementAndGet();
                            return clock.get();
                        });
        var lane = ownerLane(registry);
        try (registry) {
            assertNull(worker.get());
            lane.awaitTurn(
                    () -> {
                        var first =
                                ZLinkCompletionBridge.await(
                                        registry.register(Duration.ofMillis(1)));
                        clock.set(TimeUnit.MILLISECONDS.toNanos(2));
                        assertTrue(registry.discard(first.id()));
                        return null;
                    });
            parkedIndefinitely(worker.get(), registry);
            int idleReads = clockReads.get();
            clock.addAndGet(TimeUnit.SECONDS.toNanos(1));
            assertEquals(idleReads, clockReads.get());
            var second =
                    lane.awaitTurn(
                            () -> {
                                var operation =
                                        ZLinkCompletionBridge.await(
                                                registry.register(Duration.ofMillis(1)));
                                clock.addAndGet(TimeUnit.MILLISECONDS.toNanos(2));
                                return operation;
                            });
            assertTrue(
                    assertThrows(
                                            java.util.concurrent.ExecutionException.class,
                                            () -> second.get(1, TimeUnit.SECONDS))
                                    .getCause()
                            instanceof TimeoutException);
        }
        assertEquals(Thread.State.TERMINATED, worker.get().getState());
    }

    @Test
    void earlierOccupiedTickWakesTheExistingMaintenanceWait() throws Exception {
        AtomicReference<Thread> worker = new AtomicReference<>();
        AtomicLong clock = new AtomicLong();
        CountDownLatch firstWait = new CountDownLatch(1);
        CountDownLatch nextWait = new CountDownLatch(1);
        AtomicReference<CountDownLatch> observed = new AtomicReference<>(firstWait);
        var registry =
                new ZLinkServiceOperationRegistry(
                        command -> {
                            Thread thread = Thread.ofVirtual().unstarted(command);
                            assertTrue(worker.compareAndSet(null, thread));
                            return thread;
                        },
                        new IllegalStateException("closed"),
                        () -> {
                            if (Thread.currentThread() == worker.get()) observed.get().countDown();
                            return clock.get();
                        });
        var lane = ownerLane(registry);
        try (registry) {
            var later = ZLinkCompletionBridge.await(registry.register(Duration.ofMillis(255)));
            assertTrue(firstWait.await(1, TimeUnit.SECONDS));
            var earlier =
                    lane.awaitTurn(
                            () -> {
                                observed.set(nextWait);
                                var operation =
                                        ZLinkCompletionBridge.await(
                                                registry.register(Duration.ofNanos(1)));
                                clock.set(TimeUnit.MILLISECONDS.toNanos(2));
                                return operation;
                            });
            assertTrue(nextWait.await(1, TimeUnit.SECONDS));
            assertTrue(
                    assertThrows(
                                            java.util.concurrent.ExecutionException.class,
                                            () -> earlier.get(1, TimeUnit.SECONDS))
                                    .getCause()
                            instanceof TimeoutException);
            assertFalse(later.isDone());
            assertTrue(registry.discard(later.id()));
        }
        assertEquals(Thread.State.TERMINATED, worker.get().getState());
    }

    private static void awaitWorkerState(Thread worker, Thread.State expected) {
        long limit = System.nanoTime() + TimeUnit.SECONDS.toNanos(1);
        while (worker.getState() != expected && System.nanoTime() < limit) Thread.onSpinWait();
        assertEquals(expected, worker.getState());
    }

    @Test
    void requestAfterLongIdleDoesNotReplayEmptyWheelRevolutions() {
        AtomicLong clock = new AtomicLong();
        var maintenance = new ManualMaintenance();
        try (var registry =
                new ZLinkServiceOperationRegistry(
                        maintenance,
                        new IllegalStateException("closed"),
                        () -> clock.getAndAdd(TimeUnit.MICROSECONDS.toNanos(10)))) {
            var first = ZLinkCompletionBridge.await(registry.register(Duration.ofMillis(1)));
            assertTrue(registry.discard(first.id()));
            clock.set(Duration.ofDays(365).toNanos());
            var next = ZLinkCompletionBridge.await(registry.register(Duration.ofMillis(1)));
            clock.addAndGet(TimeUnit.MILLISECONDS.toNanos(2));
            assertEquals(1, ZLinkCompletionBridge.await(registry.expire(clock.get())));
            assertTrue(
                    assertThrows(CompletionException.class, next::join).getCause()
                            instanceof TimeoutException);
            assertEquals(1, maintenance.starts);
        }
    }

    @Test
    void interruptedCloseStillJoinsMaintenanceAndRestoresInterruption() throws Exception {
        AtomicReference<Thread> worker = new AtomicReference<>();
        try (var registry =
                new ZLinkServiceOperationRegistry(
                        command -> {
                            Thread thread = Thread.ofVirtual().unstarted(command);
                            worker.set(thread);
                            return thread;
                        })) {
            ZLinkCompletionBridge.await(registry.register(Duration.ofHours(1)));
            Thread.currentThread().interrupt();
            try {
                registry.close();
                assertTrue(Thread.currentThread().isInterrupted());
                assertEquals(Thread.State.TERMINATED, worker.get().getState());
            } finally {
                Thread.interrupted();
            }
        }
    }

    @Test
    void maintenanceStartFailureLeavesNoPendingOperation() {
        var rejected = new IllegalStateException("worker rejected");
        try (var registry =
                new ZLinkServiceOperationRegistry(
                        command -> {
                            return new Thread(command) {
                                @Override
                                public synchronized void start() {
                                    throw rejected;
                                }
                            };
                        })) {
            assertSame(
                    rejected,
                    assertThrows(
                            IllegalStateException.class,
                            () ->
                                    ZLinkCompletionBridge.await(
                                            registry.register(Duration.ofSeconds(1)))));
            assertEquals(0, registry.pendingCount());
        }
    }

    @Test
    void requestMaintenanceUsesOneWorkerWithoutScheduledTimers() {
        var maintenance = new ManualMaintenance();
        try (var registry = new ZLinkServiceOperationRegistry(maintenance)) {
            var request = ZLinkCompletionBridge.await(registry.register(Duration.ofHours(1)));
            assertEquals(1, maintenance.starts);
            registry.discard(request.id());
        }
    }

    @Test
    void sendTerminalDoesNotDetachAPendingRequestDeadlineBucket() {
        AtomicLong clock = new AtomicLong();
        var maintenance = new ManualMaintenance();
        try (var registry =
                new ZLinkServiceOperationRegistry(
                        maintenance, new IllegalStateException("closed"), clock::get)) {
            var binding = new CompletableFuture<Void>();
            var send = registry.submitSend(() -> binding);
            // The first full wheel revolution shares slot zero with an unindexed SEND slot.
            Duration timeout = Duration.ofMillis(256);
            var request = ZLinkCompletionBridge.await(registry.register(timeout));
            binding.complete(null);
            assertNull(send.toCompletableFuture().join());
            assertEquals(1, registry.pendingCount());
            assertEquals(1, ZLinkCompletionBridge.await(registry.expire(timeout.toNanos())));
            assertTrue(
                    assertThrows(CompletionException.class, request::join).getCause()
                            instanceof TimeoutException);
            assertEquals(0, registry.pendingCount());
        }
    }

    @Test
    void sendOnlyRegistryNeverSchedulesMaintenance() throws Exception {
        var maintenance = new ManualMaintenance();
        try (var registry = new ZLinkServiceOperationRegistry(maintenance)) {
            assertEquals(0, maintenance.starts);
            var binding = new CompletableFuture<Void>();
            var send = registry.submitSend(() -> binding);
            ownerLane(registry).runAsync(() -> {}).toCompletableFuture().get(1, TimeUnit.SECONDS);
            assertEquals(1, registry.pendingCount());
            assertEquals(0, maintenance.starts);
            binding.complete(null);
            assertNull(send.toCompletableFuture().join());
            assertEquals(0, maintenance.starts);
        }
        assertEquals(0, maintenance.starts);
    }

    @Test
    void firstRequestStartsMaintenanceOnceAcrossIdlePeriods() {
        var maintenance = new ManualMaintenance();
        AtomicLong clock = new AtomicLong(100);
        var registry =
                new ZLinkServiceOperationRegistry(
                        maintenance, new IllegalStateException("closed"), clock::get);
        try (registry) {
            assertEquals(0, maintenance.starts);
            var first = ZLinkCompletionBridge.await(registry.register(Duration.ofNanos(10)));
            assertEquals(1, maintenance.starts);
            clock.set(109);
            ZLinkCompletionBridge.await(registry.expire(clock.get()));
            assertFalse(first.isDone());
            clock.set(110);
            ZLinkCompletionBridge.await(registry.expire(clock.get()));
            assertFalse(first.isDone());
            clock.set(100 + TimeUnit.MILLISECONDS.toNanos(1));
            ZLinkCompletionBridge.await(registry.expire(clock.get()));
            assertTrue(
                    assertThrows(CompletionException.class, first::join).getCause()
                            instanceof TimeoutException);
            var second = ZLinkCompletionBridge.await(registry.register(Duration.ofNanos(10)));
            assertEquals(1, maintenance.starts);
            assertTrue(registry.discard(second.id()));
        }
        assertEquals(1, maintenance.starts);
        assertThrows(
                IllegalStateException.class,
                () -> ZLinkCompletionBridge.await(registry.register(Duration.ofSeconds(1))));
        assertEquals(1, maintenance.starts);
    }

    @Test
    void sendIgnoresRequestDeadlineAndCallerCancellation() {
        AtomicLong clock = new AtomicLong(100);
        var maintenance = new ManualMaintenance();
        try (var registry =
                new ZLinkServiceOperationRegistry(
                        maintenance, new IllegalStateException("closed"), clock::get)) {
            var binding = new CompletableFuture<Void>();
            var send = registry.submitSend(() -> binding);
            var request = ZLinkCompletionBridge.await(registry.register(Duration.ofNanos(10)));
            assertTrue(send.toCompletableFuture().cancel(false));
            assertFalse(binding.isDone());
            assertEquals(2, registry.pendingCount());
            clock.set(110);
            assertEquals(0, ZLinkCompletionBridge.await(registry.expire(clock.get())));
            clock.set(100 + TimeUnit.MILLISECONDS.toNanos(1));
            assertEquals(1, ZLinkCompletionBridge.await(registry.expire(clock.get())));
            assertTrue(
                    assertThrows(CompletionException.class, request::join).getCause()
                            instanceof TimeoutException);
            assertEquals(1, registry.pendingCount());
            assertFalse(send.toCompletableFuture().isDone());
            var cancelledRequest =
                    ZLinkCompletionBridge.await(registry.register(Duration.ofNanos(10)));
            assertTrue(cancelledRequest.cancel(false));
            assertThrows(CancellationException.class, cancelledRequest::join);
            assertEquals(1, registry.pendingCount());
            binding.complete(null);
            assertNull(send.toCompletableFuture().join());
            assertEquals(0, registry.pendingCount());
        }
    }

    @Test
    void closeBetweenRegistrationAndObserverWinsAgainstAlreadyCompletedAdmission()
            throws Exception {
        var closeFailure = new IllegalStateException("closed");
        var maintenance = new ManualMaintenance();
        try (var registry = new ZLinkServiceOperationRegistry(maintenance, closeFailure)) {
            var send =
                    registry.submitSend(
                            () -> {
                                assertEquals(1, registry.pendingCount());
                                registry.close();
                                return CompletableFuture.completedFuture(null);
                            });
            assertSame(
                    closeFailure,
                    assertThrows(CompletionException.class, () -> send.toCompletableFuture().join())
                            .getCause());
            assertEquals(0, registry.pendingCount());
        }
    }

    @Test
    void lateSendTerminalCannotCompleteAnotherRegistration() throws Exception {
        var maintenance = new ManualMaintenance();
        try (var registry = new ZLinkServiceOperationRegistry(maintenance)) {
            var oldBinding = new CompletableFuture<Void>();
            var old = registry.submitSend(() -> oldBinding);
            ownerLane(registry).runAsync(() -> {}).toCompletableFuture().get(1, TimeUnit.SECONDS);
            var oldId = registry.pendingIdsForTest().iterator().next();
            assertTrue(registry.discard(oldId));
            var nextBinding = new CompletableFuture<Void>();
            var next = registry.submitSend(() -> nextBinding);
            ownerLane(registry).runAsync(() -> {}).toCompletableFuture().get(1, TimeUnit.SECONDS);
            assertNotEquals(oldId, registry.pendingIdsForTest().iterator().next());
            oldBinding.complete(null);
            assertEquals(1, registry.pendingCount());
            assertFalse(next.toCompletableFuture().isDone());
            assertFalse(old.toCompletableFuture().isDone());
            nextBinding.complete(null);
            assertNull(next.toCompletableFuture().join());
            assertEquals(0, registry.pendingCount());
        }
    }

    @Test
    void sendTerminalCallbackRunsOnDispatcherOutsideOwnerTurn() throws Exception {
        var maintenance = new ManualMaintenance();
        try (var registry = new ZLinkServiceOperationRegistry(maintenance)) {
            var binding = new CompletableFuture<Void>();
            var send = registry.submitSend(() -> binding);
            var lane = ownerLane(registry);
            var callback =
                    send.thenRun(
                            () -> {
                                assertSame(ForkJoinPool.commonPool(), ForkJoinTask.getPool());
                                assertTrue(ZLinkServiceCompletionDispatcher.isCurrentCompletion());
                                assertFalse(lane.isOnLane());
                                var request =
                                        ZLinkCompletionBridge.await(
                                                registry.register(Duration.ofSeconds(1)));
                                assertTrue(registry.discard(request.id()));
                            });
            binding.complete(null);
            callback.toCompletableFuture().get(5, TimeUnit.SECONDS);
        }
    }

    @Test
    void closeAndBindingTerminalRaceCompletesEachSendExactlyOnce() throws Exception {
        var maintenance = new ManualMaintenance();
        try (var workers = Executors.newFixedThreadPool(2)) {
            for (int index = 0; index < 32; index++) {
                var closeFailure = new IllegalStateException("closed");
                var routeLoss = new IllegalStateException("route lost");
                boolean failAdmission = index % 2 == 0;
                try (var registry = new ZLinkServiceOperationRegistry(maintenance, closeFailure)) {
                    var binding = new CompletableFuture<Void>();
                    var send = registry.submitSend(() -> binding);
                    var callbacks = new AtomicInteger();
                    var observed =
                            send.handle(
                                    (ignored, failure) -> {
                                        callbacks.incrementAndGet();
                                        return failure;
                                    });
                    var start = new java.util.concurrent.CyclicBarrier(3);
                    var closing =
                            workers.submit(
                                    () -> {
                                        start.await(5, TimeUnit.SECONDS);
                                        registry.close();
                                        return null;
                                    });
                    var admitting =
                            workers.submit(
                                    () -> {
                                        start.await(5, TimeUnit.SECONDS);
                                        if (failAdmission) binding.completeExceptionally(routeLoss);
                                        else binding.complete(null);
                                        return null;
                                    });
                    start.await(5, TimeUnit.SECONDS);
                    closing.get(5, TimeUnit.SECONDS);
                    admitting.get(5, TimeUnit.SECONDS);
                    var failure = observed.toCompletableFuture().get(5, TimeUnit.SECONDS);
                    if (failure != null) {
                        var terminal =
                                org.junit.jupiter.api.Assertions.assertInstanceOf(
                                                CompletionException.class, failure)
                                        .getCause();
                        assertTrue(
                                terminal == closeFailure
                                        || (failAdmission && terminal == routeLoss));
                    } else assertFalse(failAdmission);
                    assertEquals(1, callbacks.get());
                    assertEquals(0, registry.pendingCount());
                }
            }
        }
    }

    @Test
    void registrationUsesTheCallerResultAsItsHandle() {
        try (var registry = new ZLinkServiceOperationRegistry()) {
            var operation =
                    ZLinkCompletionBridge.await(registry.register(java.time.Duration.ofSeconds(5)));
            org.junit.jupiter.api.Assertions.assertSame(operation, operation.completion());
            registry.discard(operation.id());
        }
    }

    @Test
    void completedDeadlinesAreReleasedWithinOneWheelRotation() throws Exception {
        AtomicLong clock = new AtomicLong();
        int completedRequests = 32;
        try (var registry =
                new ZLinkServiceOperationRegistry(
                        new ManualMaintenance(), new IllegalStateException("closed"), clock::get)) {
            for (int index = 0; index < completedRequests; index++) {
                var operation =
                        ZLinkCompletionBridge.await(registry.register(Duration.ofSeconds(30)));
                assertTrue(registry.complete(operation.id(), null));
            }
            assertEquals(0, registry.pendingCount());
            assertEquals(0, ZLinkCompletionBridge.await(registry.expire(clock.get())));
            clock.set(TimeUnit.MILLISECONDS.toNanos(256));
            assertEquals(0, ZLinkCompletionBridge.await(registry.expire(clock.get())));
            assertEquals(0, wheelEntryCount(registry));
        }
    }

    @Test
    void closeReleasesCompletedDeadlineReferences() throws Exception {
        var registry = new ZLinkServiceOperationRegistry(new ManualMaintenance());
        try {
            var completed = ZLinkCompletionBridge.await(registry.register(Duration.ofSeconds(30)));
            ZLinkCompletionBridge.await(registry.register(Duration.ofSeconds(30)));
            assertEquals(2, registry.pendingCount());
            assertEquals(2, wheelEntryCount(registry));
            assertTrue(registry.complete(completed.id(), null));
            assertEquals(1, registry.pendingCount());
            registry.close();
            assertTrue(registry.isClosed());
            assertEquals(0, registry.pendingCount());
            assertEquals(0, wheelEntryCount(registry));
            for (Object tail : registry.deadlineTailsForTest()) assertNull(tail);
            assertNull(registry.deadlineCursorForTest());
        } finally {
            registry.close();
        }
    }

    @Test
    void maintenanceTerminatesWhenItsOwnerLaneRejectsTheVisit() throws Exception {
        AtomicLong clock = new AtomicLong();
        var owner = new ZLinkStateLane();
        var workerReference = new AtomicReference<Thread>();
        var registry =
                new ZLinkServiceOperationRegistry(
                        owner,
                        command -> {
                            var thread = Thread.ofVirtual().unstarted(command);
                            workerReference.set(thread);
                            return thread;
                        },
                        new IllegalStateException("closed"),
                        () -> new TimeoutException("expired"),
                        clock::get);
        var operation = ZLinkCompletionBridge.await(registry.register(Duration.ofSeconds(30)));
        Thread worker = workerReference.get();
        try {
            ZLinkCompletionBridge.await(owner.closeAsync());
            clock.set(TimeUnit.SECONDS.toNanos(30));
            LockSupport.unpark(worker);
            worker.join(TimeUnit.SECONDS.toMillis(1));
            assertFalse(worker.isAlive(), "rejected maintenance must terminate");
        } finally {
            // The RED revision parks forever after rejection; release this test's worker.
            worker.interrupt();
            worker.join(TimeUnit.SECONDS.toMillis(1));
            registry.discard(operation.id());
        }
    }

    private static int wheelEntryCount(ZLinkServiceOperationRegistry registry) throws Exception {
        return ZLinkCompletionBridge.await(
                ownerLane(registry).runAsync(registry::deadlineEntryCountForTest));
    }

    @Test
    void wheelHandlesMixedDeadlinesAndIdentities() {
        AtomicLong clock = new AtomicLong();
        var maintenance = new ManualMaintenance();
        try (var registry =
                new ZLinkServiceOperationRegistry(
                        maintenance, new IllegalStateException("closed"), clock::get)) {
            List<ZLinkServiceOperationRegistry.Operation<Object>> operations = new ArrayList<>();
            for (int index = 0; index < 100; index++) {
                int timeout = (index * 37 % 100) + 1;
                operations.add(
                        ZLinkCompletionBridge.await(
                                registry.register(
                                        new UUID(index % 2, index + 1),
                                        Duration.ofMillis(timeout))));
            }
            assertEquals(
                    50,
                    ZLinkCompletionBridge.await(
                            registry.expire(TimeUnit.MILLISECONDS.toNanos(50))));
            for (int index = 0; index < operations.size(); index++) {
                int timeout = (index * 37 % 100) + 1;
                if (timeout <= 50)
                    assertThrows(
                            CompletionException.class, operations.get(index).completion()::join);
                else assertFalse(operations.get(index).completion().isDone());
            }
            assertEquals(
                    50,
                    ZLinkCompletionBridge.await(
                            registry.expire(TimeUnit.MILLISECONDS.toNanos(100))));
            assertEquals(0, registry.pendingCount());
        }
    }

    @Test
    void expirationYieldsAtTheManagementBudgetAndResumesAfterTerminalRemoval() {
        AtomicLong clock = new AtomicLong();
        var maintenance = new ManualMaintenance();
        try (var registry =
                new ZLinkServiceOperationRegistry(
                        maintenance,
                        new IllegalStateException("closed"),
                        () -> {
                            clock.addAndGet(
                                    systems.zlink.framework.runtime.internal.dispatch
                                            .ZLinkReceiveBatchBudget.DEFAULT_MAX_ELAPSED_NANOS);
                            return new TimeoutException("expired");
                        },
                        clock::get)) {
            var first = ZLinkCompletionBridge.await(registry.register(Duration.ofMillis(1)));
            var removed = ZLinkCompletionBridge.await(registry.register(Duration.ofMillis(1)));
            var last = ZLinkCompletionBridge.await(registry.register(Duration.ofMillis(1)));

            assertEquals(
                    1,
                    ZLinkCompletionBridge.await(registry.expire(TimeUnit.MILLISECONDS.toNanos(1))));
            assertTrue(registry.discard(removed.id()));
            assertEquals(1, ZLinkCompletionBridge.await(registry.expire(clock.get())));
            assertEquals(0, registry.pendingCount());
            assertThrows(CompletionException.class, first.completion()::join);
            assertThrows(CompletionException.class, last.completion()::join);
        }
    }

    @Test
    void roundsOnlyVisitsYieldAtTheManagementBudgetAndResumeFromTheCursor() {
        long budget =
                systems.zlink.framework.runtime.internal.dispatch.ZLinkReceiveBatchBudget
                        .DEFAULT_MAX_ELAPSED_NANOS;
        AtomicLong clock = new AtomicLong();
        AtomicLong step = new AtomicLong();
        var maintenance = new ManualMaintenance();
        try (var registry =
                new ZLinkServiceOperationRegistry(
                        maintenance,
                        new IllegalStateException("closed"),
                        () -> clock.getAndAdd(step.get()))) {
            List<ZLinkServiceOperationRegistry.Operation<Object>> later = new ArrayList<>();
            for (int index = 0; index < 8; index++)
                later.add(ZLinkCompletionBridge.await(registry.register(Duration.ofMillis(257))));
            var due = ZLinkCompletionBridge.await(registry.register(Duration.ofMillis(1)));
            // §4.2: every slot visit, a rounds-only one included, reads the management budget
            // counted from entry; four reads exhaust it.
            step.set(budget / 4);
            long tick = TimeUnit.MILLISECONDS.toNanos(1);

            assertEquals(0, ZLinkCompletionBridge.await(registry.expire(tick)));
            assertFalse(due.completion().isDone());
            int turns = 1;
            while (ZLinkCompletionBridge.await(registry.expire(tick)) == 0) assertTrue(++turns < 9);
            assertThrows(CompletionException.class, due.completion()::join);
            assertEquals(8, registry.pendingCount());
            for (var operation : later) assertFalse(operation.completion().isDone());

            step.set(0);
            assertEquals(
                    8,
                    ZLinkCompletionBridge.await(
                            registry.expire(TimeUnit.MILLISECONDS.toNanos(257))));
            assertEquals(0, registry.pendingCount());
        }
    }

    @Test
    void oneMaintenanceTaskServesEveryPendingDeadlineAndExpiresAtTheBoundary() {
        AtomicLong clock = new AtomicLong(100);
        var maintenance = new ManualMaintenance();
        try (var registry =
                new ZLinkServiceOperationRegistry(
                        maintenance, new IllegalStateException("closed"), clock::get)) {
            var first = ZLinkCompletionBridge.await(registry.register(Duration.ofNanos(10)));
            var second = ZLinkCompletionBridge.await(registry.register(Duration.ofNanos(10)));

            assertEquals(1, maintenance.starts);
            // The factory captures the worker; this test drives every expiry turn.
            // Hot-path E3: timer-wheel deadlines expire at the next 1ms tick, never early.
            assertEquals(0, ZLinkCompletionBridge.await(registry.expire(109)));
            assertEquals(0, ZLinkCompletionBridge.await(registry.expire(110)));
            assertEquals(
                    2,
                    ZLinkCompletionBridge.await(
                            registry.expire(100 + TimeUnit.MILLISECONDS.toNanos(1))));
            assertEquals(0, registry.pendingCount());
            assertTrue(
                    assertThrows(CompletionException.class, first.completion()::join).getCause()
                            instanceof TimeoutException);
            assertTrue(
                    assertThrows(CompletionException.class, second.completion()::join).getCause()
                            instanceof TimeoutException);
        }
    }

    @Test
    void deadlineComparisonSurvivesNanoTimeWraparound() {
        long tickNanos = TimeUnit.MILLISECONDS.toNanos(1);
        long origin = Long.MAX_VALUE - tickNanos;
        Duration timeout = Duration.ofMillis(2);
        AtomicLong clock = new AtomicLong(origin);
        var maintenance = new ManualMaintenance();
        try (var registry =
                new ZLinkServiceOperationRegistry(
                        maintenance, new IllegalStateException("closed"), clock::get)) {
            var operation = ZLinkCompletionBridge.await(registry.register(timeout));
            long deadline = origin + timeout.toNanos();

            assertTrue(deadline < 0);
            assertEquals(0, ZLinkCompletionBridge.await(registry.expire(Long.MAX_VALUE)));
            assertFalse(operation.completion().isDone());
            assertEquals(0, ZLinkCompletionBridge.await(registry.expire(deadline - 1)));
            assertFalse(operation.completion().isDone());
            clock.set(deadline + 1);
            assertEquals(1, ZLinkCompletionBridge.await(registry.expire(clock.get())));
            assertTrue(
                    assertThrows(CompletionException.class, operation.completion()::join).getCause()
                            instanceof TimeoutException);
            assertEquals(0, registry.pendingCount());
        }
    }

    @Test
    void wheelRoundsDoNotExpireInEarlierRevolutions() {
        AtomicLong clock = new AtomicLong();
        var maintenance = new ManualMaintenance();
        try (var registry =
                new ZLinkServiceOperationRegistry(
                        maintenance, new IllegalStateException("closed"), clock::get)) {
            var operation = ZLinkCompletionBridge.await(registry.register(Duration.ofMillis(513)));
            // Hot-path E3: occupied-slot visits preserve the remaining wheel rounds.
            assertEquals(
                    0,
                    ZLinkCompletionBridge.await(registry.expire(TimeUnit.MILLISECONDS.toNanos(1))));
            assertEquals(
                    0,
                    ZLinkCompletionBridge.await(
                            registry.expire(TimeUnit.MILLISECONDS.toNanos(257))));
            assertEquals(
                    0,
                    ZLinkCompletionBridge.await(
                            registry.expire(TimeUnit.MILLISECONDS.toNanos(512))));
            assertEquals(
                    1,
                    ZLinkCompletionBridge.await(
                            registry.expire(TimeUnit.MILLISECONDS.toNanos(513))));
            assertThrows(CompletionException.class, operation.completion()::join);
        }
    }

    @Test
    void registrationsDoNotCreateAdditionalScheduledTasks() {
        var maintenance = new ManualMaintenance();
        try (var registry = new ZLinkServiceOperationRegistry(maintenance)) {
            for (int index = 0; index < 1_000; index++) {
                var operation = ZLinkCompletionBridge.await(registry.register(Duration.ofHours(1)));
                assertTrue(registry.discard(operation.id()));
            }

            assertEquals(1, maintenance.starts);
        }
    }

    @Test
    void closeProgressesWhileBindingCompletionWaits() throws Exception {
        var closeFailure =
                new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.SHUTTING_DOWN, "channel runtime is closed");
        var discarded = new AtomicReference<String>();
        var transport = new CompletableFuture<String>();
        try (var registry = new ZLinkServiceOperationRegistry(closeFailure)) {
            var result =
                    registry.submit(
                            ZLinkServiceOperationIds.next(),
                            Duration.ofHours(1),
                            () -> transport,
                            discarded::set);
            var closeReturned = new CountDownLatch(1);
            var closer =
                    Thread.ofPlatform()
                            .start(
                                    () -> {
                                        registry.close();
                                        closeReturned.countDown();
                                    });
            assertTrue(closeReturned.await(1, TimeUnit.SECONDS));
            closer.join();
            assertSame(
                    closeFailure, assertThrows(CompletionException.class, result::join).getCause());
            assertFalse(transport.isDone());
            assertTrue(transport.complete("late reply"));
            assertEquals("late reply", discarded.get());
            assertEquals(0, registry.pendingCount());
        }
    }

    @Test
    void reentrantCloseWinsOverASubsequentSubmissionFailure() {
        var closed = new IllegalStateException("closed during binding submission");
        try (var registry = new ZLinkServiceOperationRegistry(closed)) {
            CompletableFuture<String> result =
                    registry.submit(
                            ZLinkServiceOperationIds.next(),
                            Duration.ofSeconds(1),
                            () -> {
                                registry.close();
                                throw new IllegalArgumentException("late submission failure");
                            },
                            ignored -> {});
            assertSame(closed, assertThrows(CompletionException.class, result::join).getCause());
            assertEquals(0, registry.pendingCount());
        }
    }

    @Test
    void synchronousSubmissionFailureUsesTheReservedCompletion() {
        RuntimeException expected = new RuntimeException("submit failed");
        try (var registry = new ZLinkServiceOperationRegistry()) {
            CompletableFuture<String> result =
                    registry.submit(
                            ZLinkServiceOperationIds.next(),
                            Duration.ofSeconds(1),
                            () -> {
                                throw expected;
                            },
                            ignored -> {});

            CompletionException failure = assertThrows(CompletionException.class, result::join);
            assertSame(expected, failure.getCause());
            assertEquals(0, registry.pendingCount());
        }
    }

    @Test
    void expiryProgressesWhileBindingCompletionWaits() {
        var transport = new CompletableFuture<String>();
        try (var registry =
                new ZLinkServiceOperationRegistry(
                        new ManualMaintenance(), new IllegalStateException("closed"), () -> 0L)) {
            var result =
                    registry.submit(
                            ZLinkServiceOperationIds.next(),
                            Duration.ofNanos(1),
                            () -> transport,
                            ignored -> {});
            assertEquals(
                    1,
                    ZLinkCompletionBridge.await(registry.expire(TimeUnit.MILLISECONDS.toNanos(1))));
            assertEquals(0, registry.pendingCount());
            assertFalse(transport.isDone());
            assertTrue(
                    assertThrows(CompletionException.class, result::join).getCause()
                            instanceof TimeoutException);
        }
    }

    @Test
    void submitAfterCloseReturnsTheConfiguredFailureWithoutStartingBinding() {
        var closeFailure =
                new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.SHUTTING_DOWN, "channel runtime is closed");
        AtomicInteger starts = new AtomicInteger();
        try (var registry = new ZLinkServiceOperationRegistry(closeFailure)) {
            registry.close();

            CompletableFuture<String> result =
                    registry.submit(
                            ZLinkServiceOperationIds.next(),
                            Duration.ofSeconds(1),
                            () -> {
                                starts.incrementAndGet();
                                return CompletableFuture.completedFuture("unexpected");
                            },
                            ignored -> {});

            CompletionException failure = assertThrows(CompletionException.class, result::join);
            assertSame(closeFailure, failure.getCause());
            assertEquals(0, starts.get());
        }
    }

    @Test
    void wireIdentityIsTheRegistryKeyAndInvalidIdentityDoesNotRegister() {
        try (var registry = new ZLinkServiceOperationRegistry()) {
            UUID firstId = new UUID(Long.MIN_VALUE, 7);
            UUID secondId = new UUID(Long.MAX_VALUE, 7);
            var first =
                    ZLinkCompletionBridge.await(registry.register(firstId, Duration.ofSeconds(1)));
            assertSame(firstId, first.id());
            assertThrows(
                    IllegalArgumentException.class,
                    () ->
                            ZLinkCompletionBridge.await(
                                    registry.register(firstId, Duration.ofSeconds(1))));
            assertThrows(
                    IllegalArgumentException.class,
                    () ->
                            ZLinkCompletionBridge.await(
                                    registry.register(new UUID(0, 0), Duration.ofSeconds(1))));
            assertEquals(1, registry.pendingCount());
            var second =
                    ZLinkCompletionBridge.await(registry.register(secondId, Duration.ofSeconds(1)));
            assertTrue(registry.complete(secondId, "second"));
            assertTrue(registry.complete(firstId, "first"));
            assertEquals("first", first.completion().join());
            assertEquals("second", second.completion().join());
        }
    }

    @Test
    void pendingEntryDoesNotAllocateAnExceptionForASuccessfulRequest() throws Exception {
        try (var registry = new ZLinkServiceOperationRegistry()) {
            var operation = ZLinkCompletionBridge.await(registry.register(Duration.ofSeconds(1)));
            Object entry = registry.pendingEntryForTest(operation.id());
            for (var field : entry.getClass().getDeclaredFields()) {
                if (Throwable.class.isAssignableFrom(field.getType())) {
                    field.setAccessible(true);
                    assertNull(field.get(entry), field.getName());
                }
            }
            assertTrue(registry.complete(operation.id(), "success"));
            assertEquals("success", operation.completion().join());
        }
    }

    @Test
    void discardedCallerCannotCancelAReusedPendingSlot() {
        try (var registry = new ZLinkServiceOperationRegistry()) {
            UUID id = UUID.randomUUID();
            for (int index = 0; index < 256; index++) {
                var old = ZLinkCompletionBridge.await(registry.register(id, Duration.ofSeconds(5)));
                assertTrue(registry.discard(id));
                var current =
                        ZLinkCompletionBridge.await(registry.register(id, Duration.ofSeconds(5)));
                assertFalse(old.completion().cancel(false));
                assertEquals(1, registry.pendingCount());
                assertFalse(current.completion().isDone());
                assertTrue(registry.discard(id));
            }
        }
    }

    @Test
    void firstTerminalPathWinsAndDetachesDeadline() {
        try (ZLinkServiceOperationRegistry registry = new ZLinkServiceOperationRegistry()) {
            ZLinkServiceOperationRegistry.Operation<String> operation =
                    ZLinkCompletionBridge.await(registry.register(Duration.ofSeconds(1)));
            assertTrue(registry.complete(operation.id(), "reply"));
            assertFalse(
                    registry.completeExceptionally(
                            operation.id(), new TimeoutException("late timeout")));
            assertEquals("reply", operation.completion().join());
            assertEquals(0, registry.pendingCount());
        }
    }

    @Test
    void invalidFailureDoesNotConsumeThePendingOperation() {
        try (ZLinkServiceOperationRegistry registry = new ZLinkServiceOperationRegistry()) {
            var operation = ZLinkCompletionBridge.await(registry.register(Duration.ofSeconds(1)));

            assertThrows(
                    NullPointerException.class,
                    () -> registry.completeExceptionally(operation.id(), null));
            assertEquals(1, registry.pendingCount());
            assertTrue(registry.complete(operation.id(), "reply"));
            assertEquals("reply", operation.completion().join());
        }
    }

    @Test
    void timeoutCompletesExactlyOnce() throws Exception {
        try (ZLinkServiceOperationRegistry registry = new ZLinkServiceOperationRegistry()) {
            ZLinkServiceOperationRegistry.Operation<String> operation =
                    ZLinkCompletionBridge.await(registry.register(Duration.ofMillis(10)));
            CompletionException failure =
                    assertThrows(
                            CompletionException.class,
                            () -> operation.completion().orTimeout(1, TimeUnit.SECONDS).join());
            assertTrue(failure.getCause() instanceof TimeoutException);
            assertFalse(registry.complete(operation.id(), "late reply"));
        }
    }

    @Test
    void registersMoreThanTheFormerPendingCapacity() {
        try (ZLinkServiceOperationRegistry registry = new ZLinkServiceOperationRegistry()) {
            List<UUID> pending = new ArrayList<>();
            for (int index = 0; index <= 4_096; index++) {
                pending.add(
                        ZLinkCompletionBridge.await(registry.register(Duration.ofSeconds(1))).id());
            }
            assertEquals(4_097, registry.pendingCount());
            pending.forEach(registry::discard);
        }
    }

    @Test
    void cancellationAtomicallyTakesTheOperationBeforeClose() {
        try (ZLinkServiceOperationRegistry registry = new ZLinkServiceOperationRegistry()) {
            var operation = ZLinkCompletionBridge.await(registry.register(Duration.ofSeconds(1)));
            AtomicInteger callbacks = new AtomicInteger();
            // The terminal runs on the completion lane (spec 01 §11). join()
            // on the source stage returns when the source completes, not
            // when its dependents have run; the dependent stage returned by
            // whenComplete is what completes after the callback returned.
            var observed =
                    operation
                            .completion()
                            .whenComplete((ignored, failure) -> callbacks.incrementAndGet());

            assertTrue(operation.completion().cancel(false));
            assertEquals(0, registry.pendingCount());
            assertThrows(CancellationException.class, operation.completion()::join);
            CompletionException relayed = assertThrows(CompletionException.class, observed::join);
            assertTrue(relayed.getCause() instanceof CancellationException);
            registry.close();
            assertEquals(1, callbacks.get());
            assertFalse(registry.complete(operation.id(), "late"));
        }
    }

    @Test
    void submittedRequestCancellationReachesTheBindingWaiterBeforeClose() throws Exception {
        try (ZLinkServiceOperationRegistry registry = new ZLinkServiceOperationRegistry()) {
            CompletableFuture<String> binding = new CompletableFuture<>();
            CountDownLatch bindingCancelled = new CountDownLatch(1);
            binding.whenComplete(
                    (ignored, failure) -> {
                        if (binding.isCancelled()) bindingCancelled.countDown();
                    });
            CompletableFuture<String> caller =
                    registry.submit(
                            UUID.randomUUID(), Duration.ofSeconds(1), () -> binding, ignored -> {});
            ownerLane(registry).runAsync(() -> {}).toCompletableFuture().get(1, TimeUnit.SECONDS);
            assertTrue(caller.cancel(false));
            assertTrue(bindingCancelled.await(1, TimeUnit.SECONDS));
            registry.close();
            assertTrue(caller.isCancelled());
            assertEquals(0, registry.pendingCount());
        }
    }

    @Test
    void terminalCompletionRunsOnANewTurnOutsideTheRegistryGate() {
        try (ZLinkServiceOperationRegistry registry = new ZLinkServiceOperationRegistry()) {
            var operation = ZLinkCompletionBridge.await(registry.register(Duration.ofSeconds(1)));
            var callbackThread = new CompletableFuture<Thread>();
            var completionPool = new AtomicReference<ForkJoinPool>();
            var completionContext = new AtomicReference<Boolean>();
            Thread callerThread = Thread.currentThread();
            operation
                    .completion()
                    .whenComplete(
                            (ignored, failure) -> {
                                completionPool.set(ForkJoinTask.getPool());
                                completionContext.set(
                                        ZLinkServiceCompletionDispatcher.isCurrentCompletion());
                                callbackThread.complete(Thread.currentThread());
                            });

            assertTrue(registry.complete(operation.id(), "reply"));
            Thread actualThread = callbackThread.orTimeout(1, TimeUnit.SECONDS).join();
            assertEquals("reply", operation.completion().join());
            assertNotEquals(callerThread, actualThread);
            assertSame(ForkJoinPool.commonPool(), completionPool.get());
            assertTrue(completionContext.get());
        }
    }

    @Test
    void registrationsAreNotCappedAcrossRegistries() {
        try (ZLinkServiceOperationRegistry first = new ZLinkServiceOperationRegistry();
                ZLinkServiceOperationRegistry second = new ZLinkServiceOperationRegistry()) {
            List<UUID> firstIds = new ArrayList<>();
            List<UUID> secondIds = new ArrayList<>();
            for (int index = 0; index <= 2_048; index++) {
                firstIds.add(ZLinkCompletionBridge.await(first.register(Duration.ofHours(1))).id());
                secondIds.add(
                        ZLinkCompletionBridge.await(second.register(Duration.ofHours(1))).id());
            }

            UUID admittedAfterFormerLimit =
                    ZLinkCompletionBridge.await(second.register(Duration.ofHours(1))).id();
            firstIds.forEach(id -> assertTrue(first.discard(id)));
            secondIds.forEach(id -> assertTrue(second.discard(id)));
            assertTrue(second.discard(admittedAfterFormerLimit));
        }
    }

    @Test
    void runningCallbackDoesNotCapFurtherRegistrations() throws Exception {
        try (ZLinkServiceOperationRegistry first = new ZLinkServiceOperationRegistry();
                ZLinkServiceOperationRegistry second = new ZLinkServiceOperationRegistry()) {
            var running = ZLinkCompletionBridge.await(first.register(Duration.ofHours(1)));
            CountDownLatch callbackEntered = new CountDownLatch(1);
            CountDownLatch releaseCallback = new CountDownLatch(1);
            running.completion()
                    .whenComplete(
                            (ignored, failure) -> {
                                callbackEntered.countDown();
                                try {
                                    releaseCallback.await();
                                } catch (InterruptedException interrupted) {
                                    Thread.currentThread().interrupt();
                                }
                            });
            List<UUID> pending = new ArrayList<>();
            for (int index = 1; index <= 4_096; index++) {
                pending.add(ZLinkCompletionBridge.await(second.register(Duration.ofHours(1))).id());
            }

            try {
                assertTrue(first.complete(running.id(), "reply"));
                assertTrue(callbackEntered.await(1, TimeUnit.SECONDS));
                pending.add(ZLinkCompletionBridge.await(second.register(Duration.ofHours(1))).id());
                pending.forEach(id -> assertTrue(second.discard(id)));
                pending.clear();
            } finally {
                pending.forEach(second::discard);
                releaseCallback.countDown();
            }
            assertEquals("reply", running.completion().orTimeout(1, TimeUnit.SECONDS).join());
        }
    }

    @Test
    void closeDispatchesEveryAcceptedTerminalWithoutWaitingForAnotherCallback() {
        var closeFailure = new IllegalStateException("closed");
        ZLinkServiceOperationRegistry registry = new ZLinkServiceOperationRegistry(closeFailure);
        var firstEntered = new CompletableFuture<Void>();
        var releaseFirst = new CompletableFuture<Void>();
        try {
            List<ZLinkServiceOperationRegistry.Operation<String>> operations = new ArrayList<>();
            List<CompletableFuture<Void>> callbacks = new ArrayList<>();
            var delivered = new AtomicInteger();
            for (int index = 0; index < 3; index++) {
                boolean first = index == 0;
                var operation =
                        ZLinkCompletionBridge.await(registry.<String>register(Duration.ofHours(1)));
                operations.add(operation);
                callbacks.add(
                        operation.handle(
                                (value, failure) -> {
                                    if (first) {
                                        firstEntered.complete(null);
                                        releaseFirst.join();
                                    } else {
                                        firstEntered.join();
                                    }
                                    assertNull(value);
                                    assertSame(closeFailure, failure);
                                    delivered.incrementAndGet();
                                    return null;
                                }));
            }

            registry.close();

            try {
                firstEntered.orTimeout(1, TimeUnit.SECONDS).join();
                CompletableFuture.allOf(
                                callbacks
                                        .subList(1, callbacks.size())
                                        .toArray(CompletableFuture[]::new))
                        .orTimeout(1, TimeUnit.SECONDS)
                        .join();
                assertFalse(callbacks.getFirst().isDone());
                assertEquals(operations.size() - 1, delivered.get());
            } finally {
                releaseFirst.complete(null);
            }
            CompletableFuture.allOf(callbacks.toArray(CompletableFuture[]::new)).join();
            assertEquals(operations.size(), delivered.get());
            for (var operation : operations) {
                assertFalse(registry.complete(operation.id(), "late reply"));
                assertFalse(registry.completeExceptionally(operation.id(), closeFailure));
            }
            assertEquals(0, registry.pendingCount());
        } finally {
            releaseFirst.complete(null);
            registry.close();
        }
    }

    @Test
    void acceptedTerminalCompletesWithoutWakingMaintenance() {
        try (ZLinkServiceOperationRegistry registry = new ZLinkServiceOperationRegistry()) {
            var operation = ZLinkCompletionBridge.await(registry.register(Duration.ofHours(1)));

            assertTrue(registry.complete(operation.id(), "reply"));
            assertEquals("reply", operation.completion().orTimeout(1, TimeUnit.SECONDS).join());
        }
    }

    @Test
    void concurrentTerminalProducersDeliverEachAcceptedResultExactlyOnce() throws Exception {
        int producerCount = 4;
        int operationsPerProducer = 64;
        var producers = Executors.newFixedThreadPool(producerCount);
        try (ZLinkServiceOperationRegistry registry = new ZLinkServiceOperationRegistry()) {
            List<List<ZLinkServiceOperationRegistry.Operation<Integer>>> operations =
                    new ArrayList<>();
            List<CompletableFuture<Void>> callbacks = new ArrayList<>();
            var start = new CompletableFuture<Void>();
            var delivered = new AtomicInteger();
            for (int producer = 0; producer < producerCount; producer++) {
                List<ZLinkServiceOperationRegistry.Operation<Integer>> pending = new ArrayList<>();
                for (int index = 0; index < operationsPerProducer; index++) {
                    int expected = producer * operationsPerProducer + index;
                    var operation =
                            ZLinkCompletionBridge.await(
                                    registry.<Integer>register(Duration.ofHours(1)));
                    callbacks.add(
                            operation.thenAccept(
                                    value -> {
                                        assertEquals(expected, value);
                                        delivered.incrementAndGet();
                                    }));
                    pending.add(operation);
                }
                operations.add(pending);
            }
            List<java.util.concurrent.Future<?>> submitted = new ArrayList<>();
            for (int producer = 0; producer < operations.size(); producer++) {
                var pending = operations.get(producer);
                int firstValue = producer * pending.size();
                submitted.add(
                        producers.submit(
                                () -> {
                                    start.join();
                                    for (int index = 0; index < pending.size(); index++) {
                                        UUID id = pending.get(index).id();
                                        assertTrue(registry.complete(id, firstValue + index));
                                        assertFalse(registry.complete(id, -1));
                                    }
                                }));
            }
            start.complete(null);
            for (var submission : submitted) submission.get(5, TimeUnit.SECONDS);
            CompletableFuture.allOf(callbacks.toArray(CompletableFuture[]::new))
                    .orTimeout(5, TimeUnit.SECONDS)
                    .join();
            assertEquals(callbacks.size(), delivered.get());
            assertEquals(0, registry.pendingCount());
        } finally {
            producers.shutdownNow();
        }
    }

    private static void await(CountDownLatch latch) {
        try {
            latch.await();
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException(interrupted);
        }
    }

    private static final class ManualMaintenance implements java.util.concurrent.ThreadFactory {
        private int starts;

        @Override
        public Thread newThread(Runnable command) {
            return new Thread(command) {
                @Override
                public synchronized void start() {
                    starts++;
                }
            };
        }
    }

    private static systems.zlink.framework.runtime.internal.execution.ZLinkStateLane ownerLane(
            ZLinkServiceOperationRegistry registry) throws Exception {
        return registry.ownerLaneForTest();
    }

    private static void parkedIndefinitely(Thread worker, Object blocker) throws Exception {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (!isParkedIndefinitely(worker, (ZLinkServiceOperationRegistry) blocker)) {
            if (System.nanoTime() - deadline >= 0) throw new AssertionError("worker did not park");
            Thread.yield();
        }
    }

    private static boolean isParkedIndefinitely(
            Thread worker, ZLinkServiceOperationRegistry registry) {
        return registry.maintenanceTickForTest() == -1
                && worker.getState() == Thread.State.WAITING
                && java.util.concurrent.locks.LockSupport.getBlocker(worker) == registry;
    }
}
