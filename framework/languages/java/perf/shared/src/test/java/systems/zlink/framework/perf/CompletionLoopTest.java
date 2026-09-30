package systems.zlink.framework.perf;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotSame;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

import org.junit.jupiter.api.Test;

final class CompletionLoopTest {
    @Test
    void immediatelyCompletedStagesContinueWithoutGrowingTheStack() {
        CompletableFuture<Void> done = new CompletableFuture<>();
        AtomicInteger issued = new AtomicInteger();
        AtomicInteger completed = new AtomicInteger();
        CompletionLoop.run(done, Runnable::run, () -> {
            if (issued.get() == 10_000) {
                return Optional.empty();
            }
            issued.incrementAndGet();
            return Optional.of(new CompletionLoop.Iteration<>(CompletableFuture.completedFuture(null),
                    (ignored, error) -> completed.incrementAndGet()));
        });

        assertTrue(done.isDone());
        done.join();
        assertEquals(10_000, issued.get());
        assertEquals(10_000, completed.get());
    }

    @Test
    void incompleteStageResumesOnExecutorWithoutRecursion() throws Exception {
        CompletableFuture<Void> stage = new CompletableFuture<>();
        CompletableFuture<Void> done = new CompletableFuture<>();
        AtomicInteger issued = new AtomicInteger();
        AtomicInteger completed = new AtomicInteger();
        AtomicReference<Thread> resumedThread = new AtomicReference<>();
        ExecutorService executor = Executors.newSingleThreadExecutor();
        Thread callerThread = Thread.currentThread();
        try {
            CompletionLoop.run(done, executor, () -> {
                if (issued.getAndIncrement() == 0) {
                    return Optional.of(new CompletionLoop.Iteration<>(stage, (ignored, error) -> {
                        if (error == null) {
                            completed.incrementAndGet();
                        }
                    }));
                }
                resumedThread.set(Thread.currentThread());
                return Optional.empty();
            });
            assertEquals(1, issued.get());

            stage.complete(null);
            done.get(5, TimeUnit.SECONDS);

            assertEquals(1, completed.get());
            assertNotSame(callerThread, resumedThread.get());
        } finally {
            executor.shutdownNow();
        }
    }

    @Test
    void completionRecordingFailureCompletesTheLoopExceptionally() {
        CompletableFuture<Void> done = new CompletableFuture<>();
        IllegalStateException expected = new IllegalStateException("completion recording failed");
        CompletionLoop.run(done, Runnable::run, () -> Optional.of(new CompletionLoop.Iteration<>(
                CompletableFuture.completedFuture(null), (ignored, error) -> { throw expected; })));

        CompletionException failure = assertThrows(CompletionException.class, done::join);
        assertSame(expected, failure.getCause());
    }

    @Test
    void recursiveCompletionCallbackOverflowsBeforeTenThousandStages() {
        AtomicInteger issued = new AtomicInteger();
        AtomicBoolean overflowed = new AtomicBoolean();
        recursivelyComplete(10_000, issued, overflowed);

        assertTrue(overflowed.get());
        assertTrue(issued.get() < 10_000);
    }

    private static void recursivelyComplete(int remaining, AtomicInteger issued, AtomicBoolean overflowed) {
        if (remaining == 0) {
            return;
        }
        issued.incrementAndGet();
        CompletableFuture.<Void>completedFuture(null).whenComplete((ignored, error) -> {
            try {
                recursivelyComplete(remaining - 1, issued, overflowed);
            } catch (StackOverflowError failure) {
                overflowed.set(true);
            }
        });
    }

}
