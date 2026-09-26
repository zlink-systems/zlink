package systems.zlink.framework.runtime.mesh;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locations.ZLinkActivationConcurrency;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.Supplier;

/**
 * The activation admission record of one MeshNode (MeshNode §5.1 "Pending activation").
 *
 * <p>Actor creation, User Spot creation, Instance Spot cold activation and a relocation target's
 * Restore each hold one admission from the moment this MeshNode receives the operation until the
 * operation reaches Ready or its target commit, or ends by rejection, failure or cleanup. Entry
 * Spots and Actor Join never take one. The limit is enforced here, and runtime monitoring reads the
 * current value and the headroom from this record only.
 */
public final class ZLinkActivationAdmission {
    private final int limit;
    private final AtomicInteger active = new AtomicInteger();

    public ZLinkActivationAdmission(int limit) {
        if (limit <= 0) {
            throw new IllegalArgumentException("activation concurrency limit must be positive");
        }
        this.limit = limit;
    }

    /**
     * Takes one admission. This is the only place that decides what a full record answers: the
     * operation fails with {@code UNAVAILABLE}. The spec does not define that answer yet
     * (MeshNode §5.1); this follows the current .NET behavior until it does.
     */
    public Permit acquire(String operation) {
        while (true) {
            int current = active.get();
            if (current >= limit) {
                throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.UNAVAILABLE,
                        "Activation concurrency limit was reached for " + operation);
            }
            if (active.compareAndSet(current, current + 1)) {
                return new Permit();
            }
        }
    }

    /** Holds one admission while {@code work} runs and returns it when that stage completes. */
    public <T> CompletionStage<T> admit(
            String operation, Supplier<? extends CompletionStage<T>> work) {
        Permit permit;
        try {
            permit = acquire(operation);
        } catch (ZLinkFrameworkException full) {
            return CompletableFuture.failedFuture(full);
        }
        CompletionStage<T> stage;
        try {
            stage = work.get();
        } catch (RuntimeException failure) {
            permit.close();
            return CompletableFuture.failedFuture(failure);
        }
        return stage.whenComplete((ignored, failure) -> permit.close());
    }

    /** The current value and the limit, as runtime monitoring reports them. */
    public ZLinkActivationConcurrency snapshot() {
        return new ZLinkActivationConcurrency(active.get(), limit);
    }

    /** One held admission. Closing it returns the admission exactly once. */
    public final class Permit implements AutoCloseable {
        private final AtomicBoolean held = new AtomicBoolean(true);

        private Permit() {}

        @Override
        public void close() {
            if (held.compareAndSet(true, false)) {
                active.decrementAndGet();
            }
        }
    }
}
