package systems.zlink.framework.runtime.internal.calls;

import systems.zlink.framework.channels.ZLinkRequestStage;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.internal.ZLinkCompletionBridge;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executor;
import java.util.function.BiConsumer;
import java.util.function.BiFunction;
import java.util.function.Consumer;
import java.util.function.Function;

/** Connects observations to the existing request and admission owners. */
public final class ZLinkRequestStages {
    private ZLinkRequestStages() {}

    public static CompletableFuture<Void> newAdmission() {
        return ZLinkCompletionBridge.completionSource();
    }

    public static void forwardAdmission(
            CompletableFuture<Void> caller, CompletionStage<Void> owner) {
        if (caller == null) return;
        ZLinkCompletionBridge.whenCompleteOwner(
                owner,
                (ignored, failure) -> {
                    if (failure == null) caller.complete(null);
                });
    }

    public static <T> ZLinkRequestStage<T> observe(
            CompletionStage<T> reply, CompletableFuture<Void> admission) {
        reply.whenComplete(
                (ignored, failure) -> {
                    if (failure == null) admission.complete(null);
                    else admission.completeExceptionally(unwrap(failure));
                });
        ZLinkCompletionBridge.forwardCancellation(admission, reply);
        return new RequestStage<>(reply, ZLinkSerialExecutionQueue.manageCurrent(admission));
    }

    private static Throwable unwrap(Throwable failure) {
        while (failure instanceof CompletionException && failure.getCause() != null)
            failure = failure.getCause();
        return failure;
    }

    private static final class RequestStage<T> implements ZLinkRequestStage<T> {
        private final CompletionStage<T> reply;
        private final CompletionStage<Void> admission;

        private RequestStage(CompletionStage<T> reply, CompletionStage<Void> admission) {
            this.reply = reply;
            this.admission = admission;
        }

        @Override
        public CompletionStage<Void> admitted() {
            return admission;
        }

        @Override
        public <U> CompletionStage<U> thenApply(Function<? super T, ? extends U> fn) {
            return reply.thenApply(fn);
        }

        @Override
        public <U> CompletionStage<U> thenApplyAsync(Function<? super T, ? extends U> fn) {
            return reply.thenApplyAsync(fn);
        }

        @Override
        public <U> CompletionStage<U> thenApplyAsync(
                Function<? super T, ? extends U> fn, Executor executor) {
            return reply.thenApplyAsync(fn, executor);
        }

        @Override
        public CompletionStage<Void> thenAccept(Consumer<? super T> action) {
            return reply.thenAccept(action);
        }

        @Override
        public CompletionStage<Void> thenAcceptAsync(Consumer<? super T> action) {
            return reply.thenAcceptAsync(action);
        }

        @Override
        public CompletionStage<Void> thenAcceptAsync(
                Consumer<? super T> action, Executor executor) {
            return reply.thenAcceptAsync(action, executor);
        }

        @Override
        public CompletionStage<Void> thenRun(Runnable action) {
            return reply.thenRun(action);
        }

        @Override
        public CompletionStage<Void> thenRunAsync(Runnable action) {
            return reply.thenRunAsync(action);
        }

        @Override
        public CompletionStage<Void> thenRunAsync(Runnable action, Executor executor) {
            return reply.thenRunAsync(action, executor);
        }

        @Override
        public <U, V> CompletionStage<V> thenCombine(
                CompletionStage<? extends U> other,
                BiFunction<? super T, ? super U, ? extends V> fn) {
            return reply.thenCombine(other, fn);
        }

        @Override
        public <U, V> CompletionStage<V> thenCombineAsync(
                CompletionStage<? extends U> other,
                BiFunction<? super T, ? super U, ? extends V> fn) {
            return reply.thenCombineAsync(other, fn);
        }

        @Override
        public <U, V> CompletionStage<V> thenCombineAsync(
                CompletionStage<? extends U> other,
                BiFunction<? super T, ? super U, ? extends V> fn,
                Executor executor) {
            return reply.thenCombineAsync(other, fn, executor);
        }

        @Override
        public <U> CompletionStage<Void> thenAcceptBoth(
                CompletionStage<? extends U> other, BiConsumer<? super T, ? super U> action) {
            return reply.thenAcceptBoth(other, action);
        }

        @Override
        public <U> CompletionStage<Void> thenAcceptBothAsync(
                CompletionStage<? extends U> other, BiConsumer<? super T, ? super U> action) {
            return reply.thenAcceptBothAsync(other, action);
        }

        @Override
        public <U> CompletionStage<Void> thenAcceptBothAsync(
                CompletionStage<? extends U> other,
                BiConsumer<? super T, ? super U> action,
                Executor executor) {
            return reply.thenAcceptBothAsync(other, action, executor);
        }

        @Override
        public CompletionStage<Void> runAfterBoth(CompletionStage<?> other, Runnable action) {
            return reply.runAfterBoth(other, action);
        }

        @Override
        public CompletionStage<Void> runAfterBothAsync(CompletionStage<?> other, Runnable action) {
            return reply.runAfterBothAsync(other, action);
        }

        @Override
        public CompletionStage<Void> runAfterBothAsync(
                CompletionStage<?> other, Runnable action, Executor executor) {
            return reply.runAfterBothAsync(other, action, executor);
        }

        @Override
        public <U> CompletionStage<U> applyToEither(
                CompletionStage<? extends T> other, Function<? super T, U> fn) {
            return reply.applyToEither(other, fn);
        }

        @Override
        public <U> CompletionStage<U> applyToEitherAsync(
                CompletionStage<? extends T> other, Function<? super T, U> fn) {
            return reply.applyToEitherAsync(other, fn);
        }

        @Override
        public <U> CompletionStage<U> applyToEitherAsync(
                CompletionStage<? extends T> other, Function<? super T, U> fn, Executor executor) {
            return reply.applyToEitherAsync(other, fn, executor);
        }

        @Override
        public CompletionStage<Void> acceptEither(
                CompletionStage<? extends T> other, Consumer<? super T> action) {
            return reply.acceptEither(other, action);
        }

        @Override
        public CompletionStage<Void> acceptEitherAsync(
                CompletionStage<? extends T> other, Consumer<? super T> action) {
            return reply.acceptEitherAsync(other, action);
        }

        @Override
        public CompletionStage<Void> acceptEitherAsync(
                CompletionStage<? extends T> other, Consumer<? super T> action, Executor executor) {
            return reply.acceptEitherAsync(other, action, executor);
        }

        @Override
        public CompletionStage<Void> runAfterEither(CompletionStage<?> other, Runnable action) {
            return reply.runAfterEither(other, action);
        }

        @Override
        public CompletionStage<Void> runAfterEitherAsync(
                CompletionStage<?> other, Runnable action) {
            return reply.runAfterEitherAsync(other, action);
        }

        @Override
        public CompletionStage<Void> runAfterEitherAsync(
                CompletionStage<?> other, Runnable action, Executor executor) {
            return reply.runAfterEitherAsync(other, action, executor);
        }

        @Override
        public <U> CompletionStage<U> thenCompose(
                Function<? super T, ? extends CompletionStage<U>> other) {
            return reply.thenCompose(other);
        }

        @Override
        public <U> CompletionStage<U> thenComposeAsync(
                Function<? super T, ? extends CompletionStage<U>> other) {
            return reply.thenComposeAsync(other);
        }

        @Override
        public <U> CompletionStage<U> thenComposeAsync(
                Function<? super T, ? extends CompletionStage<U>> other, Executor executor) {
            return reply.thenComposeAsync(other, executor);
        }

        @Override
        public <U> CompletionStage<U> handle(BiFunction<? super T, Throwable, ? extends U> fn) {
            return reply.handle(fn);
        }

        @Override
        public <U> CompletionStage<U> handleAsync(
                BiFunction<? super T, Throwable, ? extends U> fn) {
            return reply.handleAsync(fn);
        }

        @Override
        public <U> CompletionStage<U> handleAsync(
                BiFunction<? super T, Throwable, ? extends U> fn, Executor executor) {
            return reply.handleAsync(fn, executor);
        }

        @Override
        public CompletionStage<T> whenComplete(BiConsumer<? super T, ? super Throwable> action) {
            return reply.whenComplete(action);
        }

        @Override
        public CompletionStage<T> whenCompleteAsync(
                BiConsumer<? super T, ? super Throwable> action) {
            return reply.whenCompleteAsync(action);
        }

        @Override
        public CompletionStage<T> whenCompleteAsync(
                BiConsumer<? super T, ? super Throwable> action, Executor executor) {
            return reply.whenCompleteAsync(action, executor);
        }

        @Override
        public CompletionStage<T> exceptionally(Function<Throwable, ? extends T> fn) {
            return reply.exceptionally(fn);
        }

        @Override
        public CompletableFuture<T> toCompletableFuture() {
            return reply.toCompletableFuture();
        }
    }
}
