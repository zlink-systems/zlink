package systems.zlink.framework.runtime.internal.execution;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext;
import systems.zlink.framework.testutil.PerCallAllocation;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkStateLaneTest {
    private static final int POOL_OCCUPATION_TIMEOUT_SECONDS = 5;
    private static final int COMPLETION_TIMEOUT_SECONDS = 1;

    @Test
    void idleSynchronousTurnsAllocateNoPrivateCompletion() {
        var lane = new ZLinkStateLane(Runnable::run);
        java.util.function.Supplier<Object> work = () -> null;
        long bytes =
                PerCallAllocation.minWindow(
                        () -> {
                            lane.awaitTurn(work);
                            return null;
                        },
                        20_000,
                        50_000);
        assertTrue(bytes < 20_000, "idle turn allocated " + bytes + " bytes");
        assertTrue(lane.closeAsync().toCompletableFuture().isDone());
    }

    @Test
    void contendedSynchronousTurnsRunOnTheirWaitingCallersInFifoOrder() throws Exception {
        var executorCalls = new AtomicInteger();
        try (var executor = Executors.newVirtualThreadPerTaskExecutor()) {
            var published = new java.util.concurrent.Semaphore(0);
            var lane =
                    new ZLinkStateLane(
                            command -> {
                                executorCalls.incrementAndGet();
                                executor.execute(command);
                            },
                            publicationQueue(published));
            var entered = new CompletableFuture<Void>();
            var release = new CompletableFuture<Void>();
            var active = new AtomicInteger();
            var overlap = new AtomicBoolean();
            var order = new ArrayList<Integer>();
            var callers = new ArrayList<java.util.concurrent.Future<Boolean>>();
            var owner =
                    executor.submit(
                            () ->
                                    lane.awaitTurn(
                                            () -> {
                                                active.incrementAndGet();
                                                entered.complete(null);
                                                release.join();
                                                active.decrementAndGet();
                                                return null;
                                            }));
            try {
                entered.get(5, TimeUnit.SECONDS);
                for (int index = 0; index < 16; index++) {
                    int turn = index;
                    callers.add(
                            executor.submit(
                                    () -> {
                                        var caller = Thread.currentThread();
                                        return lane.awaitTurn(
                                                () -> {
                                                    if (active.incrementAndGet() != 1)
                                                        overlap.set(true);
                                                    order.add(turn);
                                                    boolean sameThread =
                                                            caller == Thread.currentThread();
                                                    active.decrementAndGet();
                                                    return sameThread;
                                                });
                                    }));
                    assertTrue(published.tryAcquire(5, TimeUnit.SECONDS));
                }
                var closed = lane.closeAsync().toCompletableFuture();
                assertFalse(closed.isDone());
                release.complete(null);
                owner.get(5, TimeUnit.SECONDS);
                for (var caller : callers) {
                    assertTrue(
                            caller.get(5, TimeUnit.SECONDS), "turn left its waiting caller thread");
                }
                closed.get(5, TimeUnit.SECONDS);
                assertEquals(java.util.stream.IntStream.range(0, 16).boxed().toList(), order);
                assertFalse(overlap.get());
                assertEquals(0, active.get());
                assertEquals(0, executorCalls.get());
                assertThrows(IllegalStateException.class, () -> lane.awaitTurn(() -> null));
            } finally {
                release.complete(null);
            }
        }
    }

    @Test
    void pendingAsyncTurnHandsOffToCallersAndMixedTurnsKeepFifo() throws Exception {
        var scheduled = new java.util.concurrent.LinkedBlockingQueue<Runnable>();
        var executorCalls = new AtomicInteger();
        var published = new java.util.concurrent.Semaphore(0);
        var lane =
                new ZLinkStateLane(
                        command -> {
                            executorCalls.incrementAndGet();
                            scheduled.add(command);
                        },
                        publicationQueue(published));
        var pending = new CompletableFuture<Void>();
        assertTrue(lane.tryPost(() -> pending));
        assertTrue(published.tryAcquire(5, TimeUnit.SECONDS));
        scheduled.remove().run();
        var order = new ArrayList<Integer>();
        var firstEntered = new CompletableFuture<Void>();
        var releaseFirst = new CompletableFuture<Void>();
        var lastEntered = new CompletableFuture<Void>();
        var releaseLast = new CompletableFuture<Void>();
        try (var callers = Executors.newVirtualThreadPerTaskExecutor()) {
            var first =
                    callers.submit(
                            () -> {
                                var caller = Thread.currentThread();
                                return lane.awaitTurn(
                                        () -> {
                                            order.add(1);
                                            firstEntered.complete(null);
                                            releaseFirst.join();
                                            return caller == Thread.currentThread();
                                        });
                            });
            try {
                assertTrue(published.tryAcquire(5, TimeUnit.SECONDS));
                var async = lane.runAsync(() -> order.add(2));
                var nowOrQueue = lane.runNowOrQueue(() -> order.add(3));
                assertTrue(
                        lane.tryPost(
                                () -> {
                                    order.add(4);
                                    return CompletableFuture.completedFuture(null);
                                }));
                assertTrue(published.tryAcquire(3, 5, TimeUnit.SECONDS));
                var last =
                        callers.submit(
                                () -> {
                                    var caller = Thread.currentThread();
                                    return lane.awaitTurn(
                                            () -> {
                                                order.add(5);
                                                lastEntered.complete(null);
                                                releaseLast.join();
                                                return caller == Thread.currentThread();
                                            });
                                });
                assertTrue(published.tryAcquire(5, TimeUnit.SECONDS));
                var closed = lane.closeAsync().toCompletableFuture();
                pending.complete(null);
                firstEntered.get(5, TimeUnit.SECONDS);
                assertEquals(
                        1, executorCalls.get(), "pending async must grant the caller directly");
                assertTrue(scheduled.isEmpty());
                assertFalse(closed.isDone());
                releaseFirst.complete(null);
                assertTrue(first.get(5, TimeUnit.SECONDS));
                var drain = scheduled.poll(5, TimeUnit.SECONDS);
                assertTrue(drain != null);
                drain.run();
                lastEntered.get(5, TimeUnit.SECONDS);
                assertFalse(closed.isDone());
                releaseLast.complete(null);
                assertTrue(last.get(5, TimeUnit.SECONDS));
                closed.get(5, TimeUnit.SECONDS);
                async.toCompletableFuture().get(5, TimeUnit.SECONDS);
                nowOrQueue.toCompletableFuture().get(5, TimeUnit.SECONDS);
                assertEquals(List.of(1, 2, 3, 4, 5), order);
                assertEquals(2, executorCalls.get());
                assertTrue(scheduled.isEmpty());
            } finally {
                pending.complete(null);
                releaseFirst.complete(null);
                releaseLast.complete(null);
                Runnable remaining;
                while ((remaining = scheduled.poll()) != null) remaining.run();
            }
        }
    }

    @Test
    void synchronousTurnsPreserveFailuresAndRejectReentryAfterHandoff() throws Exception {
        var cause = new IllegalArgumentException("turn failed");
        var nested = new CompletionException(cause);
        for (Throwable failure :
                List.of(
                        cause,
                        new AssertionError("turn failed"),
                        new CancellationException("turn cancelled"),
                        nested,
                        new CompletionException(nested))) {
            var published = new java.util.concurrent.Semaphore(0);
            var lane = new ZLinkStateLane(publicationQueue(published));
            var pending = new CompletableFuture<Void>();
            assertTrue(lane.tryPost(() -> pending));
            assertTrue(published.tryAcquire(5, TimeUnit.SECONDS));
            try (var callers = Executors.newVirtualThreadPerTaskExecutor()) {
                var result =
                        callers.submit(
                                () -> {
                                    var thrown =
                                            assertThrows(
                                                    Throwable.class,
                                                    () ->
                                                            lane.awaitTurn(
                                                                    () -> {
                                                                        assertSame(
                                                                                lane,
                                                                                ZLinkStateLane
                                                                                        .current());
                                                                        assertThrows(
                                                                                IllegalStateException
                                                                                        .class,
                                                                                () ->
                                                                                        lane
                                                                                                .awaitTurn(
                                                                                                        () ->
                                                                                                                null));
                                                                        if (failure
                                                                                instanceof
                                                                                Error error)
                                                                            throw error;
                                                                        throw (RuntimeException)
                                                                                failure;
                                                                    }));
                                    assertSame(null, ZLinkStateLane.current());
                                    return thrown;
                                });
                try {
                    assertTrue(published.tryAcquire(5, TimeUnit.SECONDS));
                    var following =
                            callers.submit(
                                    () -> {
                                        var caller = Thread.currentThread();
                                        boolean sameOwner =
                                                lane.awaitTurn(
                                                        () ->
                                                                caller == Thread.currentThread()
                                                                        && lane.isOnLane());
                                        assertSame(null, ZLinkStateLane.current());
                                        return sameOwner;
                                    });
                    assertTrue(published.tryAcquire(5, TimeUnit.SECONDS));
                    pending.complete(null);
                    Throwable expected =
                            failure instanceof CompletionException wrapped
                                    ? wrapped.getCause()
                                    : failure;
                    assertSame(expected, result.get(5, TimeUnit.SECONDS));
                    assertTrue(following.get(5, TimeUnit.SECONDS));
                    assertEquals(42, lane.awaitTurn(() -> 42));
                    assertFalse(lane.isOnLane());
                    lane.closeAsync().toCompletableFuture().get(5, TimeUnit.SECONDS);
                } finally {
                    pending.complete(null);
                }
            }
        }
    }

    private static java.util.concurrent.ConcurrentLinkedQueue<ZLinkStateLane.WorkItem>
            publicationQueue(java.util.concurrent.Semaphore published) {
        return new java.util.concurrent.ConcurrentLinkedQueue<ZLinkStateLane.WorkItem>() {
            @Override
            public boolean add(ZLinkStateLane.WorkItem turn) {
                boolean added = super.add(turn);
                published.release();
                return added;
            }
        };
    }

    @Test
    void concurrentFirstSubmissionsKeepOneTurnOwnerAndCloseDrainsEveryTurn() throws Exception {
        var lane = new ZLinkStateLane();
        int producers = 16;
        var start = new java.util.concurrent.CyclicBarrier(producers);
        var active = new AtomicInteger();
        var completed = new AtomicInteger();
        var submitted = new ArrayList<java.util.concurrent.Future<CompletionStage<Void>>>();
        try (var executor = Executors.newVirtualThreadPerTaskExecutor()) {
            for (int index = 0; index < producers; index++) {
                submitted.add(
                        executor.submit(
                                () -> {
                                    start.await(5, TimeUnit.SECONDS);
                                    return lane.runAsync(
                                            () -> {
                                                assertEquals(1, active.incrementAndGet());
                                                completed.incrementAndGet();
                                                assertEquals(0, active.decrementAndGet());
                                            });
                                }));
            }
            var turns = new ArrayList<CompletionStage<Void>>();
            for (var submission : submitted) turns.add(submission.get(5, TimeUnit.SECONDS));
            lane.closeAsync().toCompletableFuture().get(5, TimeUnit.SECONDS);
            for (var turn : turns) turn.toCompletableFuture().get(5, TimeUnit.SECONDS);
            assertEquals(producers, completed.get());
            assertThrows(IllegalStateException.class, () -> lane.runAsync(() -> {}));
        }
    }

    @Test
    void completedTurnsDrainOnOneExecutorInvocation() {
        var scheduled = new java.util.ArrayDeque<Runnable>();
        var lane = new ZLinkStateLane(scheduled::addLast);
        var turns = new ArrayList<Integer>();
        var results = new ArrayList<CompletableFuture<Integer>>();
        for (int index = 0; index < 64; index++) {
            int value = index;
            results.add(
                    lane.runAsync(
                                    () -> {
                                        assertSame(lane, ZLinkStateLane.current());
                                        turns.add(value);
                                        return value;
                                    })
                            .toCompletableFuture());
        }
        assertEquals(1, scheduled.size());
        scheduled.removeFirst().run();
        assertEquals(64, turns.size());
        assertTrue(scheduled.isEmpty());
        for (int index = 0; index < results.size(); index++) {
            assertEquals(index, results.get(index).join());
            assertEquals(index, turns.get(index));
        }
        assertTrue(lane.closeAsync().toCompletableFuture().isDone());
    }

    @Test
    void pendingTurnKeepsTheLaneUntilItsCompletion() {
        var scheduled = new java.util.ArrayDeque<Runnable>();
        var lane = new ZLinkStateLane(scheduled::addLast);
        var pending = new CompletableFuture<Void>();
        assertTrue(lane.tryPost(() -> pending));
        var ran = new AtomicBoolean();
        var following = lane.runAsync(() -> ran.set(true));
        var closed = lane.closeAsync().toCompletableFuture();
        scheduled.removeFirst().run();
        assertFalse(ran.get());
        assertFalse(closed.isDone());
        assertTrue(scheduled.isEmpty());
        pending.complete(null);
        assertEquals(1, scheduled.size());
        scheduled.removeFirst().run();
        following.toCompletableFuture().join();
        assertTrue(ran.get());
        assertTrue(closed.isDone());
        assertTrue(scheduled.isEmpty());
    }

    @Test
    void pendingCompletionProgressesWhileObservationPoolIsBlocked() throws Exception {
        var pool = java.util.concurrent.ForkJoinPool.commonPool();
        int workers = pool.getParallelism();
        var occupied = new java.util.concurrent.CountDownLatch(workers);
        var release = new java.util.concurrent.CountDownLatch(1);
        try {
            for (int i = 0; i < workers; i++) {
                pool.execute(
                        () -> {
                            occupied.countDown();
                            try {
                                release.await();
                            } catch (InterruptedException failure) {
                                Thread.currentThread().interrupt();
                                throw new AssertionError(failure);
                            }
                        });
            }
            assertTrue(occupied.await(POOL_OCCUPATION_TIMEOUT_SECONDS, TimeUnit.SECONDS));
            var scheduled = new java.util.ArrayDeque<Runnable>();
            var lane = new ZLinkStateLane(scheduled::addLast);
            var result = lane.runAsync(() -> 42).toCompletableFuture();
            scheduled.removeFirst().run();
            assertEquals(42, result.get(COMPLETION_TIMEOUT_SECONDS, TimeUnit.SECONDS));
            while (!scheduled.isEmpty()) scheduled.removeFirst().run();
            lane.closeAsync()
                    .toCompletableFuture()
                    .get(COMPLETION_TIMEOUT_SECONDS, TimeUnit.SECONDS);
        } finally {
            release.countDown();
        }
    }

    @org.junit.jupiter.params.ParameterizedTest
    @org.junit.jupiter.params.provider.ValueSource(booleans = {false, true})
    void closeWaitsForAnAdmissionThatHasNotYetPublishedItsTurn(boolean post) throws Exception {
        var adding = new java.util.concurrent.CountDownLatch(1);
        var publish = new java.util.concurrent.CountDownLatch(1);
        var lane =
                new ZLinkStateLane(
                        new java.util.concurrent.ConcurrentLinkedQueue<ZLinkStateLane.WorkItem>() {
                            @Override
                            public boolean add(ZLinkStateLane.WorkItem turn) {
                                adding.countDown();
                                try {
                                    assertTrue(publish.await(5, TimeUnit.SECONDS));
                                } catch (InterruptedException failure) {
                                    throw new AssertionError(failure);
                                }
                                return super.add(turn);
                            }
                        });
        var ran = new AtomicBoolean();
        try (var executor = Executors.newVirtualThreadPerTaskExecutor()) {
            var submitted =
                    executor.submit(
                            () -> {
                                if (post) {
                                    assertTrue(
                                            lane.tryPost(
                                                    () -> {
                                                        ran.set(true);
                                                        return CompletableFuture.completedFuture(
                                                                null);
                                                    }));
                                } else {
                                    lane.runAsync(() -> ran.set(true)).toCompletableFuture().join();
                                }
                            });
            try {
                assertTrue(adding.await(5, TimeUnit.SECONDS));
                var closed = lane.closeAsync().toCompletableFuture();
                assertFalse(closed.isDone());
                publish.countDown();
                submitted.get(5, TimeUnit.SECONDS);
                closed.get(5, TimeUnit.SECONDS);
                assertTrue(ran.get());
                assertFalse(lane.tryPost(() -> CompletableFuture.completedFuture(null)));
                assertThrows(IllegalStateException.class, () -> lane.runAsync(() -> null));
            } finally {
                publish.countDown();
            }
        }
    }

    @Test
    void inlineClaimDoesNotBypassQueuedTurnsAndPreservesNullCompletion() {
        var scheduled = new java.util.ArrayDeque<Runnable>();
        var lane = new ZLinkStateLane(scheduled::addLast);
        var turns = new ArrayList<Integer>();
        var queued =
                lane.runAsync(
                        () -> {
                            turns.add(1);
                            return 1;
                        });
        assertTrue(
                lane.tryRunNow(
                                () -> {
                                    turns.add(2);
                                    return 2;
                                })
                        == null);
        scheduled.removeFirst().run();
        assertEquals(1, queued.toCompletableFuture().join());
        assertEquals(List.of(1), turns);
        while (!scheduled.isEmpty()) {
            scheduled.removeFirst().run();
        }
        var claimed =
                lane.tryRunNow(
                        () -> {
                            turns.add(3);
                            return null;
                        });
        assertTrue(claimed != null);
        assertTrue(claimed.toCompletableFuture().isDone());
        assertTrue(claimed.toCompletableFuture().join() == null);
        assertEquals(List.of(1, 3), turns);
    }

    @Test
    void inlineClaimPreservesReentrantAndClosedRejection() {
        var lane = new ZLinkStateLane(Runnable::run);
        lane.tryRunNow(
                        () -> {
                            assertThrows(
                                    IllegalStateException.class, () -> lane.tryRunNow(() -> null));
                            return null;
                        })
                .toCompletableFuture()
                .join();
        lane.closeAsync().toCompletableFuture().join();
        assertThrows(IllegalStateException.class, () -> lane.tryRunNow(() -> null));
    }

    @Test
    void synchronousWaitGuardUsesExistingExecutionContext() {
        assertTrue(ZLinkStateLane.assertMayBlock());
        ZLinkStateLane lane = new ZLinkStateLane(Runnable::run);
        lane.runAsync(
                        () -> {
                            assertEquals(
                                    ZLinkFrameworkErrorKind.INVALID_OPERATION,
                                    assertThrows(
                                                    ZLinkFrameworkException.class,
                                                    ZLinkStateLane::assertMayBlock)
                                            .kind());
                            return null;
                        })
                .toCompletableFuture()
                .join();
        try (var ignored = ZLinkSuspendInvocationContext.enterSerialExecutionTurn(lane)) {
            assertEquals(
                    ZLinkFrameworkErrorKind.INVALID_OPERATION,
                    assertThrows(ZLinkFrameworkException.class, ZLinkStateLane::assertMayBlock)
                            .kind());
        }
    }

    @Test
    void directExecutorReturnsCompletedTurnsWithoutAnotherCompletionTask() {
        ZLinkStateLane lane = new ZLinkStateLane(Runnable::run);
        for (int index = 0; index < 100; index++) {
            CompletableFuture<Integer> result =
                    lane.runAsync(
                                    () -> {
                                        assertTrue(lane.isOnLane());
                                        return 42;
                                    })
                            .toCompletableFuture();
            assertTrue(result.isDone());
            assertFalse(lane.isOnLane());
            assertEquals(42, result.join());
            assertEquals(
                    43,
                    result.thenApply(
                                    value -> {
                                        assertFalse(lane.isOnLane());
                                        return lane.runAsync(() -> value + 1)
                                                .toCompletableFuture()
                                                .join();
                                    })
                            .join());
        }
        IllegalArgumentException failure = new IllegalArgumentException("activation failed");
        CompletableFuture<?> failed =
                lane.runAsync(
                                () -> {
                                    throw failure;
                                })
                        .toCompletableFuture();
        assertTrue(failed.isDone());
        assertSame(failure, assertThrows(CompletionException.class, failed::join).getCause());
        CancellationException cancelledWork = new CancellationException("work cancelled");
        CompletableFuture<?> cancellationFailure =
                lane.runAsync(
                                () -> {
                                    throw cancelledWork;
                                })
                        .toCompletableFuture();
        assertTrue(cancellationFailure.isDone());
        assertFalse(cancellationFailure.isCancelled());
        assertSame(
                cancelledWork,
                assertThrows(CompletionException.class, cancellationFailure::join).getCause());
        assertFalse(lane.isOnLane());
        lane.closeAsync().toCompletableFuture().join();
    }

    @Test
    void pendingCompletionCanReenterASingleThreadLaneAfterSuccessOrFailure() throws Exception {
        try (var worker = Executors.newSingleThreadExecutor()) {
            ZLinkStateLane lane = new ZLinkStateLane(worker);
            for (boolean fail : List.of(false, true)) {
                CompletableFuture<Void> entered = new CompletableFuture<>();
                CompletableFuture<Void> release = new CompletableFuture<>();
                CompletableFuture<Integer> result =
                        lane.runAsync(
                                        () -> {
                                            entered.complete(null);
                                            release.join();
                                            if (fail) {
                                                throw new IllegalArgumentException(
                                                        "activation failed");
                                            }
                                            return 1;
                                        })
                                .toCompletableFuture();
                try {
                    entered.get(3, TimeUnit.SECONDS);
                    CompletableFuture<Integer> dependent =
                            result.handle(
                                    (value, error) -> {
                                        assertFalse(lane.isOnLane());
                                        assertEquals(fail, error != null);
                                        return lane.runAsync(() -> 42).toCompletableFuture().join();
                                    });
                    release.complete(null);
                    assertEquals(42, dependent.get(3, TimeUnit.SECONDS));
                } finally {
                    release.complete(null);
                }
            }
            lane.closeAsync().toCompletableFuture().get(3, TimeUnit.SECONDS);
        }
    }

    @Test
    void defaultLanesShareExecutorButKeepIndependentTurnsAndClose() throws Exception {
        ZLinkStateLane first = new ZLinkStateLane();
        ZLinkStateLane second = new ZLinkStateLane();
        assertTrue(first.sharesExecutorWith(second));

        CompletableFuture<Void> entered = new CompletableFuture<>();
        CompletableFuture<Void> release = new CompletableFuture<>();
        var blocked =
                first.runAsync(
                        () -> {
                            entered.complete(null);
                            release.join();
                        });
        try {
            entered.get(3, TimeUnit.SECONDS);
            assertEquals(
                    42, second.runAsync(() -> 42).toCompletableFuture().get(3, TimeUnit.SECONDS));
        } finally {
            release.complete(null);
        }
        blocked.toCompletableFuture().get(3, TimeUnit.SECONDS);
        first.closeAsync().toCompletableFuture().get(3, TimeUnit.SECONDS);
        assertEquals(43, second.runAsync(() -> 43).toCompletableFuture().get(3, TimeUnit.SECONDS));
        second.closeAsync().toCompletableFuture().get(3, TimeUnit.SECONDS);
    }

    @Test
    void runAsyncReturnsTheResultOfTheWork() throws Exception {
        ZLinkStateLane lane = new ZLinkStateLane();

        assertEquals(42, lane.runAsync(() -> 42).toCompletableFuture().get(3, TimeUnit.SECONDS));

        lane.closeAsync().toCompletableFuture().get(3, TimeUnit.SECONDS);
    }

    @Test
    void runAsyncSurfacesAFailureToItsOwnCaller() throws Exception {
        ZLinkStateLane lane = new ZLinkStateLane();

        CompletionException error =
                assertThrows(
                        CompletionException.class,
                        () ->
                                lane.runAsync(
                                                () -> {
                                                    throw new IllegalStateException("boom");
                                                })
                                        .toCompletableFuture()
                                        .join());

        assertEquals("boom", error.getCause().getMessage());
        lane.closeAsync().toCompletableFuture().get(3, TimeUnit.SECONDS);
    }

    @Test
    void runAsyncKeepsServingAfterAWorkItemThrows() throws Exception {
        ZLinkStateLane lane = new ZLinkStateLane();

        assertThrows(
                CompletionException.class,
                () ->
                        lane.runAsync(
                                        () -> {
                                            throw new IllegalStateException();
                                        })
                                .toCompletableFuture()
                                .join());

        assertEquals(7, lane.runAsync(() -> 7).toCompletableFuture().get(3, TimeUnit.SECONDS));
        lane.closeAsync().toCompletableFuture().get(3, TimeUnit.SECONDS);
    }

    @Test
    void dependentContinuationCanReenterTheSameLaneAfterRunAsyncCompletes() throws Exception {
        ZLinkStateLane lane = new ZLinkStateLane();

        assertEquals(
                42,
                lane.runAsync(() -> 1)
                        .thenApply(ignored -> lane.runAsync(() -> 42).toCompletableFuture().join())
                        .toCompletableFuture()
                        .get(3, TimeUnit.SECONDS));

        lane.closeAsync().toCompletableFuture().get(3, TimeUnit.SECONDS);
    }

    @Test
    void concurrentCallersMutateUnsynchronizedStateWithoutLosingUpdates() throws Exception {
        ZLinkStateLane lane = new ZLinkStateLane();
        Map<Integer, Integer> state = new HashMap<>();
        int callers = 32;
        int perCaller = 50;
        List<CompletableFuture<Void>> work = new ArrayList<>();

        for (int caller = 0; caller < callers; caller++) {
            int callerNumber = caller;
            work.add(
                    CompletableFuture.runAsync(
                            () -> {
                                for (int item = 0; item < perCaller; item++) {
                                    int key = callerNumber * perCaller + item;
                                    lane.runAsync(() -> state.put(key, key))
                                            .toCompletableFuture()
                                            .join();
                                }
                            }));
        }

        CompletableFuture.allOf(work.toArray(CompletableFuture[]::new)).get(10, TimeUnit.SECONDS);
        assertEquals(
                callers * perCaller,
                lane.runAsync(state::size).toCompletableFuture().get(3, TimeUnit.SECONDS));
        lane.closeAsync().toCompletableFuture().get(3, TimeUnit.SECONDS);
    }

    @Test
    void workItemsNeverOverlap() throws Exception {
        ZLinkStateLane lane = new ZLinkStateLane();
        AtomicInteger inFlight = new AtomicInteger();
        AtomicBoolean observedOverlap = new AtomicBoolean();
        List<CompletableFuture<Void>> work = new ArrayList<>();

        for (int index = 0; index < 64; index++) {
            work.add(
                    CompletableFuture.runAsync(
                            () ->
                                    lane.runAsync(
                                                    () -> {
                                                        if (inFlight.incrementAndGet() != 1) {
                                                            observedOverlap.set(true);
                                                        }
                                                        for (int spin = 0; spin < 200; spin++) {
                                                            Thread.onSpinWait();
                                                        }
                                                        inFlight.decrementAndGet();
                                                    })
                                            .toCompletableFuture()
                                            .join()));
        }

        CompletableFuture.allOf(work.toArray(CompletableFuture[]::new)).get(10, TimeUnit.SECONDS);
        assertFalse(observedOverlap.get());
        lane.closeAsync().toCompletableFuture().get(3, TimeUnit.SECONDS);
    }

    @Test
    void postsFromOneCallerRunInPostOrder() throws Exception {
        ZLinkStateLane lane = new ZLinkStateLane();
        List<Integer> order = new ArrayList<>();

        for (int value = 0; value < 100; value++) {
            int posted = value;
            assertTrue(
                    lane.tryPost(
                            () -> {
                                order.add(posted);
                                return CompletableFuture.completedFuture(null);
                            }));
        }

        assertEquals(
                List.of(java.util.stream.IntStream.range(0, 100).boxed().toArray(Integer[]::new)),
                lane.runAsync(() -> List.copyOf(order))
                        .toCompletableFuture()
                        .get(3, TimeUnit.SECONDS));
        lane.closeAsync().toCompletableFuture().get(3, TimeUnit.SECONDS);
    }

    @Test
    void drainingMoreThanOneBatchStillRunsEveryItem() throws Exception {
        ZLinkStateLane lane = new ZLinkStateLane();
        AtomicInteger count = new AtomicInteger();

        for (int index = 0; index < 250; index++) {
            assertTrue(
                    lane.tryPost(
                            () -> {
                                count.incrementAndGet();
                                return CompletableFuture.completedFuture(null);
                            }));
        }

        assertEquals(250, lane.runAsync(count::get).toCompletableFuture().get(3, TimeUnit.SECONDS));
        lane.closeAsync().toCompletableFuture().get(3, TimeUnit.SECONDS);
    }

    @Test
    void reenteringTheSameLaneFailsInsteadOfHanging() throws Exception {
        ZLinkStateLane lane = new ZLinkStateLane();

        IllegalStateException error =
                lane.runAsync(
                                () ->
                                        assertThrows(
                                                IllegalStateException.class,
                                                () -> lane.runAsync(() -> 1)))
                        .toCompletableFuture()
                        .get(3, TimeUnit.SECONDS);

        assertTrue(error.getMessage().contains("already runs on the state lane"));
        lane.closeAsync().toCompletableFuture().get(3, TimeUnit.SECONDS);
    }

    @Test
    void isOnLaneIsTrueOnlyInsideATurn() throws Exception {
        ZLinkStateLane lane = new ZLinkStateLane();

        assertFalse(lane.isOnLane());
        assertTrue(lane.runAsync(lane::isOnLane).toCompletableFuture().get(3, TimeUnit.SECONDS));
        assertFalse(lane.isOnLane());
        lane.closeAsync().toCompletableFuture().get(3, TimeUnit.SECONDS);
    }

    @Test
    void aDifferentLaneIsEnterableFromInsideATurn() throws Exception {
        ZLinkStateLane outer = new ZLinkStateLane();
        ZLinkStateLane inner = new ZLinkStateLane();

        assertEquals(
                5,
                outer.runAsync(() -> inner.runAsync(() -> 5).toCompletableFuture().join())
                        .toCompletableFuture()
                        .get(3, TimeUnit.SECONDS));

        outer.closeAsync().toCompletableFuture().get(3, TimeUnit.SECONDS);
        inner.closeAsync().toCompletableFuture().get(3, TimeUnit.SECONDS);
    }

    @Test
    void closeAsyncWaitsForQueuedWork() throws Exception {
        ZLinkStateLane lane = new ZLinkStateLane();
        AtomicInteger completed = new AtomicInteger();

        for (int index = 0; index < 200; index++) {
            lane.tryPost(
                    () -> {
                        completed.incrementAndGet();
                        return CompletableFuture.completedFuture(null);
                    });
        }

        lane.closeAsync().toCompletableFuture().get(3, TimeUnit.SECONDS);
        assertEquals(200, completed.get());
    }

    @Test
    void runAsyncAfterCloseThrows() throws Exception {
        ZLinkStateLane lane = new ZLinkStateLane();
        lane.closeAsync().toCompletableFuture().get(3, TimeUnit.SECONDS);

        assertThrows(IllegalStateException.class, () -> lane.runAsync(() -> 1));
    }

    @Test
    void tryPostAfterCloseReportsRefusalInsteadOfThrowing() throws Exception {
        ZLinkStateLane lane = new ZLinkStateLane();
        lane.closeAsync().toCompletableFuture().get(3, TimeUnit.SECONDS);

        assertFalse(lane.tryPost(() -> CompletableFuture.completedFuture(null)));
    }

    @Test
    void closeAsyncIsIdempotent() throws Exception {
        ZLinkStateLane lane = new ZLinkStateLane();

        lane.closeAsync().toCompletableFuture().get(3, TimeUnit.SECONDS);
        lane.closeAsync().toCompletableFuture().get(3, TimeUnit.SECONDS);
    }
}
