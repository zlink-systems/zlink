package systems.zlink.framework.runtime.internal.service;

import java.time.Duration;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.RejectedExecutionException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

/** Owns request correlation, deadline, and terminal-once completion. */
public final class ZLinkServiceOperationRegistry implements AutoCloseable {
    public static final int DEFAULT_MAX_PENDING_OPERATIONS = 4_096;

    private final ScheduledExecutorService scheduler;
    private final int maxPendingOperations;
    private final Object gate = new Object();
    private final Map<UUID, Entry<?>> entries = new HashMap<>();
    private boolean closed;

    public ZLinkServiceOperationRegistry(ScheduledExecutorService scheduler) {
        this(scheduler, DEFAULT_MAX_PENDING_OPERATIONS);
    }

    public ZLinkServiceOperationRegistry(
        ScheduledExecutorService scheduler,
        int maxPendingOperations) {
        this.scheduler = Objects.requireNonNull(scheduler, "scheduler");
        if (maxPendingOperations <= 0) {
            throw new IllegalArgumentException(
                "maxPendingOperations must be positive");
        }
        this.maxPendingOperations = maxPendingOperations;
    }

    public <T> Operation<T> register(Duration timeout) {
        Objects.requireNonNull(timeout, "timeout");
        if (timeout.isNegative() || timeout.isZero()) {
            throw new IllegalArgumentException("timeout must be positive");
        }
        long timeoutNanos;
        try {
            timeoutNanos = timeout.toNanos();
        } catch (ArithmeticException overflow) {
            timeoutNanos = Long.MAX_VALUE;
        }
        UUID id;
        Entry<T> entry = new Entry<>();
        synchronized (gate) {
            if (closed) {
                throw new IllegalStateException("operation registry is closed");
            }
            if (entries.size() >= maxPendingOperations) {
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.CAPACITY_EXCEEDED,
                    "service operation capacity is exhausted");
            }
            do {
                id = UUID.randomUUID();
            } while (id.getMostSignificantBits() == 0
                && id.getLeastSignificantBits() == 0
                || entries.containsKey(id));
            entries.put(id, entry);
            try {
                UUID operationId = id;
                entry.deadline = scheduler.schedule(
                    () -> completeExceptionally(
                        operationId,
                        new TimeoutException("service operation timed out")),
                    timeoutNanos,
                    TimeUnit.NANOSECONDS);
            } catch (RuntimeException schedulingFailure) {
                entries.remove(id, entry);
                throw schedulingFailure;
            }
        }
        UUID operationId = id;
        entry.completion.cancellation(() -> cancel(operationId, entry));
        return new Operation<>(id, entry.completion);
    }

    public <T> boolean complete(UUID id, T value) {
        @SuppressWarnings("unchecked")
        Entry<T> entry = (Entry<T>) take(Objects.requireNonNull(id, "id"));
        if (entry == null) {
            return false;
        }
        cancelDeadline(entry);
        dispatchCompletion(() -> entry.completion.complete(value));
        return true;
    }

    public boolean completeExceptionally(UUID id, Throwable failure) {
        Entry<?> entry = take(Objects.requireNonNull(id, "id"));
        if (entry == null) {
            return false;
        }
        cancelDeadline(entry);
        Throwable terminal = Objects.requireNonNull(failure, "failure");
        dispatchCompletion(() -> entry.completion.completeExceptionally(terminal));
        return true;
    }

    /**
     * Removes an operation whose transport submission was rejected before a
     * request existed. No callback is delivered for such an operation.
     */
    public boolean discard(UUID id) {
        Entry<?> entry = take(Objects.requireNonNull(id, "id"));
        if (entry == null) {
            return false;
        }
        cancelDeadline(entry);
        return true;
    }

    public int pendingCount() {
        synchronized (gate) {
            return entries.size();
        }
    }

    @Override
    public void close() {
        List<Entry<?>> detached;
        synchronized (gate) {
            if (closed) {
                return;
            }
            closed = true;
            detached = new ArrayList<>(entries.values());
            entries.clear();
        }
        IllegalStateException failure = new IllegalStateException("service runtime is closed");
        detached.forEach(entry -> {
            cancelDeadline(entry);
            dispatchCompletion(() -> entry.completion.completeExceptionally(failure));
        });
    }

    private Entry<?> take(UUID id) {
        synchronized (gate) {
            return entries.remove(id);
        }
    }

    private boolean cancel(UUID id, Entry<?> expected) {
        synchronized (gate) {
            if (entries.get(id) != expected || expected.completion.isDone()) {
                return false;
            }
            entries.remove(id);
        }
        cancelDeadline(expected);
        dispatchCompletion(expected.completion::completeCancellation);
        return true;
    }

    private void dispatchCompletion(Runnable completion) {
        try {
            scheduler.execute(completion);
        } catch (RejectedExecutionException schedulerClosed) {
            CompletableFuture.runAsync(completion);
        }
    }

    private static void cancelDeadline(Entry<?> entry) {
        ScheduledFuture<?> deadline = entry.deadline;
        if (deadline != null) {
            deadline.cancel(false);
        }
    }

    public record Operation<T>(UUID id, CompletableFuture<T> completion) {
        public Operation {
            Objects.requireNonNull(id, "id");
            Objects.requireNonNull(completion, "completion");
        }
    }

    private static final class Entry<T> {
        private final OperationFuture<T> completion = new OperationFuture<>();
        private volatile ScheduledFuture<?> deadline;
    }

    private static final class OperationFuture<T> extends CompletableFuture<T> {
        private volatile Supplier cancellation = () -> false;

        void cancellation(Supplier cancellation) {
            this.cancellation = Objects.requireNonNull(cancellation, "cancellation");
        }

        @Override
        public boolean cancel(boolean mayInterruptIfRunning) {
            return cancellation.cancel();
        }

        void completeCancellation() {
            super.cancel(false);
        }

        @FunctionalInterface
        private interface Supplier {
            boolean cancel();
        }
    }
}
