package systems.zlink.framework.runtime.internal.service;

import systems.zlink.framework.runtime.internal.ZLinkCompletionBridge;
import systems.zlink.framework.runtime.internal.calls.ZLinkBlockingCalls;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkReceiveBatchBudget;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

import java.lang.invoke.MethodHandles;
import java.lang.invoke.VarHandle;
import java.time.Duration;
import java.util.Arrays;
import java.util.Objects;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ThreadFactory;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.locks.LockSupport;
import java.util.function.BiConsumer;
import java.util.function.Consumer;
import java.util.function.Function;
import java.util.function.LongSupplier;
import java.util.function.Supplier;

/** Owns operation correlation, request deadlines, and terminal-once completion. */
public final class ZLinkServiceOperationRegistry implements AutoCloseable {
    private final ThreadFactory maintenanceFactory;
    private Thread maintenance;
    // The worker's reserved wheel visit survives removal of the last pending request.
    private volatile long maintenanceTick = -1;
    private final LongSupplier nanoTime;
    private final Supplier<? extends Throwable> timeoutFailure;
    private final ZLinkServiceCompletionDispatcher completions =
            ZLinkServiceCompletionDispatcher.INSTANCE;
    private final ZLinkStateLane owner;
    private final ConcurrentHashMap<UUID, Entry<?>> pending;
    private final Throwable closeFailure;
    private static final long DEADLINE_TICK_NANOS = TimeUnit.MILLISECONDS.toNanos(1);
    private static final int DEADLINE_SLOTS = 256;
    private static final int BITMAP_WORD_BITS = Long.SIZE;
    private final Entry<?>[] deadlineHeads = new Entry<?>[DEADLINE_SLOTS];
    private final Entry<?>[] deadlineTails = new Entry<?>[DEADLINE_SLOTS];
    private final long[] deadlineOccupied = new long[DEADLINE_SLOTS / BITMAP_WORD_BITS];
    private final long deadlineOrigin;
    private long deadlineTick;
    private long maintenanceVisitedTick;
    private long deadlineSweepEndTick = -1;
    private long deadlineNextVisitTick;
    private static final long VISIT_IN_FLIGHT = -2;
    private Entry<?> deadlineCursor;
    private volatile boolean closed;

    long maintenanceTickForTest() {
        return maintenanceTick;
    }

    ZLinkStateLane ownerLaneForTest() {
        return owner;
    }

    Object[] deadlineHeadsForTest() {
        return deadlineHeads.clone();
    }

    Object[] deadlineTailsForTest() {
        return deadlineTails.clone();
    }

    Object deadlineCursorForTest() {
        return deadlineCursor;
    }

    int deadlineEntryCountForTest() {
        int count = 0;
        for (Entry<?> entry : deadlineHeads) {
            while (entry != null) {
                count++;
                entry = entry.deadlineNext;
            }
        }
        return count;
    }

    Object pendingEntryForTest(UUID id) {
        return pending.get(id);
    }

    java.util.Set<UUID> pendingIdsForTest() {
        return java.util.Set.copyOf(pending.keySet());
    }

    Object pendingReceiverForTest() {
        if (pending.size() != 1) throw new IllegalStateException("expected one pending operation");
        return pending.values().iterator().next().completion;
    }

    boolean pendingRegisteredForTest() {
        if (pending.size() != 1) throw new IllegalStateException("expected one pending operation");
        return pending.values().iterator().next().registeredForTest();
    }

    public ZLinkServiceOperationRegistry() {
        this(new IllegalStateException("service runtime is closed"));
    }

    public ZLinkServiceOperationRegistry(Throwable closeFailure) {
        this(closeFailure, () -> new TimeoutException("service operation timed out"));
    }

    ZLinkServiceOperationRegistry(ThreadFactory maintenanceFactory) {
        this(maintenanceFactory, new IllegalStateException("service runtime is closed"));
    }

    ZLinkServiceOperationRegistry(ThreadFactory maintenanceFactory, Throwable closeFailure) {
        this(maintenanceFactory, closeFailure, System::nanoTime);
    }

    public ZLinkServiceOperationRegistry(
            Throwable closeFailure, Supplier<? extends Throwable> timeoutFailure) {
        this(closeFailure, timeoutFailure, System::nanoTime);
    }

    ZLinkServiceOperationRegistry(
            ThreadFactory maintenanceFactory, Throwable closeFailure, LongSupplier nanoTime) {
        this(
                maintenanceFactory,
                closeFailure,
                () -> new TimeoutException("service operation timed out"),
                nanoTime);
    }

    public ZLinkServiceOperationRegistry(
            Throwable closeFailure,
            Supplier<? extends Throwable> timeoutFailure,
            LongSupplier nanoTime) {
        this(
                Thread.ofVirtual().name("zlink-jvm-operation-maintenance").factory(),
                closeFailure,
                timeoutFailure,
                nanoTime);
    }

    ZLinkServiceOperationRegistry(
            ThreadFactory maintenanceFactory,
            Throwable closeFailure,
            Supplier<? extends Throwable> timeoutFailure,
            LongSupplier nanoTime) {
        this(new ZLinkStateLane(), maintenanceFactory, closeFailure, timeoutFailure, nanoTime);
    }

    public ZLinkServiceOperationRegistry(ZLinkStateLane owner, Throwable closeFailure) {
        this(
                owner,
                Thread.ofVirtual().name("zlink-jvm-operation-maintenance").factory(),
                closeFailure,
                () -> new TimeoutException("service operation timed out"),
                System::nanoTime);
    }

    public ZLinkServiceOperationRegistry(
            ZLinkStateLane owner,
            Throwable closeFailure,
            Supplier<? extends Throwable> timeoutFailure,
            LongSupplier nanoTime) {
        this(
                owner,
                Thread.ofVirtual().name("zlink-jvm-operation-maintenance").factory(),
                closeFailure,
                timeoutFailure,
                nanoTime);
    }

    ZLinkServiceOperationRegistry(
            ZLinkStateLane owner,
            ThreadFactory maintenanceFactory,
            Throwable closeFailure,
            Supplier<? extends Throwable> timeoutFailure,
            LongSupplier nanoTime) {
        this(
                owner,
                maintenanceFactory,
                closeFailure,
                timeoutFailure,
                nanoTime,
                new ConcurrentHashMap<>());
    }

    ZLinkServiceOperationRegistry(
            ZLinkStateLane owner,
            ThreadFactory maintenanceFactory,
            Throwable closeFailure,
            Supplier<? extends Throwable> timeoutFailure,
            LongSupplier nanoTime,
            ConcurrentHashMap<UUID, Entry<?>> pending) {
        this.pending = Objects.requireNonNull(pending, "pending");
        this.owner = Objects.requireNonNull(owner, "owner");
        this.maintenanceFactory = Objects.requireNonNull(maintenanceFactory, "maintenanceFactory");
        this.closeFailure = Objects.requireNonNull(closeFailure, "closeFailure");
        this.timeoutFailure = Objects.requireNonNull(timeoutFailure, "timeoutFailure");
        this.nanoTime = Objects.requireNonNull(nanoTime, "nanoTime");
        this.deadlineOrigin = nanoTime.getAsLong();
    }

    public <T> CompletionStage<Operation<T>> register(Duration timeout) {
        return register(timeout, null);
    }

    /** Registers the full identity already carried by the service wire. */
    public <T> CompletionStage<Operation<T>> register(UUID operationId, Duration timeout) {
        return register(timeout, Objects.requireNonNull(operationId, "operationId"));
    }

    private <T> CompletionStage<Operation<T>> register(Duration timeout, UUID suppliedId) {
        long timeoutNanos = timeoutNanos(timeout);
        long deadline = nanoTime.getAsLong() + timeoutNanos;
        return submitOwnerTurn(() -> registerOnOwner(deadline, suppliedId, Kind.REQUEST));
    }

    /** Direct registration for a submission already executing on this registry's owner. */
    public <T> Operation<T> registerOnOwner(Duration timeout) {
        if (!owner.isOnLane())
            throw new IllegalStateException("registration requires its owner turn");
        return registerOnOwner(nanoTime.getAsLong() + timeoutNanos(timeout), null, Kind.REQUEST);
    }

    /** Registers a backpressured send before observing its binding admission terminal. */
    public CompletionStage<Void> submitSend(Supplier<CompletionStage<Void>> admitted) {
        Objects.requireNonNull(admitted, "admitted");
        var completion = new CompletableFuture<Void>();
        submitOwnerTurn(
                        () -> {
                            if (closed) {
                                ZLinkServiceCompletionDispatcher.deliver(
                                        CompletableFuture.failedStage(closeFailure), completion);
                                return false;
                            }
                            UUID id = ZLinkServiceOperationIds.next();
                            Entry<Void> entry = registerEntry(0, id, Kind.SEND, completion, null);
                            try {
                                Objects.requireNonNull(admitted.get(), "binding admission")
                                        .whenComplete(
                                                (ignored, failure) -> {
                                                    if (failure != null)
                                                        completeExceptionally(entry, failure);
                                                    else complete(entry, null);
                                                });
                            } catch (RuntimeException | Error failure) {
                                completeExceptionally(entry, failure);
                            }
                            return true;
                        },
                        true)
                .whenComplete(
                        (accepted, failure) -> {
                            if (failure != null)
                                ZLinkServiceCompletionDispatcher.deliver(
                                        CompletableFuture.failedStage(failure), completion);
                        });
        // Cancellation ends only this caller's view, never physical send admission.
        return completion.minimalCompletionStage();
    }

    private <T> CompletionStage<T> submitOwnerTurn(Supplier<T> work) {
        return submitOwnerTurn(work, false);
    }

    private <T> CompletionStage<T> submitOwnerTurn(Supplier<T> work, boolean submission) {
        if (owner.isOnLane()) {
            try {
                return CompletableFuture.completedFuture(work.get());
            } catch (RuntimeException | Error failure) {
                return CompletableFuture.failedStage(failure);
            }
        }
        var flow = ZLinkFlowContext.current();
        if (submission && !ZLinkBlockingCalls.isRuntimeExecutionContext()) {
            try {
                return CompletableFuture.completedFuture(
                        owner.awaitTurn(() -> ZLinkFlowContext.call(flow, work)));
            } catch (RuntimeException | Error failure) {
                return CompletableFuture.failedStage(failure);
            }
        }
        return owner.runAsync(() -> ZLinkFlowContext.call(flow, work));
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
     * §3 E3/E5): the submission receives the entry's terminal callback. Its returned producer stage
     * remains owned by the submission so physical processing can finish after logical terminal. A
     * value that arrives after the entry ended (timeout, cancellation, close) goes to {@code
     * discardValue}.
     */
    public <T> CompletableFuture<T> submitTerminal(
            UUID id,
            Duration timeout,
            Function<BiConsumer<T, Throwable>, CompletionStage<?>> submission,
            Consumer<? super T> discardValue) {
        Objects.requireNonNull(id, "id");
        Objects.requireNonNull(submission, "submission");
        Objects.requireNonNull(discardValue, "discardValue");
        Operation<T> completion = prepareOperation(id);
        submitTerminal(id, timeout, submission, discardValue, completion);
        return completion;
    }

    /** Uses the caller's pre-created typed receiver as the registered completion destination. */
    public <T> CompletionStage<Boolean> submitTerminal(
            UUID id,
            Duration timeout,
            Function<BiConsumer<T, Throwable>, CompletionStage<?>> submission,
            Consumer<? super T> discardValue,
            Operation<T> completion) {
        Objects.requireNonNull(id, "id");
        Objects.requireNonNull(submission, "submission");
        Objects.requireNonNull(discardValue, "discardValue");
        Objects.requireNonNull(completion, "completion");
        if (!completion.id().equals(id))
            throw new IllegalArgumentException("receiver and operation identities differ");
        long nanos = timeoutNanos(timeout);
        long deadline = nanoTime.getAsLong() + nanos;
        var registration =
                submitOwnerTurn(
                        () -> {
                            if (!completion.beginRegistration()) return false;
                            if (closed) {
                                if (!completion.finishRegistration(this)) return false;
                                ZLinkServiceCompletionDispatcher.deliver(
                                        CompletableFuture.failedStage(closeFailure), completion);
                                return false;
                            }
                            long remaining = deadline - nanoTime.getAsLong();
                            if (remaining <= 0) {
                                if (!completion.finishRegistration(this)) return false;
                                ZLinkServiceCompletionDispatcher.deliver(
                                        CompletableFuture.failedStage(timeoutFailure.get()),
                                        completion);
                                return false;
                            }
                            Entry<T> entry =
                                    registerEntry(
                                            deadline, id, Kind.REQUEST, completion, discardValue);
                            if (!completion.finishRegistration(this)) {
                                abandonRegistration(entry);
                                return false;
                            }
                            if (!isPending(id)) return false;
                            submitOnOwner(entry, submission, discardValue);
                            return true;
                        },
                        true);
        registration.whenComplete(
                (accepted, failure) -> {
                    if (failure != null && completion.rejectRegistration(this))
                        ZLinkServiceCompletionDispatcher.deliver(
                                CompletableFuture.failedStage(failure), completion);
                });
        return registration;
    }

    private <T> void submitOnOwner(
            Entry<T> entry,
            Function<BiConsumer<T, Throwable>, CompletionStage<?>> submission,
            Consumer<? super T> discardValue) {
        BiConsumer<T, Throwable> terminal =
                (value, failure) -> {
                    if (failure != null) completeExceptionally(entry, failure);
                    else if (!complete(entry, value) && value != null) discardValue.accept(value);
                };
        try {
            submission.apply(terminal);
        } catch (RuntimeException | Error failure) {
            completeExceptionally(entry, failure);
        }
    }

    public <T> boolean complete(UUID id, T value) {
        @SuppressWarnings("unchecked")
        Entry<T> entry = (Entry<T>) pending.get(Objects.requireNonNull(id, "id"));
        return complete(entry, value);
    }

    private <T> boolean complete(Entry<T> entry, T value) {
        if (entry == null || take(entry) == null) return false;
        entry.completeWithValue(value);
        completions.post(entry);
        return true;
    }

    public boolean completeExceptionally(UUID id, Throwable failure) {
        Objects.requireNonNull(failure, "failure");
        return completeExceptionally(pending.get(Objects.requireNonNull(id, "id")), failure);
    }

    private boolean completeExceptionally(Entry<?> entry, Throwable failure) {
        if (entry == null || take(entry) == null) return false;
        entry.completeWithFailure(failure);
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
        entry.clearCancellation();
        completions.releaseWithoutDispatch(entry);
        return true;
    }

    /** The terminal table alone owns whether a logical request is still pending. */
    public boolean isPending(UUID id) {
        return pending.containsKey(Objects.requireNonNull(id, "id"));
    }

    public int pendingCount() {
        return pending.size();
    }

    /** Reads the one deadline owned by the registered logical request. */
    public Duration remainingTimeout(UUID id) {
        Entry<?> entry = pending.get(Objects.requireNonNull(id, "id"));
        if (entry == null) return Duration.ZERO;
        return Duration.ofNanos(Math.max(0L, entry.deadlineNanos - nanoTime.getAsLong()));
    }

    public boolean isClosed() {
        return closed;
    }

    @Override
    public void close() {
        var closing = submitOwnerTurn(this::closeOnOwner);
        // A handler or lane turn never waits for infrastructure progress.
        if (ZLinkStateLane.current() != null) return;
        Thread worker = ZLinkCompletionBridge.await(closing);
        if (worker != null && worker != Thread.currentThread()) {
            boolean interrupted = false;
            while (worker.isAlive()) {
                try {
                    worker.join();
                } catch (InterruptedException failure) {
                    interrupted = true;
                }
            }
            if (interrupted) Thread.currentThread().interrupt();
        }
    }

    public CompletionStage<Void> closeAsync() {
        return submitOwnerTurn(
                () -> {
                    closeOnOwner();
                    return null;
                });
    }

    private Thread closeOnOwner() {
        if (closed) return null;
        closed = true;
        LockSupport.unpark(maintenance);
        cancelPendingOnOwner();
        Arrays.fill(deadlineHeads, null);
        Arrays.fill(deadlineTails, null);
        Arrays.fill(deadlineOccupied, 0);
        deadlineCursor = null;
        return maintenance;
    }

    public CompletionStage<Void> cancelPendingOperations() {
        return submitOwnerTurn(
                () -> {
                    cancelPendingOnOwner();
                    return null;
                });
    }

    private void cancelPendingOnOwner() {
        for (UUID id : pending.keySet()) completeExceptionally(id, closeFailure);
    }

    private Entry<?> take(UUID id) {
        Entry<?> entry = pending.get(id);
        return entry == null ? null : take(entry);
    }

    private Entry<?> take(Entry<?> entry) {
        if (entry.completion instanceof Operation<?> operation
                && !operation.finishRegistration(this)) return null;
        return pending.remove(entry.id, entry) ? entry : null;
    }

    private void abandonRegistration(Entry<?> entry) {
        if (!pending.remove(entry.id, entry)) return;
        completions.releaseWithoutDispatch(entry);
    }

    private boolean cancel(UUID id, CompletableFuture<?> caller) {
        Entry<?> entry = pending.get(id);
        if (entry == null || entry.completion != caller || take(entry) == null) return false;
        entry.completeWithCancellation();
        completions.post(entry);
        return true;
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
        long tick = targetTick;
        int slot = (int) (tick & (DEADLINE_SLOTS - 1));
        entry.deadlineSlot = slot;
        entry.deadlineTick = tick;
        entry.deadlinePrevious = deadlineTails[slot];
        if (entry.deadlinePrevious == null) deadlineHeads[slot] = entry;
        else entry.deadlinePrevious.deadlineNext = entry;
        deadlineTails[slot] = entry;
        deadlineOccupied[slot / BITMAP_WORD_BITS] |= 1L << (slot % BITMAP_WORD_BITS);
    }

    private void unindexDeadline(Entry<?> entry) {
        int slot = entry.deadlineSlot;
        Entry<?> previous = entry.deadlinePrevious;
        if (previous == null && deadlineHeads[slot] != entry) return;
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

    CompletionStage<Integer> expire(long nowNanos) {
        return submitOwnerTurn(() -> expireOnOwner(nowNanos));
    }

    private int expireOnOwner(long nowNanos) {
        Entry<?> expiredHead = null;
        Entry<?> expiredTail = null;
        int expired = 0;
        long startedNanos = nanoTime.getAsLong();
        long nowTick = (nowNanos - deadlineOrigin) / DEADLINE_TICK_NANOS;
        if (deadlineSweepEndTick < 0) {
            maintenanceVisitedTick = nowTick;
            deadlineTick = nowTick;
            deadlineSweepEndTick = nowTick + DEADLINE_SLOTS;
            deadlineNextVisitTick = nowTick + DEADLINE_SLOTS;
        }
        while (nanoTime.getAsLong() - startedNanos
                < ZLinkReceiveBatchBudget.DEFAULT_MAX_ELAPSED_NANOS) {
            if (deadlineCursor == null) {
                long next = nextOccupiedTick();
                if (next < 0 || next >= deadlineSweepEndTick) {
                    deadlineSweepEndTick = -1;
                    if (next >= 0 && nowTick - maintenanceVisitedTick >= DEADLINE_SLOTS) {
                        maintenanceVisitedTick = nowTick;
                        deadlineTick = nowTick;
                        deadlineSweepEndTick = nowTick + DEADLINE_SLOTS;
                        deadlineNextVisitTick = nowTick + DEADLINE_SLOTS;
                        continue;
                    }
                    break;
                }
                deadlineTick = next;
                deadlineCursor = deadlineHeads[(int) (next & (DEADLINE_SLOTS - 1))];
            }
            Entry<?> current = deadlineCursor;
            if (current.deadlineTick > nowTick && pending.get(current.id) == current) {
                deadlineNextVisitTick = Math.min(deadlineNextVisitTick, current.deadlineTick);
                deadlineCursor = current.deadlineNext;
                if (deadlineCursor == null) deadlineTick++;
                continue;
            }
            unindexDeadline(current);
            // E5 already removed completed entries; wheel cleanup does not claim them again.
            if (take(current) == null) continue;
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

    private long nextMaintenanceTick() {
        if (nextOccupiedTick() < 0) return -1;
        // The cursor also reduces the next deadline while visiting, within the same budget.
        return deadlineSweepEndTick >= 0 ? maintenanceVisitedTick : deadlineNextVisitTick;
    }

    private void reserveMaintenanceVisit(Entry<?> entry) {
        if (deadlineSweepEndTick >= 0)
            deadlineNextVisitTick = Math.min(deadlineNextVisitTick, entry.deadlineTick);
        // Keep the booked visit even if completion has emptied the index. Only that visit
        // decides whether to stop; a later deadline cannot move it or needs another clock read.
        if (maintenanceTick == VISIT_IN_FLIGHT
                || maintenanceTick >= 0 && entry.deadlineTick >= maintenanceTick) return;
        long nowTick = (nanoTime.getAsLong() - deadlineOrigin) / DEADLINE_TICK_NANOS;
        if (maintenanceTick < 0) maintenanceVisitedTick = nowTick;
        long horizon = maintenanceVisitedTick + DEADLINE_SLOTS;
        long next = Math.min(entry.deadlineTick, horizon);
        if (maintenanceTick < 0 || next < maintenanceTick) {
            maintenanceTick = next;
            LockSupport.unpark(maintenance);
        }
    }

    private void maintainDeadlines() {
        while (!closed) {
            long tick = maintenanceTick;
            if (tick < 0) LockSupport.park(this);
            else {
                long remaining =
                        tick * DEADLINE_TICK_NANOS - (nanoTime.getAsLong() - deadlineOrigin);
                if (remaining > 0) LockSupport.parkNanos(this, remaining);
                else {
                    // Registration wakes must not submit another visit while this one is queued.
                    maintenanceTick = VISIT_IN_FLIGHT;
                    if (!owner.tryPost(
                            () -> {
                                if (!closed) {
                                    long now = nanoTime.getAsLong();
                                    expireOnOwner(now);
                                    maintenanceTick = nextMaintenanceTick();
                                }
                                LockSupport.unpark(maintenance);
                                return CompletableFuture.completedFuture(null);
                            })) return;
                }
            }
            if (Thread.interrupted()) return;
        }
    }

    private <T> Operation<T> registerOnOwner(long deadlineNanos, UUID suppliedId, Kind kind) {
        UUID id = suppliedId == null ? ZLinkServiceOperationIds.next() : suppliedId;
        Operation<T> operation = new Operation<>(kind == Kind.REQUEST, id);
        operation.beginRegistration();
        Entry<T> entry = registerEntry(deadlineNanos, id, kind, operation, null);
        if (!operation.finishRegistration(this)) abandonRegistration(entry);
        return operation;
    }

    private <T> Entry<T> registerEntry(
            long deadlineNanos,
            UUID id,
            Kind kind,
            CompletableFuture<T> completion,
            Consumer<? super T> discardValue) {
        if (closed) {
            throw new IllegalStateException("operation registry is closed");
        }
        if ((id.getMostSignificantBits() == 0 && id.getLeastSignificantBits() == 0)
                || pending.containsKey(id)) {
            throw new IllegalArgumentException("operation identity is zero or already pending");
        }
        long targetTick = 0;
        if (kind == Kind.REQUEST) {
            if (maintenance == null) {
                Thread worker = maintenanceFactory.newThread(this::maintainDeadlines);
                worker.start();
                maintenance = worker;
            }
            long now = nanoTime.getAsLong();
            long elapsed = now - deadlineOrigin;
            if (nextOccupiedTick() < 0)
                deadlineTick = Math.max(deadlineTick, elapsed / DEADLINE_TICK_NANOS);
            long remaining = Math.max(0L, deadlineNanos - now);
            targetTick =
                    elapsed / DEADLINE_TICK_NANOS
                            + remaining / DEADLINE_TICK_NANOS
                            + (elapsed % DEADLINE_TICK_NANOS
                                            + remaining % DEADLINE_TICK_NANOS
                                            + DEADLINE_TICK_NANOS
                                            - 1)
                                    / DEADLINE_TICK_NANOS;
        }
        // Each registration keeps its identity while a stale deadline still references it.
        Entry<T> entry = new Entry<>(id, completion, discardValue);
        entry.deadlineNanos = deadlineNanos;
        completions.register(entry);
        pending.put(id, entry);
        if (kind == Kind.REQUEST) {
            indexDeadline(entry, targetTick);
            reserveMaintenanceVisit(entry);
        }
        return entry;
    }

    /** Prepares the final receiver; owner registration accepts it before physical submission. */
    public static <T> Operation<T> prepareOperation(UUID id) {
        return new Operation<>(true, Objects.requireNonNull(id, "id"));
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

    public static final class Operation<T> extends CompletableFuture<T> {
        private static final Object UNREGISTERED = new Object();
        private static final Object REGISTERING = new Object();
        private static final VarHandle OWNER;

        static {
            try {
                OWNER =
                        MethodHandles.lookup()
                                .findVarHandle(Operation.class, "owner", Object.class);
            } catch (ReflectiveOperationException failure) {
                throw new ExceptionInInitializerError(failure);
            }
        }

        private volatile Object owner;
        private final UUID id;

        private Operation(boolean cancellable, UUID id) {
            this.owner = cancellable ? UNREGISTERED : null;
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

        boolean beginRegistration() {
            return OWNER.compareAndSet(this, UNREGISTERED, REGISTERING);
        }

        boolean finishRegistration(ZLinkServiceOperationRegistry registry) {
            if (owner == registry) return true;
            return OWNER.compareAndSet(this, REGISTERING, registry) || owner == registry;
        }

        boolean rejectRegistration(ZLinkServiceOperationRegistry registry) {
            while (true) {
                Object current = owner;
                if (current == registry) return true;
                if (current != UNREGISTERED && current != REGISTERING) return false;
                if (OWNER.compareAndSet(this, current, registry)) return true;
            }
        }

        @Override
        public boolean cancel(boolean mayInterruptIfRunning) {
            while (true) {
                Object current = owner;
                if (current instanceof ZLinkServiceOperationRegistry registry)
                    return registry.cancel(id, this);
                if (current != UNREGISTERED && current != REGISTERING) return false;
                if (OWNER.compareAndSet(this, current, null)) return super.cancel(false);
            }
        }

        void completeCancellation() {
            super.cancel(false);
        }
    }

    private enum Kind {
        REQUEST,
        SEND
    }

    static final class Entry<T> extends ZLinkServiceCompletionDispatcher.WorkItem {
        private static final int NONE = 0;
        private static final int VALUE = 1;
        private static final int FAILURE = 2;
        private static final int CANCELLATION = 3;

        private CompletableFuture<T> completion;
        private Consumer<? super T> discardValue;
        private final UUID id;
        private long deadlineTick;
        private long deadlineNanos;
        private int deadlineSlot;
        private Entry<?> deadlinePrevious;
        private Entry<?> deadlineNext;
        private int terminalKind = NONE;
        private Object terminalValue;
        private Throwable terminalFailure;

        Entry(UUID id, CompletableFuture<T> completion, Consumer<? super T> discardValue) {
            this.discardValue = discardValue;
            this.id = id;
            this.completion = completion;
        }

        void clearCancellation() {
            if (completion instanceof Operation<?> operation) operation.clearCancellation();
        }

        @Override
        void released() {
            completion = null;
            discardValue = null;
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
                    case VALUE -> {
                        if (!completion.complete((T) terminalValue)
                                && terminalValue != null
                                && discardValue != null) discardValue.accept((T) terminalValue);
                    }
                    case FAILURE -> completion.completeExceptionally(terminalFailure);
                    case CANCELLATION -> {
                        if (completion instanceof Operation<?> operation)
                            operation.completeCancellation();
                        else completion.cancel(false);
                    }
                    default ->
                            throw new IllegalStateException(
                                    "completion work item has no terminal outcome");
                }
            } finally {
                clearCancellation();
                terminalValue = null;
                terminalFailure = null;
            }
        }

        void dispatchNext(Entry<?> value) {
            setDispatchNext(value);
        }
    }
}
