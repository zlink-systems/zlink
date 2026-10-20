package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.execution.ZLinkExecutionLanePolicy;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.execution.ZLinkWorkerPool;
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkYieldAdmissionTest {
    @Test
    void secondYieldRejectsBeforeActualIoSubmission() {
        var executor = new ZLinkManualExecutor();
        var queue = new ZLinkSerialExecutionQueue(executor, ZLinkExecutionLanePolicy.spot());
        var firstRemote = new CompletableFuture<Integer>();
        var submissions = new AtomicInteger();
        var rejected = new AtomicReference<Throwable>();
        try (var pool = new ZLinkWorkerPool(0, 1, Duration.ofSeconds(30))) {
            var turn =
                    queue.enqueue(
                            () -> {
                                try (var scope =
                                        ZLinkSuspendInvocationContext.enterApplicationExecution(
                                                application())) {
                                    var first = call(pool, submissions, firstRemote).yield();
                                    try {
                                        call(
                                                        pool,
                                                        submissions,
                                                        CompletableFuture.completedFuture(2))
                                                .yield();
                                    } catch (ZLinkFrameworkException error) {
                                        rejected.set(error);
                                    }
                                    return first.thenAccept(value -> assertEquals(1, value));
                                }
                            },
                            null);
            try {
                executor.drain();
            } finally {
                firstRemote.complete(1);
                executor.drain();
            }
            assertTrue(turn.toCompletableFuture().isDone());
            turn.toCompletableFuture().join();
            assertEquals(
                    1, submissions.get(), "actual I/O submissions before second-call rejection");
            assertInvalid(rejected.get());
        } finally {
            firstRemote.complete(1);
            executor.drain();
            queue.close();
        }
    }

    @Test
    void reentrantYieldRejectsWhileFirstSubmissionIsStillRunning() {
        var executor = new ZLinkManualExecutor();
        var queue = new ZLinkSerialExecutionQueue(executor, ZLinkExecutionLanePolicy.spot());
        var submissions = new AtomicInteger();
        var rejected = new AtomicReference<Throwable>();
        try (var pool = new ZLinkWorkerPool(0, 1, Duration.ofSeconds(30))) {
            var turn =
                    queue.enqueue(
                            () -> {
                                try (var scope =
                                        ZLinkSuspendInvocationContext.enterApplicationExecution(
                                                application())) {
                                    return new DefaultZLinkIoWorkerCall<Integer>(
                                                    pool,
                                                    signal -> {
                                                        submissions.incrementAndGet();
                                                        try {
                                                            call(
                                                                            pool,
                                                                            submissions,
                                                                            CompletableFuture
                                                                                    .completedFuture(
                                                                                            2))
                                                                    .yield();
                                                        } catch (ZLinkFrameworkException error) {
                                                            rejected.set(error);
                                                        }
                                                        return CompletableFuture.completedFuture(1);
                                                    })
                                            .yield()
                                            .thenAccept(value -> assertEquals(1, value));
                                }
                            },
                            null);
            executor.drain();
            assertTrue(turn.toCompletableFuture().isDone());
            turn.toCompletableFuture().join();
            assertEquals(1, submissions.get(), "actual reentrant I/O submissions");
            assertInvalid(rejected.get());
        } finally {
            executor.drain();
            queue.close();
        }
    }

    @Test
    void closedTurnCarrierRejectsBeforeActualIoSubmission() {
        var executor = new ZLinkManualExecutor();
        var queue = new ZLinkSerialExecutionQueue(executor, ZLinkExecutionLanePolicy.spot());
        var tail = new AtomicReference<Runnable>();
        var submissions = new AtomicInteger();
        var rejected = new AtomicReference<Throwable>();
        try (var pool = new ZLinkWorkerPool(0, 1, Duration.ofSeconds(30))) {
            var turn =
                    queue.enqueue(
                            () -> {
                                try (var scope =
                                        ZLinkSuspendInvocationContext.enterApplicationExecution(
                                                application())) {
                                    ZLinkSerialExecutionQueue.propagateCurrent(tail::set)
                                            .execute(
                                                    () -> {
                                                        try {
                                                            call(
                                                                            pool,
                                                                            submissions,
                                                                            CompletableFuture
                                                                                    .completedFuture(
                                                                                            1))
                                                                    .yield();
                                                        } catch (ZLinkFrameworkException error) {
                                                            rejected.set(error);
                                                        }
                                                    });
                                    return CompletableFuture.completedFuture(null);
                                }
                            },
                            null);
            executor.drain();
            turn.toCompletableFuture().join();
            tail.get().run();
            executor.drain();
            assertEquals(0, submissions.get(), "actual I/O submissions after owner turn closed");
            assertInvalid(rejected.get());
        } finally {
            executor.drain();
            queue.close();
        }
    }

    @Test
    void resumedNewTurnMayYieldAgain() {
        var executor = new ZLinkManualExecutor();
        var queue = new ZLinkSerialExecutionQueue(executor, ZLinkExecutionLanePolicy.spot());
        var first = new CompletableFuture<Integer>();
        var second = new CompletableFuture<Integer>();
        var submissions = new AtomicInteger();
        try (var pool = new ZLinkWorkerPool(0, 1, Duration.ofSeconds(30))) {
            var turn =
                    queue.enqueue(
                            () -> {
                                try (var scope =
                                        ZLinkSuspendInvocationContext.enterApplicationExecution(
                                                application())) {
                                    return call(pool, submissions, first)
                                            .yield()
                                            .thenCompose(
                                                    value ->
                                                            call(pool, submissions, second).yield())
                                            .thenAccept(value -> assertEquals(2, value));
                                }
                            },
                            null);
            try {
                executor.drain();
                assertEquals(1, submissions.get());
                first.complete(1);
                executor.drain();
                assertEquals(2, submissions.get());
            } finally {
                first.complete(1);
                second.complete(2);
                executor.drain();
            }
            assertTrue(turn.toCompletableFuture().isDone());
            turn.toCompletableFuture().join();
        } finally {
            first.complete(1);
            second.complete(2);
            executor.drain();
            queue.close();
        }
    }

    @Test
    void releasedOriginalCarrierRejectsWhileHandlerOriginIsPending() {
        var executor = new ZLinkManualExecutor();
        var queue = new ZLinkSerialExecutionQueue(executor, ZLinkExecutionLanePolicy.spot());
        var remote = new CompletableFuture<Integer>();
        var tail = new AtomicReference<Runnable>();
        var submissions = new AtomicInteger();
        var rejected = new AtomicReference<Throwable>();
        try (var pool = new ZLinkWorkerPool(0, 1, Duration.ofSeconds(30))) {
            var turn =
                    queue.enqueue(
                            () -> {
                                try (var scope =
                                        ZLinkSuspendInvocationContext.enterApplicationExecution(
                                                application())) {
                                    ZLinkSerialExecutionQueue.propagateCurrent(tail::set)
                                            .execute(
                                                    () -> {
                                                        try {
                                                            call(
                                                                            pool,
                                                                            submissions,
                                                                            CompletableFuture
                                                                                    .completedFuture(
                                                                                            2))
                                                                    .yield();
                                                        } catch (ZLinkFrameworkException error) {
                                                            rejected.set(error);
                                                        }
                                                    });
                                    return call(pool, submissions, remote)
                                            .yield()
                                            .thenApply(value -> null);
                                }
                            },
                            null);
            try {
                executor.drain();
                assertFalse(turn.toCompletableFuture().isDone());
                tail.get().run();
            } finally {
                remote.complete(1);
                executor.drain();
            }
            assertTrue(turn.toCompletableFuture().isDone());
            turn.toCompletableFuture().join();
            assertEquals(
                    1, submissions.get(), "actual I/O submissions from released original carrier");
            assertInvalid(rejected.get());
        } finally {
            remote.complete(1);
            executor.drain();
            queue.close();
        }
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void readyResultStillResumesInANewQueueTurn(boolean shared) {
        var executor = new ZLinkManualExecutor();
        var queue = new ZLinkSerialExecutionQueue(executor, ZLinkExecutionLanePolicy.spot());
        if (shared) new ZLinkSerialExecutionQueue.SharedSpotGate(queue);
        var order = new java.util.ArrayList<String>();
        var submissions = new AtomicInteger();
        var rejected = new AtomicReference<Throwable>();
        var turn =
                queue.enqueue(
                        () -> {
                            try (var scope =
                                    ZLinkSuspendInvocationContext.enterApplicationExecution(
                                            application())) {
                                queue.enqueue(
                                        () -> {
                                            order.add("queued");
                                            return CompletableFuture.completedFuture(null);
                                        },
                                        null);
                                ZLinkSuspendInvocationContext.requireYieldAllowed(
                                        "test ready request");
                                submissions.incrementAndGet();
                                var yielded =
                                        ZLinkSerialExecutionQueue.yieldCurrent(
                                                CompletableFuture.completedFuture(1));
                                try {
                                    ZLinkSuspendInvocationContext.requireYieldAllowed(
                                            "second ready request");
                                    submissions.incrementAndGet();
                                    ZLinkSerialExecutionQueue.yieldCurrent(
                                            CompletableFuture.completedFuture(2));
                                } catch (ZLinkFrameworkException error) {
                                    rejected.set(error);
                                }
                                return yielded.thenAccept(value -> order.add("resumed"));
                            }
                        },
                        null);
        try {
            executor.drain();
        } finally {
            queue.close();
        }
        assertTrue(turn.toCompletableFuture().isDone());
        turn.toCompletableFuture().join();
        assertEquals(java.util.List.of("queued", "resumed"), order);
        assertEquals(1, submissions.get());
        assertInvalid(rejected.get());
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void lifecycleBurstYieldsToResumedApplicationTurn(boolean shared) {
        var executor = new ZLinkManualExecutor();
        var queue =
                new ZLinkSerialExecutionQueue(
                        executor,
                        ZLinkExecutionLanePolicy.spot(),
                        1,
                        ZLinkSerialExecutionQueue.DEFAULT_OWNER_TIME_BUDGET);
        if (shared) new ZLinkSerialExecutionQueue.SharedSpotGate(queue);
        var remote = new CompletableFuture<Integer>();
        var order = new java.util.ArrayList<String>();
        var turn =
                queue.enqueue(
                        () -> {
                            try (var scope =
                                    ZLinkSuspendInvocationContext.enterApplicationExecution(
                                            application())) {
                                ZLinkSuspendInvocationContext.requireYieldAllowed("test request");
                                return ZLinkSerialExecutionQueue.yieldCurrent(remote)
                                        .thenAccept(value -> order.add("app-resume"));
                            }
                        },
                        null);
        try {
            executor.drain();
            queue.enqueueLifecycleBarrier(
                    () -> {
                        order.add("lifecycle-1");
                        queue.enqueueLifecycleBarrier(
                                () -> {
                                    order.add("lifecycle-2");
                                    return CompletableFuture.completedFuture(null);
                                });
                        remote.complete(1);
                        return CompletableFuture.completedFuture(null);
                    });
            executor.drain();
        } finally {
            remote.complete(1);
            executor.drain();
            queue.close();
        }
        assertTrue(turn.toCompletableFuture().isDone());
        turn.toCompletableFuture().join();
        assertEquals(java.util.List.of("lifecycle-1", "app-resume", "lifecycle-2"), order);
    }

    @Test
    void deferredLifecycleCallbackKeepsItsTurnUntilActualCallbackYields() {
        var executor = new ZLinkManualExecutor();
        var callbacks = new ZLinkManualExecutor();
        var queue = new ZLinkSerialExecutionQueue(executor, ZLinkExecutionLanePolicy.spot());
        var callbackResult = new CompletableFuture<Void>();
        var remote = new CompletableFuture<Integer>();
        var submissions = new AtomicInteger();
        var siblingRuns = new AtomicInteger();
        try (var pool = new ZLinkWorkerPool(0, 1, Duration.ofSeconds(30))) {
            var turn =
                    queue.enqueueLifecycleBarrier(
                            () -> {
                                try (var scope =
                                        ZLinkSuspendInvocationContext.enterApplicationExecution(
                                                application())) {
                                    ZLinkSerialExecutionQueue.deferCurrentReleaseUntil(
                                            callbackResult);
                                    ZLinkSerialExecutionQueue.propagateCurrent(callbacks)
                                            .execute(
                                                    () -> {
                                                        try {
                                                            call(pool, submissions, remote)
                                                                    .yield()
                                                                    .whenComplete(
                                                                            (value, error) -> {
                                                                                if (error != null)
                                                                                    callbackResult
                                                                                            .completeExceptionally(
                                                                                                    error);
                                                                                else
                                                                                    callbackResult
                                                                                            .complete(
                                                                                                    null);
                                                                            });
                                                        } catch (RuntimeException error) {
                                                            callbackResult.completeExceptionally(
                                                                    error);
                                                        }
                                                    });
                                    return callbackResult;
                                }
                            });
            var sibling =
                    queue.enqueue(
                            () -> {
                                siblingRuns.incrementAndGet();
                                return CompletableFuture.completedFuture(null);
                            },
                            null);
            try {
                executor.drain();
                assertEquals(0, siblingRuns.get(), "reserved callback still owns its gate");
                assertEquals(0, submissions.get(), "callback has not started");
                callbacks.drain();
                assertEquals(1, submissions.get(), "first public Yield accepted on live owner");
                executor.drain();
                assertEquals(1, siblingRuns.get(), "explicit Yield releases reserved gate");
                assertFalse(turn.toCompletableFuture().isDone());
            } finally {
                callbacks.drain();
                remote.complete(1);
                executor.drain();
            }
            assertTrue(turn.toCompletableFuture().isDone());
            turn.toCompletableFuture().join();
            sibling.toCompletableFuture().join();
        } finally {
            callbacks.drain();
            remote.complete(1);
            executor.drain();
            queue.close();
        }
    }

    private static DefaultZLinkIoWorkerCall<Integer> call(
            ZLinkWorkerPool pool, AtomicInteger submissions, CompletionStage<Integer> remote) {
        return new DefaultZLinkIoWorkerCall<>(
                pool,
                signal -> {
                    submissions.incrementAndGet();
                    return remote;
                });
    }

    private static ZLinkSuspendInvocationContext.ApplicationExecution application() {
        return new ZLinkSuspendInvocationContext.ApplicationExecution(
                "room-1", null, true, true, ignored -> false);
    }

    private static void assertInvalid(Throwable error) {
        assertInstanceOf(ZLinkFrameworkException.class, error);
        assertEquals(
                ZLinkFrameworkErrorKind.INVALID_OPERATION,
                ((ZLinkFrameworkException) error).kind());
    }
}
