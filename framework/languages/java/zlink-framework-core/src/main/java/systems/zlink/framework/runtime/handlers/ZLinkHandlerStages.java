package systems.zlink.framework.runtime.handlers;

import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.function.Supplier;

public final class ZLinkHandlerStages {
    private ZLinkHandlerStages() {}

    public static CompletionStage<Void> fromRunnable(Runnable operation) {
        try {
            operation.run();
            return CompletableFuture.completedFuture(null);
        } catch (RuntimeException ex) {
            return CompletableFuture.failedFuture(ex);
        }
    }

    public static <T> CompletionStage<T> fromSupplier(Supplier<T> operation) {
        try {
            return CompletableFuture.completedFuture(operation.get());
        } catch (RuntimeException ex) {
            return CompletableFuture.failedFuture(ex);
        }
    }

    public static <T> CompletionStage<T> fromStageSupplier(
            Supplier<? extends CompletionStage<T>> operation) {
        try {
            CompletionStage<T> stage = operation.get();
            if (stage == null) {
                return CompletableFuture.failedFuture(
                        new NullPointerException("handler returned a null completion stage"));
            }
            return stage;
        } catch (RuntimeException ex) {
            return CompletableFuture.failedFuture(ex);
        }
    }

    /** Completes every operation in order and propagates their failures as one terminal. */
    public static CompletionStage<Void> completeAll(
            List<? extends Supplier<? extends CompletionStage<Void>>> operations) {
        CompletionStage<Throwable> failures = CompletableFuture.completedFuture(null);
        for (Supplier<? extends CompletionStage<Void>> operation : operations) {
            failures =
                    failures.thenCompose(
                            first ->
                                    fromStageSupplier(operation)
                                            .handle(
                                                    (ignored, failure) -> {
                                                        if (failure == null) return first;
                                                        Throwable cause = failure;
                                                        while (cause instanceof CompletionException
                                                                && cause.getCause() != null) {
                                                            cause = cause.getCause();
                                                        }
                                                        if (first == null) return cause;
                                                        if (first != cause)
                                                            first.addSuppressed(cause);
                                                        return first;
                                                    }));
        }
        return failures.thenCompose(
                failure ->
                        failure == null
                                ? CompletableFuture.completedFuture(null)
                                : CompletableFuture.failedFuture(failure));
    }
}
