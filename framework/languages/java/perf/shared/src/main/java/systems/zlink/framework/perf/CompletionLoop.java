package systems.zlink.framework.perf;

import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executor;
import java.util.concurrent.ForkJoinPool;
import java.util.concurrent.atomic.AtomicLong;
import java.util.function.BiConsumer;
import java.util.function.IntFunction;
import java.util.function.Supplier;

/**
 * Runs sequential asynchronous operations; a stage that is already complete continues the loop
 * instead of re-entering it.
 */
public final class CompletionLoop {
    private CompletionLoop() {}

    /** One operation and the recording its scenario does when the operation completes. */
    public record Iteration<T>(
            CompletionStage<T> stage, BiConsumer<? super T, ? super Throwable> completed) {
        public Iteration {
            Objects.requireNonNull(stage);
            Objects.requireNonNull(completed);
        }
    }

    public static <T> void run(
            CompletableFuture<Void> done, Supplier<Optional<Iteration<T>>> next) {
        run(done, ForkJoinPool.commonPool(), next);
    }

    public static <T> void run(
            CompletableFuture<Void> done,
            Executor executor,
            Supplier<Optional<Iteration<T>>> next) {
        Objects.requireNonNull(done);
        Objects.requireNonNull(executor);
        Objects.requireNonNull(next);
        advance(done, executor, next);
    }

    /**
     * Continuously submits request calls across logical streams and completes after every submitted
     * call settles.
     */
    public static <T> void runRequests(
            CompletableFuture<Void> done, int streams, IntFunction<Optional<Iteration<T>>> next) {
        Objects.requireNonNull(done);
        Objects.requireNonNull(next);
        if (streams <= 0) {
            throw new IllegalArgumentException(
                    "A request loop must have at least one logical stream.");
        }
        AtomicLong outstanding = new AtomicLong(1);
        try {
            ForkJoinPool.commonPool()
                    .execute(
                            () -> {
                                int stream = 0;
                                try {
                                    while (!done.isDone()) {
                                        Optional<Iteration<T>> iteration = next.apply(stream);
                                        if (iteration.isEmpty()) {
                                            break;
                                        }
                                        Iteration<T> current = iteration.get();
                                        outstanding.incrementAndGet();
                                        current.stage()
                                                .whenComplete(
                                                        (value, error) -> {
                                                            try {
                                                                current.completed()
                                                                        .accept(value, error);
                                                            } catch (Throwable failure) {
                                                                done.completeExceptionally(failure);
                                                            } finally {
                                                                if (outstanding.decrementAndGet()
                                                                        == 0) {
                                                                    done.complete(null);
                                                                }
                                                            }
                                                        });
                                        if (++stream == streams) {
                                            stream = 0;
                                            Thread.yield();
                                        }
                                    }
                                } catch (Throwable failure) {
                                    done.completeExceptionally(failure);
                                } finally {
                                    if (outstanding.decrementAndGet() == 0) {
                                        done.complete(null);
                                    }
                                }
                            });
        } catch (Throwable failure) {
            done.completeExceptionally(failure);
        }
    }

    private static <T> void advance(
            CompletableFuture<Void> done,
            Executor executor,
            Supplier<Optional<Iteration<T>>> next) {
        while (!done.isDone()) {
            Optional<Iteration<T>> iteration;
            try {
                iteration = next.get();
            } catch (Throwable error) {
                done.completeExceptionally(error);
                return;
            }
            if (iteration.isEmpty()) {
                done.complete(null);
                return;
            }

            Iteration<T> current = iteration.get();
            boolean alreadyCompleted = current.stage().toCompletableFuture().isDone();
            current.stage()
                    .whenComplete(
                            (value, error) -> {
                                try {
                                    current.completed().accept(value, error);
                                } catch (Throwable failure) {
                                    done.completeExceptionally(failure);
                                    return;
                                }
                                if (!alreadyCompleted && !done.isDone()) {
                                    try {
                                        executor.execute(() -> advance(done, executor, next));
                                    } catch (Throwable failure) {
                                        done.completeExceptionally(failure);
                                    }
                                }
                            });
            if (!alreadyCompleted) {
                return;
            }
        }
    }
}
