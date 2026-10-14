package systems.zlink.framework.runtime.internal;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.function.BiConsumer;
import java.util.function.BiFunction;
import java.util.function.Consumer;
import java.util.function.Function;

/** Bridges caller cancellation to waiter stages and disposes results the caller cannot accept. */
public final class ZLinkCompletionBridge {
    private ZLinkCompletionBridge() {}

    /**
     * Gives the caller its own stage over an owner source. The view commits on the owner's
     * completion, runs caller continuations in a new turn on the standard process executor, and
     * forwards caller cancellation to the owner. Owner observers stay on the owner source, so no
     * thread that runs a caller continuation can reach them.
     */
    public static <T> CompletableFuture<T> applicationView(CompletableFuture<T> owner) {
        var view = new ApplicationView<>(owner);
        owner.whenComplete(
                (value, failure) -> {
                    if (failure == null) view.complete(value);
                    else view.completeExceptionally(failure);
                });
        return view;
    }

    /** Preserves direct failures at existing synchronous state-registration boundaries. */
    public static <T> T await(CompletionStage<T> completion) {
        try {
            return completion.toCompletableFuture().join();
        } catch (CompletionException failure) {
            if (failure.getCause() instanceof RuntimeException runtime) throw runtime;
            if (failure.getCause() instanceof Error error) throw error;
            throw failure;
        }
    }

    public static void forwardCancellation(CompletableFuture<?> caller, CompletionStage<?> waiter) {
        forwardCancellation(caller, () -> waiter.toCompletableFuture().cancel(false));
    }

    public static void forwardCancellation(
            CompletableFuture<?> caller, CompletionStage<?> first, CompletionStage<?> second) {
        forwardCancellation(
                caller,
                () -> {
                    first.toCompletableFuture().cancel(false);
                    if (second != null) second.toCompletableFuture().cancel(false);
                });
    }

    /** Runs the forwarding action when cancellation must be scheduled on the waiter's owner. */
    public static void forwardCancellation(CompletableFuture<?> caller, Runnable forward) {
        caller.whenComplete(
                (ignored, failure) -> {
                    if (caller.isCancelled()) forward.run();
                });
    }

    public static <T> void completeOrDiscard(
            CompletableFuture<T> caller, T value, Consumer<? super T> discard) {
        if (!caller.complete(value)) discard.accept(value);
    }

    private static final class ApplicationView<T> extends DispatchedFuture<T> {
        private final CompletableFuture<T> owner;

        private ApplicationView(CompletableFuture<T> owner) {
            this.owner = owner;
        }

        @Override
        public boolean cancel(boolean mayInterruptIfRunning) {
            owner.cancel(mayInterruptIfRunning);
            return super.cancel(mayInterruptIfRunning);
        }
    }

    private static class DispatchedFuture<T> extends CompletableFuture<T> {
        @Override
        public <U> CompletableFuture<U> newIncompleteFuture() {
            return new DispatchedFuture<>();
        }

        @Override
        public <U> CompletableFuture<U> thenApply(Function<? super T, ? extends U> fn) {
            return super.thenApplyAsync(fn);
        }

        @Override
        public CompletableFuture<Void> thenAccept(Consumer<? super T> action) {
            return super.thenAcceptAsync(action);
        }

        @Override
        public CompletableFuture<Void> thenRun(Runnable action) {
            return super.thenRunAsync(action);
        }

        @Override
        public <U, V> CompletableFuture<V> thenCombine(
                CompletionStage<? extends U> other,
                BiFunction<? super T, ? super U, ? extends V> fn) {
            return super.thenCombineAsync(other, fn);
        }

        @Override
        public <U> CompletableFuture<Void> thenAcceptBoth(
                CompletionStage<? extends U> other, BiConsumer<? super T, ? super U> action) {
            return super.thenAcceptBothAsync(other, action);
        }

        @Override
        public CompletableFuture<Void> runAfterBoth(CompletionStage<?> other, Runnable action) {
            return super.runAfterBothAsync(other, action);
        }

        @Override
        public <U> CompletableFuture<U> applyToEither(
                CompletionStage<? extends T> other, Function<? super T, U> fn) {
            return super.applyToEitherAsync(other, fn);
        }

        @Override
        public CompletableFuture<Void> acceptEither(
                CompletionStage<? extends T> other, Consumer<? super T> action) {
            return super.acceptEitherAsync(other, action);
        }

        @Override
        public CompletableFuture<Void> runAfterEither(CompletionStage<?> other, Runnable action) {
            return super.runAfterEitherAsync(other, action);
        }

        @Override
        public <U> CompletableFuture<U> thenCompose(
                Function<? super T, ? extends CompletionStage<U>> fn) {
            return super.thenComposeAsync(fn);
        }

        @Override
        public CompletableFuture<T> exceptionally(Function<Throwable, ? extends T> fn) {
            return super.exceptionallyAsync(fn);
        }

        @Override
        public CompletableFuture<T> exceptionallyCompose(
                Function<Throwable, ? extends CompletionStage<T>> fn) {
            return super.exceptionallyComposeAsync(fn);
        }

        @Override
        public CompletableFuture<T> whenComplete(BiConsumer<? super T, ? super Throwable> action) {
            return super.whenCompleteAsync(action);
        }

        @Override
        public <U> CompletableFuture<U> handle(BiFunction<? super T, Throwable, ? extends U> fn) {
            return super.handleAsync(fn);
        }
    }
}
