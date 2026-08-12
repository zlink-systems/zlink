package systems.zlink.framework.runtime.internal.service;

import java.time.Duration;
import java.util.HashMap;
import java.util.Map;
import java.util.Objects;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

/** Owns request correlation, deadline, and terminal-once completion. */
public final class ZLinkServiceOperationRegistry implements AutoCloseable {
    public static final int DEFAULT_MAX_PENDING_OPERATIONS = 4_096;

    private final ScheduledExecutorService scheduler;
    private final int maxPendingOperations;
    private final ZLinkServiceCompletionDispatcher completions =
        ZLinkServiceCompletionDispatcher.INSTANCE;
    private final Object gate = new Object();
    private final Map<UUID, Entry<?>> entries = new HashMap<>();
    private final IllegalStateException closeFailure =
        new IllegalStateException("service runtime is closed");
    private Entry<?> activeHead;
    private Entry<?> activeTail;
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
        Entry<T> entry;
        synchronized (gate) {
            if (closed) {
                throw new IllegalStateException("operation registry is closed");
            }
            if (entries.size() >= maxPendingOperations) {
                throw capacityExceeded();
            }
            entry = new Entry<>();
            if (!completions.tryReserve(entry)) {
                throw capacityExceeded();
            }
            do {
                id = UUID.randomUUID();
            } while (id.getMostSignificantBits() == 0
                && id.getLeastSignificantBits() == 0
                || entries.containsKey(id));
            entry.id = id;
            entry.completion.cancellation(() -> cancel(entry.id, entry));
            entries.put(id, entry);
            linkActive(entry);
            try {
                UUID operationId = id;
                entry.deadline = scheduler.schedule(
                    () -> completeExceptionally(
                        operationId,
                        entry.timeoutFailure),
                    timeoutNanos,
                    TimeUnit.NANOSECONDS);
            } catch (RuntimeException schedulingFailure) {
                entries.remove(id, entry);
                unlinkActive(entry);
                entry.completion.clearCancellation();
                completions.releaseWithoutDispatch(entry);
                throw schedulingFailure;
            }
        }
        return new Operation<>(id, entry.completion);
    }

    public <T> boolean complete(UUID id, T value) {
        @SuppressWarnings("unchecked")
        Entry<T> entry = (Entry<T>) take(Objects.requireNonNull(id, "id"));
        if (entry == null) {
            return false;
        }
        cancelDeadline(entry);
        entry.completeWithValue(value);
        completions.post(entry);
        return true;
    }

    public boolean completeExceptionally(UUID id, Throwable failure) {
        Throwable terminal = Objects.requireNonNull(failure, "failure");
        Entry<?> entry = take(Objects.requireNonNull(id, "id"));
        if (entry == null) {
            return false;
        }
        cancelDeadline(entry);
        entry.completeWithFailure(terminal);
        completions.post(entry);
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
        entry.completion.clearCancellation();
        completions.releaseWithoutDispatch(entry);
        return true;
    }

    public int pendingCount() {
        synchronized (gate) {
            return entries.size();
        }
    }

    @Override
    public void close() {
        Entry<?> detachedHead;
        Entry<?> detachedTail;
        synchronized (gate) {
            if (closed) {
                return;
            }
            closed = true;
            detachedHead = activeHead;
            detachedTail = activeTail;
            Entry<?> current = detachedHead;
            while (current != null) {
                Entry<?> next = current.activeNext;
                current.activePrevious = null;
                current.activeNext = null;
                current.dispatchNext(next);
                current.completeWithFailure(closeFailure);
                current = next;
            }
            entries.clear();
            activeHead = null;
            activeTail = null;
        }
        Entry<?> current = detachedHead;
        while (current != null) {
            cancelDeadline(current);
            current = current.dispatchNext();
        }
        completions.postChain(detachedHead, detachedTail);
    }

    private Entry<?> take(UUID id) {
        synchronized (gate) {
            Entry<?> entry = entries.remove(id);
            if (entry != null) {
                unlinkActive(entry);
            }
            return entry;
        }
    }

    private boolean cancel(UUID id, Entry<?> expected) {
        synchronized (gate) {
            if (entries.get(id) != expected || expected.completion.isDone()) {
                return false;
            }
            entries.remove(id);
            unlinkActive(expected);
        }
        cancelDeadline(expected);
        expected.completeWithCancellation();
        completions.post(expected);
        return true;
    }

    private void linkActive(Entry<?> entry) {
        entry.activePrevious = activeTail;
        if (activeTail == null) {
            activeHead = entry;
        } else {
            activeTail.activeNext = entry;
        }
        activeTail = entry;
    }

    private void unlinkActive(Entry<?> entry) {
        Entry<?> previous = entry.activePrevious;
        Entry<?> next = entry.activeNext;
        if (previous == null) {
            activeHead = next;
        } else {
            previous.activeNext = next;
        }
        if (next == null) {
            activeTail = previous;
        } else {
            next.activePrevious = previous;
        }
        entry.activePrevious = null;
        entry.activeNext = null;
    }

    private static ZLinkFrameworkException capacityExceeded() {
        return new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.CAPACITY_EXCEEDED,
            "service operation capacity is exhausted");
    }

    private static void cancelDeadline(Entry<?> entry) {
        ScheduledFuture<?> deadline = entry.deadline;
        if (deadline != null) {
            try {
                deadline.cancel(false);
            } catch (RuntimeException ignored) {
                // A timer implementation cannot revoke an accepted terminal.
            }
        }
    }

    public record Operation<T>(UUID id, CompletableFuture<T> completion) {
        public Operation {
            Objects.requireNonNull(id, "id");
            Objects.requireNonNull(completion, "completion");
        }
    }

    private static final class Entry<T>
        extends ZLinkServiceCompletionDispatcher.WorkItem {
        private static final int VALUE = 1;
        private static final int FAILURE = 2;
        private static final int CANCELLATION = 3;

        private final OperationFuture<T> completion = new OperationFuture<>();
        private final TimeoutException timeoutFailure =
            new TimeoutException("service operation timed out");
        private UUID id;
        private volatile ScheduledFuture<?> deadline;
        private Entry<?> activePrevious;
        private Entry<?> activeNext;
        private int terminalKind;
        private Object terminalValue;
        private Throwable terminalFailure;

        void completeWithValue(Object value) {
            terminalKind = VALUE;
            terminalValue = value;
        }

        void completeWithFailure(Throwable failure) {
            terminalKind = FAILURE;
            terminalFailure = failure;
        }

        void completeWithCancellation() {
            terminalKind = CANCELLATION;
        }

        @SuppressWarnings("unchecked")
        @Override
        void dispatch() {
            try {
                switch (terminalKind) {
                    case VALUE -> completion.complete((T) terminalValue);
                    case FAILURE ->
                        completion.completeExceptionally(terminalFailure);
                    case CANCELLATION -> completion.completeCancellation();
                    default -> throw new IllegalStateException(
                        "completion work item has no terminal outcome");
                }
            } finally {
                completion.clearCancellation();
                terminalValue = null;
                terminalFailure = null;
            }
        }

        void dispatchNext(Entry<?> value) {
            setDispatchNext(value);
        }

        Entry<?> dispatchNext() {
            return (Entry<?>) getDispatchNext();
        }
    }

    private static final class OperationFuture<T> extends CompletableFuture<T> {
        private static final Supplier NO_CANCELLATION = () -> false;
        private volatile Supplier cancellation = NO_CANCELLATION;

        void cancellation(Supplier cancellation) {
            this.cancellation = Objects.requireNonNull(cancellation, "cancellation");
        }

        void clearCancellation() {
            cancellation = NO_CANCELLATION;
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
