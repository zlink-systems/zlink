package systems.zlink.framework.runtime.internal.calls;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ForkJoinPool;
import java.util.concurrent.ForkJoinTask;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.BiConsumer;

class ZLinkRequestStagesTest {
    @Test
    void successfulReplyCompletesMissingAdmissionObservation() {
        var reply = new CompletableFuture<String>();
        var admission = new CompletableFuture<Void>();
        var stage = ZLinkRequestStages.observe(reply, admission);
        assertFalse(stage.admitted().toCompletableFuture().isDone());
        reply.complete("reply");
        assertTrue(ZLinkRequestStages.ownerAdmission(stage).toCompletableFuture().isDone());
        assertNull(stage.admitted().toCompletableFuture().join());
        assertEquals("reply", stage.toCompletableFuture().join());
    }

    @Test
    void successfulReplyDoesNotReplaceEarlierAdmissionFailure() {
        var reply = new CompletableFuture<String>();
        var admission = new CompletableFuture<Void>();
        var stage = ZLinkRequestStages.observe(reply, admission);
        var failure = new IllegalStateException("admission failed");
        admission.completeExceptionally(failure);
        reply.complete("reply");
        var observed =
                assertThrows(
                        java.util.concurrent.CompletionException.class,
                        () -> stage.admitted().toCompletableFuture().join());
        assertSame(failure, observed.getCause());
    }

    @Test
    void outsideLaneAdmissionObservationResumesInDispatcherTurn() throws Exception {
        var reply = new CompletableFuture<String>();
        var admission = new CompletableFuture<Void>();
        var stage = ZLinkRequestStages.observe(reply, admission);
        var resumedOn = new AtomicReference<Thread>();
        var completionPool = new AtomicReference<ForkJoinPool>();
        var completedOnStack = new AtomicReference<Boolean>();
        var completed = new CountDownLatch(1);
        // Attach while pending, then wait on the latch so no observing thread can help completion.
        var observed =
                stage.admitted()
                        .thenRun(
                                () -> {
                                    resumedOn.set(Thread.currentThread());
                                    completionPool.set(ForkJoinTask.getPool());
                                    completedOnStack.set(
                                            StackWalker.getInstance()
                                                    .walk(
                                                            frames ->
                                                                    frames.anyMatch(
                                                                            frame ->
                                                                                    frame.getClassName()
                                                                                                    .equals(
                                                                                                            CompletableFuture
                                                                                                                    .class
                                                                                                                    .getName())
                                                                                            && frame.getMethodName()
                                                                                                    .equals(
                                                                                                            "complete"))));
                                    completed.countDown();
                                });
        var completer = new Thread(() -> reply.complete("reply"), "binding-completion");
        completer.start();
        completer.join();

        assertTrue(completed.await(5, TimeUnit.SECONDS));
        observed.toCompletableFuture().join();
        assertNotSame(completer, resumedOn.get());
        assertSame(ForkJoinPool.commonPool(), completionPool.get());
        assertTrue(completedOnStack.get());
    }

    @Test
    void concurrentAdmissionObserversShareOneDeliveryRegistration() throws Exception {
        class Admission extends CompletableFuture<Void> {
            final CountDownLatch entered = new CountDownLatch(1);
            final CountDownLatch release = new CountDownLatch(1);
            final AtomicInteger registrations = new AtomicInteger();
            boolean observingDelivery;

            @Override
            public CompletableFuture<Void> whenComplete(
                    BiConsumer<? super Void, ? super Throwable> callback) {
                if (observingDelivery && registrations.incrementAndGet() == 1) {
                    entered.countDown();
                    try {
                        release.await();
                    } catch (InterruptedException interrupted) {
                        Thread.currentThread().interrupt();
                        throw new java.util.concurrent.CompletionException(interrupted);
                    }
                }
                return super.whenComplete(callback);
            }
        }
        var reply = new CompletableFuture<String>();
        var admission = new Admission();
        var stage = ZLinkRequestStages.observe(reply, admission);
        admission.observingDelivery = true;
        var first = new FutureTask<CompletionStage<Void>>(stage::admitted);
        var second = new FutureTask<CompletionStage<Void>>(stage::admitted);
        var firstObserver = new Thread(first, "first-admission-observer");
        var secondObserver = new Thread(second, "second-admission-observer");
        firstObserver.start();
        try {
            assertTrue(admission.entered.await(5, TimeUnit.SECONDS));
            secondObserver.start();
            second.get(5, TimeUnit.SECONDS);
        } finally {
            admission.release.countDown();
            firstObserver.join();
            secondObserver.join();
            reply.complete("reply");
        }
        var firstView = first.get(5, TimeUnit.SECONDS);
        assertSame(firstView, second.get(5, TimeUnit.SECONDS));
        assertNull(firstView.toCompletableFuture().get(5, TimeUnit.SECONDS));
        assertEquals(1, admission.registrations.get());
    }
}
