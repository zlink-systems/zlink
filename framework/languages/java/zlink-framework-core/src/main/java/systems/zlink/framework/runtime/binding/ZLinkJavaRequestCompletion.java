package systems.zlink.framework.runtime.binding;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.Consumer;

/** Connects a binding request waiter to the caller's cancellation and reply ownership. */
final class ZLinkJavaRequestCompletion {
    private ZLinkJavaRequestCompletion() {}

    static void forwardCancellation(CompletableFuture<?> caller, CompletionStage<?> waiter) {
        forwardCancellation(caller, waiter, null);
    }

    static void forwardCancellation(
            CompletableFuture<?> caller, CompletionStage<?> first, CompletionStage<?> second) {
        caller.whenComplete(
                (ignored, failure) -> {
                    if (caller.isCancelled()) {
                        first.toCompletableFuture().cancel(false);
                        if (second != null) second.toCompletableFuture().cancel(false);
                    }
                });
    }

    static <T> void completeOrDiscard(
            CompletableFuture<T> caller, T value, Consumer<? super T> discard) {
        if (!caller.complete(value)) discard.accept(value);
    }
}
