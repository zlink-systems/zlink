package systems.zlink.framework.execution;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotSame;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ForkJoinPool;
import java.util.concurrent.ForkJoinTask;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/**
 * Spec 01 §11: a wait started outside a runtime execution context (an application thread) resumes
 * in a new process-pool turn, outside the call stack that decided the completion. Dependent actions
 * the application attaches and the thread a blocking wait returns on keep the {@link
 * CompletableFuture} contract.
 */
final class ZLinkOutsideLaneResumptionTest {
    private static final ThreadLocal<Boolean> COMPLETING = new ThreadLocal<>();

    @Test
    void frameworkDeliveredCompletionResumesInDispatcherTurnNotOnCompletingThread()
            throws Exception {
        var source = new CompletableFuture<String>();
        CompletionStage<String> caller = ZLinkSerialExecutionQueue.manageCurrent(source);
        var resumedOn = new AtomicReference<Thread>();
        var insideCompletingCall = new AtomicReference<Boolean>();
        var ownsProducerLock = new AtomicReference<Boolean>();
        var producerLock = new Object();
        var completionPool = new AtomicReference<ForkJoinPool>();
        var observed = new CountDownLatch(1);
        var resumed =
                caller.thenApply(
                        value -> {
                            resumedOn.set(Thread.currentThread());
                            insideCompletingCall.set(Boolean.TRUE.equals(COMPLETING.get()));
                            ownsProducerLock.set(Thread.holdsLock(producerLock));
                            completionPool.set(ForkJoinTask.getPool());
                            observed.countDown();
                            return value;
                        });

        var completer =
                new Thread(
                        () -> {
                            synchronized (producerLock) {
                                COMPLETING.set(true);
                                source.complete("reply");
                                COMPLETING.remove();
                            }
                        },
                        "binding-completion");
        completer.start();
        completer.join();

        assertTrue(observed.await(5, TimeUnit.SECONDS));
        assertEquals("reply", resumed.toCompletableFuture().join());
        assertNotSame(completer, resumedOn.get());
        assertSame(ForkJoinPool.commonPool(), completionPool.get());
        assertEquals(false, insideCompletingCall.get());
        assertEquals(false, ownsProducerLock.get());
    }

    @Test
    void failureIsDeliveredInDispatcherTurnToo() throws Exception {
        var source = new CompletableFuture<String>();
        CompletionStage<String> caller = ZLinkSerialExecutionQueue.manageCurrent(source);
        var recoveredOn = new AtomicReference<Thread>();
        var failure = new IllegalStateException("remote error");
        var completionPool = new AtomicReference<ForkJoinPool>();
        var observed = new CountDownLatch(1);
        var recovered =
                caller.exceptionally(
                        error -> {
                            recoveredOn.set(Thread.currentThread());
                            completionPool.set(ForkJoinTask.getPool());
                            observed.countDown();
                            assertSame(failure, error);
                            return "recovered";
                        });

        var completer =
                new Thread(() -> source.completeExceptionally(failure), "binding-completion");
        completer.start();
        completer.join();

        assertTrue(observed.await(5, TimeUnit.SECONDS));
        assertEquals("recovered", recovered.toCompletableFuture().join());
        assertNotSame(completer, recoveredOn.get());
        assertSame(ForkJoinPool.commonPool(), completionPool.get());
    }

    @Test
    void unrelatedCompletionDoesNotWaitForTheFirstDependentToReturn() throws Exception {
        var first = new CompletableFuture<String>();
        var second = new CompletableFuture<String>();
        var release = new CompletableFuture<Void>();
        var entered = new CountDownLatch(1);
        var delivered = new CountDownLatch(1);
        var firstResult =
                ZLinkSerialExecutionQueue.manageCurrent(first)
                        .thenRun(
                                () -> {
                                    entered.countDown();
                                    release.join();
                                });
        var secondResult =
                ZLinkSerialExecutionQueue.manageCurrent(second).thenRun(delivered::countDown);
        var firstCompleter = new Thread(() -> first.complete("first"), "first-completion");
        var secondCompleter = new Thread(() -> second.complete("second"), "second-completion");
        firstCompleter.start();
        try {
            assertTrue(entered.await(5, TimeUnit.SECONDS));
            secondCompleter.start();
            assertTrue(delivered.await(5, TimeUnit.SECONDS));
        } finally {
            release.complete(null);
            firstCompleter.join();
            secondCompleter.join();
        }
        firstResult.toCompletableFuture().join();
        secondResult.toCompletableFuture().join();
    }

    @Test
    void applicationAsyncDependentRunsOnTheExecutorTheApplicationChose() throws Exception {
        var source = new CompletableFuture<String>();
        CompletionStage<String> caller = ZLinkSerialExecutionQueue.manageCurrent(source);
        var ownerObservedOn = new AtomicReference<Thread>();
        source.whenComplete((value, failure) -> ownerObservedOn.set(Thread.currentThread()));
        var chosen = new AtomicReference<Thread>();
        var ranOn = new AtomicReference<Thread>();
        var resumed =
                caller.thenApplyAsync(
                        value -> {
                            ranOn.set(Thread.currentThread());
                            return value;
                        },
                        task -> {
                            var thread = new Thread(task, "application-executor");
                            chosen.set(thread);
                            thread.start();
                            try {
                                thread.join();
                            } catch (InterruptedException interrupted) {
                                Thread.currentThread().interrupt();
                                throw new java.util.concurrent.CompletionException(interrupted);
                            }
                        });

        var completer = new Thread(() -> source.complete("reply"), "binding-completion");
        completer.start();
        try {
            assertEquals("reply", resumed.toCompletableFuture().get(5, TimeUnit.SECONDS));
            assertSame(chosen.get(), ranOn.get());
            assertSame(completer, ownerObservedOn.get());
        } finally {
            completer.join();
            if (chosen.get() != null) chosen.get().join();
        }
    }

    @Test
    void dependentAttachedAfterDeliveryRunsOnTheAttachingThread() throws Exception {
        var source = new CompletableFuture<String>();
        CompletionStage<String> caller = ZLinkSerialExecutionQueue.manageCurrent(source);
        var completer = new Thread(() -> source.complete("reply"), "binding-completion");
        completer.start();
        try {
            caller.toCompletableFuture().get(5, TimeUnit.SECONDS);
        } finally {
            completer.join();
        }

        var ranOn = new AtomicReference<Thread>();
        caller.thenRun(() -> ranOn.set(Thread.currentThread()));

        assertSame(Thread.currentThread(), ranOn.get());
    }

    @Test
    void blockingWaitReturnsOnTheWaitingThread() throws Exception {
        var source = new CompletableFuture<String>();
        CompletionStage<String> caller = ZLinkSerialExecutionQueue.manageCurrent(source);
        var started = new CountDownLatch(1);
        var completer =
                new Thread(
                        () -> {
                            try {
                                started.await();
                            } catch (InterruptedException interrupted) {
                                Thread.currentThread().interrupt();
                            }
                            source.complete("reply");
                        },
                        "binding-completion");
        completer.start();
        var waiter = Thread.currentThread();
        started.countDown();

        try {
            assertEquals("reply", caller.toCompletableFuture().join());
            assertSame(waiter, Thread.currentThread());
        } finally {
            completer.join();
        }
    }

    @Test
    void callerCancellationReachesTheSource() {
        var source = new CompletableFuture<String>();
        CompletionStage<String> caller = ZLinkSerialExecutionQueue.manageCurrent(source);

        assertTrue(caller.toCompletableFuture().cancel(false));
        assertTrue(source.isCancelled());
    }

    @Test
    void sourceAlreadyCompleteIsReturnedForTheCallerToObserveAfterReturn() {
        var source = CompletableFuture.completedFuture("reply");
        assertSame(source, ZLinkSerialExecutionQueue.manageCurrent(source));
    }
}
