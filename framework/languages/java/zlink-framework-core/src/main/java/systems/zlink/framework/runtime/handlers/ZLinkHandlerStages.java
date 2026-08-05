package systems.zlink.framework.runtime.handlers;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.Supplier;

public final class ZLinkHandlerStages {
    private ZLinkHandlerStages() {
    }

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
}
