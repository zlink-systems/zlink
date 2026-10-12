package systems.zlink.framework.runtime.internal.service;

import systems.zlink.framework.runtime.internal.ZLinkCompletionBridge;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkReceiveBatchBudget;

import java.time.Duration;
import java.util.HashMap;
import java.util.Map;
import java.util.Objects;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.function.Consumer;
import java.util.function.LongSupplier;
import java.util.function.Supplier;

/** Owns request correlation, deadline, and terminal-once completion. */
public final class ZLinkServiceOperationRegistry implements AutoCloseable {
    private final ScheduledFuture<?> maintenance;
    private final LongSupplier nanoTime;
    private final Supplier<? extends Throwable> timeoutFailure;
    private final ZLinkServiceCompletionDispatcher completions =
            ZLinkServiceCompletionDispatcher.INSTANCE;
    private final Object gate = new Object();
    private final Map<UUID, Entry<?>> entries = new HashMap<>();
    private final Throwable closeFailure;
    private DeadlineNode deadlineRoot;
    private Entry<?> activeHead;
    private Entry<?> activeTail;
    private volatile boolean closed;

    public ZLinkServiceOperationRegistry(ScheduledExecutorService scheduler) {
        this(scheduler, new IllegalStateException("service runtime is closed"));
    }

    public ZLinkServiceOperationRegistry(
            ScheduledExecutorService scheduler, Throwable closeFailure) {
        this(scheduler, closeFailure, () -> new TimeoutException("service operation timed out"));
    }

    public ZLinkServiceOperationRegistry(
            ScheduledExecutorService scheduler,
            Throwable closeFailure,
            Supplier<? extends Throwable> timeoutFailure) {
        this(scheduler, closeFailure, timeoutFailure, System::nanoTime);
    }

    ZLinkServiceOperationRegistry(
            ScheduledExecutorService scheduler, Throwable closeFailure, LongSupplier nanoTime) {
        this(
                scheduler,
                closeFailure,
                () -> new TimeoutException("service operation timed out"),
                nanoTime);
    }

    public ZLinkServiceOperationRegistry(
            ScheduledExecutorService scheduler,
            Throwable closeFailure,
            Supplier<? extends Throwable> timeoutFailure,
            LongSupplier nanoTime) {
        Objects.requireNonNull(scheduler, "scheduler");
        this.closeFailure = Objects.requireNonNull(closeFailure, "closeFailure");
        this.timeoutFailure = Objects.requireNonNull(timeoutFailure, "timeoutFailure");
        this.nanoTime = Objects.requireNonNull(nanoTime, "nanoTime");
        this.maintenance =
                scheduler.scheduleAtFixedRate(this::expireMaintenance, 1, 1, TimeUnit.MILLISECONDS);
    }

    public <T> Operation<T> register(Duration timeout) {
        return register(timeout, null);
    }

    /** Registers the full identity already carried by the service wire. */
    public <T> Operation<T> register(UUID operationId, Duration timeout) {
        return register(timeout, Objects.requireNonNull(operationId, "operationId"));
    }

    private <T> Operation<T> register(Duration timeout, UUID suppliedId) {
        long timeoutNanos = timeoutNanos(timeout);
        synchronized (gate) {
            return registerLocked(timeoutNanos, suppliedId);
        }
    }

    public <T> CompletableFuture<T> submit(
            UUID id,
            Duration timeout,
            Supplier<? extends CompletionStage<T>> submission,
            Consumer<? super T> discardValue) {
        Objects.requireNonNull(id, "id");
        Objects.requireNonNull(submission, "submission");
        Objects.requireNonNull(discardValue, "discardValue");
        long timeoutNanos = timeoutNanos(timeout);
        Operation<T> operation;
        synchronized (gate) {
            if (closed) return CompletableFuture.failedFuture(closeFailure);
            operation = registerLocked(timeoutNanos, id);
        }
        CompletionStage<T> submitted;
        try {
            submitted = Objects.requireNonNull(submission.get(), "submission result");
        } catch (RuntimeException | Error failure) {
            completeExceptionally(operation.id(), failure);
            return operation.completion();
        }
        ZLinkCompletionBridge.forwardCancellation(operation.completion(), submitted);
        submitted.whenComplete(
                (value, failure) -> {
                    if (failure != null) completeExceptionally(operation.id(), failure);
                    else if (!complete(operation.id(), value) && value != null)
                        discardValue.accept(value);
                });
        return operation.completion();
    }

    public <T> boolean complete(UUID id, T value) {
        @SuppressWarnings("unchecked")
        Entry<T> entry = (Entry<T>) take(Objects.requireNonNull(id, "id"));
        if (entry == null) {
            return false;
        }
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
        entry.completeWithFailure(terminal);
        completions.post(entry);
        return true;
    }

    /**
     * Removes an operation whose transport submission was rejected before a request existed. No
     * callback is delivered for such an operation.
     */
    public boolean discard(UUID id) {
        Entry<?> entry = take(Objects.requireNonNull(id, "id"));
        if (entry == null) {
            return false;
        }
        entry.completion.clearCancellation();
        completions.releaseWithoutDispatch(entry);
        return true;
    }

    public int pendingCount() {
        synchronized (gate) {
            return entries.size();
        }
    }

    public boolean isClosed() {
        return closed;
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
            deadlineRoot = null;
        }
        try {
            maintenance.cancel(false);
        } finally {
            completions.postChain(detachedHead, detachedTail);
        }
    }

    private Entry<?> take(UUID id) {
        synchronized (gate) {
            return takeLocked(id);
        }
    }

    private Entry<?> takeLocked(UUID id) {
        Entry<?> entry = entries.remove(id);
        if (entry != null) {
            unindexDeadline(entry);
            unlinkActive(entry);
        }
        return entry;
    }

    private boolean cancel(UUID id, Entry<?> expected) {
        synchronized (gate) {
            if (entries.get(id) != expected || expected.completion.isDone()) {
                return false;
            }
            takeLocked(id);
        }
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

    private static final int DEADLINE_KEY_BITS = 3 * Long.SIZE;

    private static int keyBit(Entry<?> entry, int bit) {
        long word =
                bit < Long.SIZE
                        ? entry.prefix ^ Long.MIN_VALUE
                        : bit < 2 * Long.SIZE
                                ? entry.id.getMostSignificantBits()
                                : entry.id.getLeastSignificantBits();
        return (int) ((word >>> (Long.SIZE - 1 - bit % Long.SIZE)) & 1);
    }

    private static DeadlineNode child(DeadlineNode node, int side) {
        DeadlineBranch branch = (DeadlineBranch) node;
        return side == 0 ? branch.left : branch.right;
    }

    private void indexDeadline(Entry<?> entry) {
        if (deadlineRoot == null) {
            deadlineRoot = entry;
            return;
        }
        DeadlineNode other = deadlineRoot;
        while (other.bit() < DEADLINE_KEY_BITS) other = child(other, keyBit(entry, other.bit()));
        int differing = 0;
        while (keyBit(entry, differing) == keyBit((Entry<?>) other, differing)) differing++;
        DeadlineNode parent = null;
        DeadlineNode current = deadlineRoot;
        while (current.bit() < differing) {
            parent = current;
            current = child(current, keyBit(entry, current.bit()));
        }
        DeadlineBranch branch =
                new DeadlineBranch(
                        differing,
                        entry.prefix,
                        keyBit(entry, differing) == 0 ? entry : current,
                        keyBit(entry, differing) == 0 ? current : entry);
        if (parent == null) deadlineRoot = branch;
        else if (keyBit(entry, parent.bit()) == 0) ((DeadlineBranch) parent).left = branch;
        else ((DeadlineBranch) parent).right = branch;
    }

    private void unindexDeadline(Entry<?> entry) {
        DeadlineNode parent = null;
        DeadlineNode grandparent = null;
        DeadlineNode current = deadlineRoot;
        while (current.bit() < DEADLINE_KEY_BITS) {
            grandparent = parent;
            parent = current;
            current = child(current, keyBit(entry, current.bit()));
        }
        if (parent == null) deadlineRoot = null;
        else {
            DeadlineNode sibling =
                    ((DeadlineBranch) parent).left == entry
                            ? ((DeadlineBranch) parent).right
                            : ((DeadlineBranch) parent).left;
            if (grandparent == null) deadlineRoot = sibling;
            else if (((DeadlineBranch) grandparent).left == parent)
                ((DeadlineBranch) grandparent).left = sibling;
            else ((DeadlineBranch) grandparent).right = sibling;
        }
    }

    private static Entry<?> firstDeadline(DeadlineNode node) {
        if (node == null) return null;
        while (node.bit() < DEADLINE_KEY_BITS) node = ((DeadlineBranch) node).left;
        return (Entry<?>) node;
    }

    private static Entry<?> deadlineAtOrAfter(DeadlineNode node, long lowerBound) {
        if (node == null) return null;
        long key = node.prefix() ^ Long.MIN_VALUE;
        if (node.bit() >= Long.SIZE)
            return Long.compareUnsigned(key, lowerBound) >= 0 ? firstDeadline(node) : null;
        long lowerBits = -1L >>> node.bit();
        long minimum = key & ~lowerBits;
        if (Long.compareUnsigned(minimum, lowerBound) >= 0) return firstDeadline(node);
        if (Long.compareUnsigned(minimum | lowerBits, lowerBound) < 0) return null;
        Entry<?> first = deadlineAtOrAfter(((DeadlineBranch) node).left, lowerBound);
        return first != null ? first : deadlineAtOrAfter(((DeadlineBranch) node).right, lowerBound);
    }

    private Entry<?> firstExpired(long nowNanos) {
        // nanoTime is circular: inspect the elapsed half of its fixed-width domain.
        long oldest = nowNanos - Long.MAX_VALUE;
        Entry<?> first = deadlineAtOrAfter(deadlineRoot, oldest ^ Long.MIN_VALUE);
        if (first == null && oldest > nowNanos) first = firstDeadline(deadlineRoot);
        return first != null && deadlineReached(nowNanos, first.prefix) ? first : null;
    }

    int expire(long nowNanos) {
        Entry<?> expiredHead = null;
        Entry<?> expiredTail = null;
        int expired = 0;
        long startedNanos = nanoTime.getAsLong();
        while (nanoTime.getAsLong() - startedNanos
                < ZLinkReceiveBatchBudget.DEFAULT_MAX_ELAPSED_NANOS) {
            Entry<?> current;
            synchronized (gate) {
                current = firstExpired(nowNanos);
                if (current == null) break;
                takeLocked(current.id);
            }
            current.completeWithFailure(
                    Objects.requireNonNull(timeoutFailure.get(), "timeout failure"));
            current.dispatchNext(null);
            if (expiredTail == null) expiredHead = current;
            else expiredTail.dispatchNext(current);
            expiredTail = current;
            expired++;
        }
        completions.postChain(expiredHead, expiredTail);
        return expired;
    }

    private void expireMaintenance() {
        expire(nanoTime.getAsLong());
    }

    private <T> Operation<T> registerLocked(long timeoutNanos, UUID suppliedId) {
        if (closed) {
            throw new IllegalStateException("operation registry is closed");
        }
        UUID id = suppliedId;
        if (id == null) {
            do {
                id = ZLinkServiceOperationIds.next();
            } while (entries.containsKey(id));
        } else if ((id.getMostSignificantBits() == 0 && id.getLeastSignificantBits() == 0)
                || entries.containsKey(id)) {
            throw new IllegalArgumentException("operation identity is zero or already pending");
        }
        Entry<T> entry = new Entry<>();
        completions.register(entry);
        entry.id = id;
        entry.prefix = nanoTime.getAsLong() + timeoutNanos;
        entry.completion.cancellation(() -> cancel(entry.id, entry));
        entries.put(id, entry);
        indexDeadline(entry);
        linkActive(entry);
        return new Operation<>(id, entry.completion);
    }

    private static long timeoutNanos(Duration timeout) {
        Objects.requireNonNull(timeout, "timeout");
        if (timeout.isNegative() || timeout.isZero()) {
            throw new IllegalArgumentException("timeout must be positive");
        }
        try {
            return timeout.toNanos();
        } catch (ArithmeticException overflow) {
            return Long.MAX_VALUE;
        }
    }

    private static boolean deadlineReached(long nowNanos, long deadlineNanos) {
        return nowNanos - deadlineNanos >= 0;
    }

    public record Operation<T>(UUID id, CompletableFuture<T> completion) {
        public Operation {
            Objects.requireNonNull(id, "id");
            Objects.requireNonNull(completion, "completion");
        }
    }

    private sealed interface DeadlineNode permits DeadlineBranch, Entry {
        int bit();

        long prefix();
    }

    private static final class DeadlineBranch implements DeadlineNode {
        private final int bit;
        private final long prefix;
        private DeadlineNode left;
        private DeadlineNode right;

        private DeadlineBranch(int bit, long prefix, DeadlineNode left, DeadlineNode right) {
            this.bit = bit;
            this.prefix = prefix;
            this.left = left;
            this.right = right;
        }

        @Override
        public int bit() {
            return bit;
        }

        @Override
        public long prefix() {
            return prefix;
        }
    }

    private static final class Entry<T> extends ZLinkServiceCompletionDispatcher.WorkItem
            implements DeadlineNode {
        private long prefix;

        @Override
        public int bit() {
            return DEADLINE_KEY_BITS;
        }

        @Override
        public long prefix() {
            return prefix;
        }

        private static final int VALUE = 1;
        private static final int FAILURE = 2;
        private static final int CANCELLATION = 3;

        private final OperationFuture<T> completion = new OperationFuture<>();
        private UUID id;
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
                    case FAILURE -> completion.completeExceptionally(terminalFailure);
                    case CANCELLATION -> completion.completeCancellation();
                    default ->
                            throw new IllegalStateException(
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
