package systems.zlink.framework.runtime.internal.calls;

import systems.zlink.framework.channels.ZLinkRequestStage;
import systems.zlink.framework.runtime.internal.ZLinkCompletionBridge;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;

/** Connects observations to the existing request and admission owners. */
public final class ZLinkRequestStages {
    private ZLinkRequestStages() {}

    public static void forwardAdmission(
            CompletableFuture<Void> caller, CompletionStage<Void> owner) {
        if (caller == null) return;
        owner.whenComplete(
                (ignored, failure) -> {
                    if (failure == null) caller.complete(null);
                });
    }

    public static <T> ZLinkRequestStage<T> observe(
            CompletionStage<T> reply, CompletableFuture<Void> admission) {
        reply.whenComplete(
                (ignored, failure) -> {
                    if (failure != null) admission.completeExceptionally(unwrap(failure));
                });
        ZLinkCompletionBridge.forwardCancellation(admission, reply);
        return new RequestStage<>(
                reply,
                systems.zlink.framework.execution.ZLinkSerialExecutionQueue.manageCurrent(
                        admission));
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
        public <U> java.util.concurrent.CompletionStage<U> thenApply(
                java.util.function.Function<? super T, ? extends U> arg0) {
            return reply.thenApply(arg0);
        }

        @Override
        public <U> java.util.concurrent.CompletionStage<U> thenApplyAsync(
                java.util.function.Function<? super T, ? extends U> arg0) {
            return reply.thenApplyAsync(arg0);
        }

        @Override
        public <U> java.util.concurrent.CompletionStage<U> thenApplyAsync(
                java.util.function.Function<? super T, ? extends U> arg0,
                java.util.concurrent.Executor arg1) {
            return reply.thenApplyAsync(arg0, arg1);
        }

        @Override
        public java.util.concurrent.CompletionStage<java.lang.Void> thenAccept(
                java.util.function.Consumer<? super T> arg0) {
            return reply.thenAccept(arg0);
        }

        @Override
        public java.util.concurrent.CompletionStage<java.lang.Void> thenAcceptAsync(
                java.util.function.Consumer<? super T> arg0) {
            return reply.thenAcceptAsync(arg0);
        }

        @Override
        public java.util.concurrent.CompletionStage<java.lang.Void> thenAcceptAsync(
                java.util.function.Consumer<? super T> arg0, java.util.concurrent.Executor arg1) {
            return reply.thenAcceptAsync(arg0, arg1);
        }

        @Override
        public java.util.concurrent.CompletionStage<java.lang.Void> thenRun(
                java.lang.Runnable arg0) {
            return reply.thenRun(arg0);
        }

        @Override
        public java.util.concurrent.CompletionStage<java.lang.Void> thenRunAsync(
                java.lang.Runnable arg0) {
            return reply.thenRunAsync(arg0);
        }

        @Override
        public java.util.concurrent.CompletionStage<java.lang.Void> thenRunAsync(
                java.lang.Runnable arg0, java.util.concurrent.Executor arg1) {
            return reply.thenRunAsync(arg0, arg1);
        }

        @Override
        public <U, V> java.util.concurrent.CompletionStage<V> thenCombine(
                java.util.concurrent.CompletionStage<? extends U> arg0,
                java.util.function.BiFunction<? super T, ? super U, ? extends V> arg1) {
            return reply.thenCombine(arg0, arg1);
        }

        @Override
        public <U, V> java.util.concurrent.CompletionStage<V> thenCombineAsync(
                java.util.concurrent.CompletionStage<? extends U> arg0,
                java.util.function.BiFunction<? super T, ? super U, ? extends V> arg1) {
            return reply.thenCombineAsync(arg0, arg1);
        }

        @Override
        public <U, V> java.util.concurrent.CompletionStage<V> thenCombineAsync(
                java.util.concurrent.CompletionStage<? extends U> arg0,
                java.util.function.BiFunction<? super T, ? super U, ? extends V> arg1,
                java.util.concurrent.Executor arg2) {
            return reply.thenCombineAsync(arg0, arg1, arg2);
        }

        @Override
        public <U> java.util.concurrent.CompletionStage<java.lang.Void> thenAcceptBoth(
                java.util.concurrent.CompletionStage<? extends U> arg0,
                java.util.function.BiConsumer<? super T, ? super U> arg1) {
            return reply.thenAcceptBoth(arg0, arg1);
        }

        @Override
        public <U> java.util.concurrent.CompletionStage<java.lang.Void> thenAcceptBothAsync(
                java.util.concurrent.CompletionStage<? extends U> arg0,
                java.util.function.BiConsumer<? super T, ? super U> arg1) {
            return reply.thenAcceptBothAsync(arg0, arg1);
        }

        @Override
        public <U> java.util.concurrent.CompletionStage<java.lang.Void> thenAcceptBothAsync(
                java.util.concurrent.CompletionStage<? extends U> arg0,
                java.util.function.BiConsumer<? super T, ? super U> arg1,
                java.util.concurrent.Executor arg2) {
            return reply.thenAcceptBothAsync(arg0, arg1, arg2);
        }

        @Override
        public java.util.concurrent.CompletionStage<java.lang.Void> runAfterBoth(
                java.util.concurrent.CompletionStage<?> arg0, java.lang.Runnable arg1) {
            return reply.runAfterBoth(arg0, arg1);
        }

        @Override
        public java.util.concurrent.CompletionStage<java.lang.Void> runAfterBothAsync(
                java.util.concurrent.CompletionStage<?> arg0, java.lang.Runnable arg1) {
            return reply.runAfterBothAsync(arg0, arg1);
        }

        @Override
        public java.util.concurrent.CompletionStage<java.lang.Void> runAfterBothAsync(
                java.util.concurrent.CompletionStage<?> arg0,
                java.lang.Runnable arg1,
                java.util.concurrent.Executor arg2) {
            return reply.runAfterBothAsync(arg0, arg1, arg2);
        }

        @Override
        public <U> java.util.concurrent.CompletionStage<U> applyToEither(
                java.util.concurrent.CompletionStage<? extends T> arg0,
                java.util.function.Function<? super T, U> arg1) {
            return reply.applyToEither(arg0, arg1);
        }

        @Override
        public <U> java.util.concurrent.CompletionStage<U> applyToEitherAsync(
                java.util.concurrent.CompletionStage<? extends T> arg0,
                java.util.function.Function<? super T, U> arg1) {
            return reply.applyToEitherAsync(arg0, arg1);
        }

        @Override
        public <U> java.util.concurrent.CompletionStage<U> applyToEitherAsync(
                java.util.concurrent.CompletionStage<? extends T> arg0,
                java.util.function.Function<? super T, U> arg1,
                java.util.concurrent.Executor arg2) {
            return reply.applyToEitherAsync(arg0, arg1, arg2);
        }

        @Override
        public java.util.concurrent.CompletionStage<java.lang.Void> acceptEither(
                java.util.concurrent.CompletionStage<? extends T> arg0,
                java.util.function.Consumer<? super T> arg1) {
            return reply.acceptEither(arg0, arg1);
        }

        @Override
        public java.util.concurrent.CompletionStage<java.lang.Void> acceptEitherAsync(
                java.util.concurrent.CompletionStage<? extends T> arg0,
                java.util.function.Consumer<? super T> arg1) {
            return reply.acceptEitherAsync(arg0, arg1);
        }

        @Override
        public java.util.concurrent.CompletionStage<java.lang.Void> acceptEitherAsync(
                java.util.concurrent.CompletionStage<? extends T> arg0,
                java.util.function.Consumer<? super T> arg1,
                java.util.concurrent.Executor arg2) {
            return reply.acceptEitherAsync(arg0, arg1, arg2);
        }

        @Override
        public java.util.concurrent.CompletionStage<java.lang.Void> runAfterEither(
                java.util.concurrent.CompletionStage<?> arg0, java.lang.Runnable arg1) {
            return reply.runAfterEither(arg0, arg1);
        }

        @Override
        public java.util.concurrent.CompletionStage<java.lang.Void> runAfterEitherAsync(
                java.util.concurrent.CompletionStage<?> arg0, java.lang.Runnable arg1) {
            return reply.runAfterEitherAsync(arg0, arg1);
        }

        @Override
        public java.util.concurrent.CompletionStage<java.lang.Void> runAfterEitherAsync(
                java.util.concurrent.CompletionStage<?> arg0,
                java.lang.Runnable arg1,
                java.util.concurrent.Executor arg2) {
            return reply.runAfterEitherAsync(arg0, arg1, arg2);
        }

        @Override
        public <U> java.util.concurrent.CompletionStage<U> thenCompose(
                java.util.function.Function<
                                ? super T, ? extends java.util.concurrent.CompletionStage<U>>
                        arg0) {
            return reply.thenCompose(arg0);
        }

        @Override
        public <U> java.util.concurrent.CompletionStage<U> thenComposeAsync(
                java.util.function.Function<
                                ? super T, ? extends java.util.concurrent.CompletionStage<U>>
                        arg0) {
            return reply.thenComposeAsync(arg0);
        }

        @Override
        public <U> java.util.concurrent.CompletionStage<U> thenComposeAsync(
                java.util.function.Function<
                                ? super T, ? extends java.util.concurrent.CompletionStage<U>>
                        arg0,
                java.util.concurrent.Executor arg1) {
            return reply.thenComposeAsync(arg0, arg1);
        }

        @Override
        public <U> java.util.concurrent.CompletionStage<U> handle(
                java.util.function.BiFunction<? super T, java.lang.Throwable, ? extends U> arg0) {
            return reply.handle(arg0);
        }

        @Override
        public <U> java.util.concurrent.CompletionStage<U> handleAsync(
                java.util.function.BiFunction<? super T, java.lang.Throwable, ? extends U> arg0) {
            return reply.handleAsync(arg0);
        }

        @Override
        public <U> java.util.concurrent.CompletionStage<U> handleAsync(
                java.util.function.BiFunction<? super T, java.lang.Throwable, ? extends U> arg0,
                java.util.concurrent.Executor arg1) {
            return reply.handleAsync(arg0, arg1);
        }

        @Override
        public java.util.concurrent.CompletionStage<T> whenComplete(
                java.util.function.BiConsumer<? super T, ? super java.lang.Throwable> arg0) {
            return reply.whenComplete(arg0);
        }

        @Override
        public java.util.concurrent.CompletionStage<T> whenCompleteAsync(
                java.util.function.BiConsumer<? super T, ? super java.lang.Throwable> arg0) {
            return reply.whenCompleteAsync(arg0);
        }

        @Override
        public java.util.concurrent.CompletionStage<T> whenCompleteAsync(
                java.util.function.BiConsumer<? super T, ? super java.lang.Throwable> arg0,
                java.util.concurrent.Executor arg1) {
            return reply.whenCompleteAsync(arg0, arg1);
        }

        @Override
        public java.util.concurrent.CompletionStage<T> exceptionally(
                java.util.function.Function<java.lang.Throwable, ? extends T> arg0) {
            return reply.exceptionally(arg0);
        }

        @Override
        public CompletableFuture<T> toCompletableFuture() {
            return reply.toCompletableFuture();
        }
    }
}
