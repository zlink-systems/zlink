package systems.zlink.framework.runtime.host;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.Supplier;

final class ZLinkCloseGate {
    private final AtomicBoolean started =
        new AtomicBoolean();
    private final CompletableFuture<Void> ownershipCleanup =
        new CompletableFuture<>();

    CompletionStage<Void> close(
        Supplier<CompletionStage<Void>> cleanup) {
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
