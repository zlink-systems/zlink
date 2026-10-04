package systems.zlink.framework.runtime.internal;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.Consumer;

/** Bridges caller cancellation to waiter stages and disposes results the caller cannot accept. */
public final class ZLinkCompletionBridge {
    private ZLinkCompletionBridge() {}

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
}
