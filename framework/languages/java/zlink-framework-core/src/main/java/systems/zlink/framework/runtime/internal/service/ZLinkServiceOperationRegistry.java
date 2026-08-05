package systems.zlink.framework.runtime.internal.service;

import java.time.Duration;
import java.util.Map;
import java.util.Objects;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.atomic.AtomicBoolean;

/** Owns request correlation, deadline, and terminal-once completion. */
public final class ZLinkServiceOperationRegistry implements AutoCloseable {
    public static final int DEFAULT_MAX_PENDING_OPERATIONS = 65_536;

    private final ScheduledExecutorService scheduler;
    private final int maxPendingOperations;
    private final Map<UUID, Entry<?>> entries = new ConcurrentHashMap<>();
    private final AtomicBoolean closed = new AtomicBoolean();

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

    public synchronized <T> Operation<T> register(Duration timeout) {
        Objects.requireNonNull(timeout, "timeout");
        if (timeout.isNegative() || timeout.isZero()) {
            throw new IllegalArgumentException("timeout must be positive");
        }
        if (closed.get()) {
            throw new IllegalStateException("operation registry is closed");
        }
        if (entries.size() >= maxPendingOperations) {
            throw new RejectedExecutionException(
                "service operation capacity is exhausted");
        }

        UUID id = UUID.randomUUID();
        Entry<T> entry = new Entry<>();
        entries.put(id, entry);
        long timeoutNanos;
        try {
            timeoutNanos = timeout.toNanos();
        } catch (ArithmeticException overflow) {
            timeoutNanos = Long.MAX_VALUE;
        }
        entry.deadline = scheduler.schedule(
            () -> completeExceptionally(id, new TimeoutException("service operation timed out")),
            timeoutNanos,
            TimeUnit.NANOSECONDS);
        return new Operation<>(id, entry.completion);
    }

    public <T> boolean complete(UUID id, T value) {
        @SuppressWarnings("unchecked")
        Entry<T> entry = (Entry<T>) entries.remove(Objects.requireNonNull(id, "id"));
        if (entry == null) {
            return false;
        }
        cancelDeadline(entry);
        return entry.completion.complete(value);
    }

    public boolean completeExceptionally(UUID id, Throwable failure) {
        Entry<?> entry = entries.remove(Objects.requireNonNull(id, "id"));
        if (entry == null) {
            return false;
        }
        cancelDeadline(entry);
        return entry.completion.completeExceptionally(
            Objects.requireNonNull(failure, "failure"));
    }

    /**
     * Removes an operation whose transport submission was rejected before a
     * request existed. No callback is delivered for such an operation.
     */
    public boolean discard(UUID id) {
        Entry<?> entry = entries.remove(Objects.requireNonNull(id, "id"));
        if (entry == null) {
            return false;
        }
        cancelDeadline(entry);
        return true;
    }

    public int pendingCount() {
        return entries.size();
    }

    @Override
    public synchronized void close() {
        if (!closed.compareAndSet(false, true)) {
            return;
        }
        IllegalStateException failure = new IllegalStateException("service runtime is closed");
        entries.keySet().forEach(id -> completeExceptionally(id, failure));
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
        private final CompletableFuture<T> completion = new CompletableFuture<>();
        private volatile ScheduledFuture<?> deadline;
    }
}
