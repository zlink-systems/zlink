package systems.zlink.framework.runtime.host;

final class ZLinkCloseGate {
    private final java.util.concurrent.atomic.AtomicBoolean started =
        new java.util.concurrent.atomic.AtomicBoolean();
    private final java.util.concurrent.CompletableFuture<Void> ownershipCleanup =
        new java.util.concurrent.CompletableFuture<>();

    java.util.concurrent.CompletionStage<Void> close(
        java.util.function.Supplier<java.util.concurrent.CompletionStage<Void>> cleanup) {
        if (!started.compareAndSet(false, true)) {
            return ownershipCleanup;
        }
        try {
            cleanup.get().whenComplete((ignored, failure) -> {
                if (failure == null) {
                    ownershipCleanup.complete(null);
                } else {
                    ownershipCleanup.completeExceptionally(failure);
                }
            });
        } catch (RuntimeException failure) {
            ownershipCleanup.completeExceptionally(failure);
        }
        return ownershipCleanup;
    }
}
