package systems.zlink.framework.runtime.internal;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.function.BiConsumer;
import java.util.function.Consumer;

/** Bridges caller cancellation to waiter stages and disposes results the caller cannot accept. */
public final class ZLinkCompletionBridge {
    private ZLinkCompletionBridge() {}

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
        forwardCancellation(caller, waiter, null);
    }

    public static void forwardCancellation(
            CompletableFuture<?> caller, CompletionStage<?> first, CompletionStage<?> second) {
        forwardCancellation(
                caller,
                first,
                second,
                (firstWaiter, secondWaiter) -> {
                    firstWaiter.toCompletableFuture().cancel(false);
                    if (secondWaiter != null) secondWaiter.toCompletableFuture().cancel(false);
                });
    }

    /** Runs the forwarding action when cancellation must be scheduled on the waiter's owner. */
    public static void forwardCancellation(CompletableFuture<?> caller, Runnable forward) {
        forwardCancellation(caller, forward, null, (action, ignored) -> action.run());
    }

    private static <T, U> void forwardCancellation(
            CompletableFuture<?> caller, T first, U second, BiConsumer<T, U> action) {
        caller.whenComplete(
                (ignored, failure) -> {
                    if (caller.isCancelled()) action.accept(first, second);
                });
    }

    public static <T> void completeOrDiscard(
            CompletableFuture<T> caller, T value, Consumer<? super T> discard) {
        if (!caller.complete(value)) discard.accept(value);
    }
}
