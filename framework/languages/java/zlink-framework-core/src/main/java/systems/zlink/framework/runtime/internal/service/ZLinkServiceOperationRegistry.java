package systems.zlink.framework.runtime.internal.service;

import java.time.Duration;
import java.util.Arrays;
import java.util.Objects;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.BiConsumer;
import java.util.function.Consumer;
import java.util.function.Function;
import java.util.function.LongSupplier;
import java.util.function.Supplier;

/** Owns operation correlation, request deadlines, and terminal-once completion. */
public final class ZLinkServiceOperationRegistry implements AutoCloseable {
    private final ScheduledFuture<?> maintenance;
    private final LongSupplier nanoTime;
    private final Supplier<? extends Throwable> timeoutFailure;
    private final ZLinkServiceCompletionDispatcher completions =
            ZLinkServiceCompletionDispatcher.INSTANCE;
    private final Object gate = new Object();
    private static final int INITIAL_SLOT_COUNT = 64;
    private static final int HASH_SPREAD_SHIFT = 16;
    private volatile Entry<?>[] slots = new Entry<?>[0];
    private Entry<?>[] buckets = new Entry<?>[INITIAL_SLOT_COUNT];
    // Only the registry turn pops; completed/discarded registrations push after release.
    private final AtomicInteger freeSlot = new AtomicInteger();
    private int pendingCount;
    private final Throwable closeFailure;
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
        growSlots();
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
            return registerLocked(timeoutNanos, suppliedId, Kind.REQUEST);
        }
    }

    /** Registers a backpressured send before observing its binding admission terminal. */
    public CompletionStage<Void> submitSend(Supplier<CompletionStage<Void>> admitted) {
        Objects.requireNonNull(admitted, "admitted");
        Operation<Void> operation;
        synchronized (gate) {
            if (closed) return CompletableFuture.failedStage(closeFailure);
            operation = registerLocked(0, null, Kind.SEND);
        }
        UUID id = operation.id();
        try {
            Objects.requireNonNull(admitted.get(), "binding admission")
                    .whenComplete(
                            (ignored, failure) -> {
                                if (failure != null) completeExceptionally(id, failure);
                                else complete(id, null);
                            });
        } catch (RuntimeException | Error failure) {
            completeExceptionally(id, failure);
        }
        // JVM cancellation ends only this caller's wait, never the send or binding admission.
        return operation.minimalCompletionStage();
    }

    public <T> CompletableFuture<T> submit(
            UUID id,
            Duration timeout,
            Supplier<? extends CompletionStage<T>> submission,
            Consumer<? super T> discardValue) {
        Objects.requireNonNull(submission, "submission");
        return submitTerminal(
                id,
                timeout,
                terminal -> {
                    CompletionStage<T> submitted =
                            Objects.requireNonNull(submission.get(), "submission result");
                    submitted.whenComplete(terminal);
                    return submitted;
                },
                discardValue);
    }

    /**
     * Registers an operation whose submission reports its terminal straight to this entry (spec 08
     * §3 E3/E5): the submission receives the entry's terminal callback and returns the stage that
     * caller cancellation must reach, or {@code null} when nothing remains to cancel. A value that
     * arrives after the entry ended (timeout, cancellation, close) goes to {@code discardValue}.
     */
    public <T> CompletableFuture<T> submitTerminal(
            UUID id,
            Duration timeout,
            Function<BiConsumer<T, Throwable>, CompletionStage<?>> submission,
            Consumer<? super T> discardValue) {
        Objects.requireNonNull(id, "id");
        Objects.requireNonNull(submission, "submission");
        Objects.requireNonNull(discardValue, "discardValue");
        long timeoutNanos = timeoutNanos(timeout);
        Operation<T> operation;
        synchronized (gate) {
            if (closed) return CompletableFuture.failedFuture(closeFailure);
            operation = registerLocked(timeoutNanos, id, Kind.REQUEST);
        }
        BiConsumer<T, Throwable> terminal =
                (value, failure) -> {
                    if (failure != null) completeExceptionally(id, failure);
                    else if (!complete(id, value) && value != null) discardValue.accept(value);
                };
        CompletionStage<?> cancellationTarget;
        try {
            cancellationTarget = submission.apply(terminal);
        } catch (RuntimeException | Error failure) {
            completeExceptionally(id, failure);
            return operation.completion();
        }
        if (cancellationTarget != null) operation.forwardCancellationTo(cancellationTarget);
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
            return pendingCount;
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
                takeLocked(current.id);
                current.dispatchNext(next);
                current.completeWithFailure(closeFailure);
                current = next;
            }
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
        Entry<?> entry = removeEntry(id);
        if (entry != null) {
            unlinkActive(entry);
        }
        return entry;
    }

    private boolean cancel(int slot, long generation) {
        Entry<?> expected;
        synchronized (gate) {
            expected = slots[slot];
            if (expected.generation != generation
                    || findEntry(expected.id) != expected
                    || expected.completion.isDone()) {
                return false;
            }
            takeLocked(expected.id);
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

    int expire(long nowNanos) {
        Entry<?> expiredHead = null;
        Entry<?> expiredTail = null;
        int expired = 0;
        synchronized (gate) {
            Entry<?> current = activeHead;
            while (current != null) {
                Entry<?> next = current.activeNext;
                if (current.kind == Kind.REQUEST
                        && deadlineReached(nowNanos, current.deadlineNanos)) {
                    takeLocked(current.id);
                    current.completeWithFailure(
                            Objects.requireNonNull(timeoutFailure.get(), "timeout failure"));
                    current.dispatchNext(null);
                    if (expiredTail == null) {
                        expiredHead = current;
                    } else {
                        expiredTail.dispatchNext(current);
                    }
                    expiredTail = current;
                    expired++;
                }
                current = next;
            }
        }
        completions.postChain(expiredHead, expiredTail);
        return expired;
    }

    private void expireMaintenance() {
        expire(nanoTime.getAsLong());
    }

    private <T> Operation<T> registerLocked(long timeoutNanos, UUID suppliedId, Kind kind) {
        if (closed) {
            throw new IllegalStateException("operation registry is closed");
        }
        UUID id = suppliedId;
        if (id == null) {
            do {
                id = ZLinkServiceOperationIds.next();
            } while (findEntry(id) != null);
        } else if ((id.getMostSignificantBits() == 0 && id.getLeastSignificantBits() == 0)
                || findEntry(id) != null) {
            throw new IllegalArgumentException("operation identity is zero or already pending");
        }
        @SuppressWarnings("unchecked")
        Entry<T> entry = (Entry<T>) claimSlot();
        completions.register(entry);
        entry.id = id;
        entry.kind = kind;
        if (kind == Kind.REQUEST) entry.deadlineNanos = nanoTime.getAsLong() + timeoutNanos;
        entry.generation++;
        Operation<T> operation =
                new Operation<>(
                        kind == Kind.REQUEST ? this : null, entry.slot, entry.generation, id);
        entry.completion = operation;
        int bucket = bucket(id);
        entry.hashNext = buckets[bucket];
        buckets[bucket] = entry;
        pendingCount++;
        linkActive(entry);
        return operation;
    }

    private int bucket(UUID id) {
        int hash = id.hashCode();
        return (hash ^ (hash >>> HASH_SPREAD_SHIFT)) & (buckets.length - 1);
    }

    private Entry<?> findEntry(UUID id) {
        if (id == null) return null;
        Entry<?> entry = buckets[bucket(id)];
        while (entry != null && !id.equals(entry.id)) entry = entry.hashNext;
        return entry;
    }

    private Entry<?> removeEntry(UUID id) {
        int bucket = bucket(id);
        Entry<?> previous = null;
        Entry<?> entry = buckets[bucket];
        while (entry != null) {
            if (id.equals(entry.id)) {
                if (previous == null) buckets[bucket] = entry.hashNext;
                else previous.hashNext = entry.hashNext;
                entry.hashNext = null;
                pendingCount--;
                return entry;
            }
            previous = entry;
            entry = entry.hashNext;
        }
        return null;
    }

    private Entry<?> claimSlot() {
        if (freeSlot.get() == 0) growSlots();
        int head;
        Entry<?> entry;
        do {
            head = freeSlot.get();
            entry = slots[head - 1];
        } while (!freeSlot.compareAndSet(head, entry.freeNext));
        return entry;
    }

    private void growSlots() {
        int oldSize = slots.length;
        int size = oldSize == 0 ? INITIAL_SLOT_COUNT : Math.multiplyExact(oldSize, 2);
        Entry<?>[] expanded = Arrays.copyOf(slots, size);
        for (int index = oldSize; index < size; index++) {
            expanded[index] = new Entry<>(this, index);
            expanded[index].freeNext = index + 2;
        }
        Entry<?> last = expanded[size - 1];
        slots = expanded;
        if (size > buckets.length) {
            buckets = new Entry<?>[size];
            for (Entry<?> entry = activeHead; entry != null; entry = entry.activeNext) {
                int bucket = bucket(entry.id);
                entry.hashNext = buckets[bucket];
                buckets[bucket] = entry;
            }
        }
        int head;
        do {
            head = freeSlot.get();
            last.freeNext = head;
        } while (!freeSlot.compareAndSet(head, oldSize + 1));
    }

    private void releaseSlot(Entry<?> entry) {
        entry.id = null;
        entry.completion = null;
        entry.hashNext = null;
        entry.terminalKind = Entry.NONE;
        int head;
        do {
            head = freeSlot.get();
            entry.freeNext = head;
        } while (!freeSlot.compareAndSet(head, entry.slot + 1));
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

    public static final class Operation<T> extends CompletableFuture<T> {
        private volatile ZLinkServiceOperationRegistry owner;
        // The submitted stage a caller cancellation reaches once this operation is cancelled.
        private volatile CompletionStage<?> cancellationTarget;
        private final int slot;
        private final long generation;
        private final UUID id;

        private Operation(ZLinkServiceOperationRegistry owner, int slot, long generation, UUID id) {
            this.owner = owner;
            this.slot = slot;
            this.generation = generation;
            this.id = id;
        }

        public UUID id() {
            return id;
        }

        public CompletableFuture<T> completion() {
            return this;
        }

        void clearCancellation() {
            owner = null;
        }

        @Override
        public boolean cancel(boolean mayInterruptIfRunning) {
            ZLinkServiceOperationRegistry registry = owner;
            return registry != null && registry.cancel(slot, generation);
        }

        void completeCancellation() {
            super.cancel(false);
            CompletionStage<?> target = cancellationTarget;
            if (target != null) target.toCompletableFuture().cancel(false);
        }

        private void forwardCancellationTo(CompletionStage<?> target) {
            cancellationTarget = target;
            if (isCancelled()) target.toCompletableFuture().cancel(false);
        }
    }

    private enum Kind {
        REQUEST,
        SEND
    }

    private static final class Entry<T> extends ZLinkServiceCompletionDispatcher.WorkItem {
        private static final int NONE = 0;
        private static final int VALUE = 1;
        private static final int FAILURE = 2;
        private static final int CANCELLATION = 3;

        private final ZLinkServiceOperationRegistry owner;
        private final int slot;
        private int freeNext;
        private long generation;
        private Entry<?> hashNext;
        private Operation<T> completion;
        private UUID id;
        private Kind kind;
        private long deadlineNanos;
        private Entry<?> activePrevious;
        private Entry<?> activeNext;
        private int terminalKind = NONE;
        private Object terminalValue;
        private Throwable terminalFailure;

        Entry(ZLinkServiceOperationRegistry owner, int slot) {
            this.owner = owner;
            this.slot = slot;
        }

        @Override
        void released() {
            owner.releaseSlot(this);
        }

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
    }
}
