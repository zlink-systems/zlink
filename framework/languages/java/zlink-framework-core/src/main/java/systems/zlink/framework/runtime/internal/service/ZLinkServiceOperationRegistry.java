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
    private static final long DEADLINE_TICK_NANOS = TimeUnit.MILLISECONDS.toNanos(1);
    private static final int DEADLINE_SLOTS = 256;
    private static final int BITMAP_WORD_BITS = Long.SIZE;
    private final Entry<?>[] deadlineHeads = new Entry<?>[DEADLINE_SLOTS];
    private final Entry<?>[] deadlineTails = new Entry<?>[DEADLINE_SLOTS];
    private final long[] deadlineOccupied = new long[DEADLINE_SLOTS / BITMAP_WORD_BITS];
    private final long deadlineOrigin;
    private long deadlineTick;
    private Entry<?> deadlineCursor;
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
        this.deadlineOrigin = nanoTime.getAsLong();
        this.maintenance =
                scheduler.scheduleAtFixedRate(
                        this::expireMaintenance,
                        DEADLINE_TICK_NANOS,
                        DEADLINE_TICK_NANOS,
                        TimeUnit.NANOSECONDS);
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
                unindexDeadline(current);
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

    private long nextOccupiedTick() {
        int slot = (int) (deadlineTick & (DEADLINE_SLOTS - 1));
        int firstWord = slot / BITMAP_WORD_BITS;
        int words = deadlineOccupied.length;
        for (int step = 0; step <= words; step++) {
            int word = (firstWord + step) % words;
            long bits = deadlineOccupied[word];
            if (step == 0) bits &= -1L << (slot % BITMAP_WORD_BITS);
            if (step == words) bits &= ~(-1L << (slot % BITMAP_WORD_BITS));
            if (bits != 0) {
                int next = word * BITMAP_WORD_BITS + Long.numberOfTrailingZeros(bits);
                return deadlineTick + ((next - slot) & (DEADLINE_SLOTS - 1));
            }
        }
        return -1;
    }

    private void indexDeadline(Entry<?> entry, long targetTick) {
        long tick = Math.max(targetTick, deadlineTick);
        int slot = (int) (tick & (DEADLINE_SLOTS - 1));
        entry.deadlineSlot = slot;
        entry.deadlineRounds = (tick - deadlineTick) / DEADLINE_SLOTS;
        entry.deadlinePrevious = deadlineTails[slot];
        if (entry.deadlinePrevious == null) deadlineHeads[slot] = entry;
        else entry.deadlinePrevious.deadlineNext = entry;
        deadlineTails[slot] = entry;
        deadlineOccupied[slot / BITMAP_WORD_BITS] |= 1L << (slot % BITMAP_WORD_BITS);
    }

    private void unindexDeadline(Entry<?> entry) {
        int slot = entry.deadlineSlot;
        Entry<?> previous = entry.deadlinePrevious;
        Entry<?> next = entry.deadlineNext;
        if (previous == null) deadlineHeads[slot] = next;
        else previous.deadlineNext = next;
        if (next == null) deadlineTails[slot] = previous;
        else next.deadlinePrevious = previous;
        if (deadlineCursor == entry) {
            deadlineCursor = next;
            if (next == null) deadlineTick++;
        }
        if (deadlineHeads[slot] == null)
            deadlineOccupied[slot / BITMAP_WORD_BITS] &= ~(1L << (slot % BITMAP_WORD_BITS));
        entry.deadlinePrevious = null;
        entry.deadlineNext = null;
    }

    int expire(long nowNanos) {
        Entry<?> expiredHead = null;
        Entry<?> expiredTail = null;
        int expired = 0;
        long startedNanos = nanoTime.getAsLong();
        long nowTick = (nowNanos - deadlineOrigin) / DEADLINE_TICK_NANOS;
        // §4.2: the budget counts from entry and bounds every slot visit, a rounds-only visit
        // included; the cursor keeps the rest of the slot for the next maintenance turn.
        while (nanoTime.getAsLong() - startedNanos
                < ZLinkReceiveBatchBudget.DEFAULT_MAX_ELAPSED_NANOS) {
            Entry<?> current;
            synchronized (gate) {
                if (deadlineCursor == null) {
                    long next = nextOccupiedTick();
                    if (next < 0) {
                        deadlineTick = Math.max(deadlineTick, nowTick + 1);
                        break;
                    }
                    if (next > nowTick) break;
                    deadlineTick = next;
                    deadlineCursor = deadlineHeads[(int) (next & (DEADLINE_SLOTS - 1))];
                }
                current = deadlineCursor;
                if (current.deadlineRounds > 0) {
                    current.deadlineRounds--;
                    deadlineCursor = current.deadlineNext;
                    if (deadlineCursor == null) deadlineTick++;
                    continue;
                }
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
        long elapsed = nanoTime.getAsLong() - deadlineOrigin;
        long targetTick =
                elapsed / DEADLINE_TICK_NANOS
                        + timeoutNanos / DEADLINE_TICK_NANOS
                        + (elapsed % DEADLINE_TICK_NANOS
                                        + timeoutNanos % DEADLINE_TICK_NANOS
                                        + DEADLINE_TICK_NANOS
                                        - 1)
                                / DEADLINE_TICK_NANOS;
        entry.completion.cancellation(() -> cancel(entry.id, entry));
        entries.put(id, entry);
        indexDeadline(entry, targetTick);
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

    public record Operation<T>(UUID id, CompletableFuture<T> completion) {
        public Operation {
            Objects.requireNonNull(id, "id");
            Objects.requireNonNull(completion, "completion");
        }
    }

    private static final class Entry<T> extends ZLinkServiceCompletionDispatcher.WorkItem {
        private long deadlineRounds;
        private int deadlineSlot;
        private Entry<?> deadlinePrevious;
        private Entry<?> deadlineNext;

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
