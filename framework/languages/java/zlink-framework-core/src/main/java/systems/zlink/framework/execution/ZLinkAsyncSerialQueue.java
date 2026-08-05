package systems.zlink.framework.execution;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executor;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.time.Duration;
import java.util.function.Supplier;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;

public final class ZLinkAsyncSerialQueue {
    public static final int DEFAULT_APPLICATION_MESSAGE_CAPACITY = 1024;
    public static final int DEFAULT_LIFECYCLE_MESSAGE_CAPACITY = 128;
    public static final long DEFAULT_APPLICATION_BYTE_CAPACITY = 64L * 1024 * 1024;
    public static final long DEFAULT_LIFECYCLE_BYTE_CAPACITY = 4L * 1024 * 1024;
    public static final long DEFAULT_FIXED_WORK_BYTE_COST = 256;
    public static final int DEFAULT_LIFECYCLE_BURST_LIMIT = 8;
    public static final Duration DEFAULT_OWNER_TIME_BUDGET = Duration.ofMillis(10);
    private static final ThreadLocal<ZLinkAsyncSerialQueue> CURRENT = new ThreadLocal<>();
    private static final ThreadLocal<CompletableFuture<Void>> CURRENT_GATE = new ThreadLocal<>();
    private static final ThreadLocal<Boolean> CURRENT_RELEASE_DEFERRED = new ThreadLocal<>();

    private final Executor executor;
    private final ExecutorService ownedExecutor;
    private final boolean releaseOnIncompleteStage;
    private final int applicationMessageCapacity;
    private final long applicationByteCapacity;
    private final int lifecycleMessageCapacity;
    private final long lifecycleByteCapacity;
    private final long fixedWorkByteCost;
    private final int lifecycleBurstLimit;
    private final long ownerTimeBudgetNanos;
    private final ArrayDeque<Entry> applicationPending = new ArrayDeque<>();
    // A resumed application turn is kept separate so it can pass a relocation
    // boundary without inserting at the front of the application FIFO.
    private final ArrayDeque<Entry> continuationPending = new ArrayDeque<>();
    private final ArrayDeque<Entry> lifecyclePending = new ArrayDeque<>();
    private int applicationMessages;
    private int lifecycleMessages;
    private long applicationBytes;
    private long lifecycleBytes;
    private int lifecycleStreak;
    private int outstanding;
    private long nextSequence = 1L;
    private long nextRelocationSerial = 1L;
    private Entry active;
    private int suspendedContinuations;
    private long turnClaimedAtNanos;
    private RelocationState relocation;
    private boolean relocated;
    private Runnable capacityAvailable = () -> { };
    private final List<CompletableFuture<Void>> quiescenceWaiters =
        new ArrayList<>();

    public ZLinkAsyncSerialQueue() {
        this(false);
    }

    public ZLinkAsyncSerialQueue(boolean releaseOnIncompleteStage) {
        this(
            null,
            releaseOnIncompleteStage,
            DEFAULT_APPLICATION_MESSAGE_CAPACITY,
            DEFAULT_APPLICATION_BYTE_CAPACITY,
            DEFAULT_LIFECYCLE_MESSAGE_CAPACITY,
            DEFAULT_LIFECYCLE_BYTE_CAPACITY,
            DEFAULT_FIXED_WORK_BYTE_COST,
            DEFAULT_LIFECYCLE_BURST_LIMIT,
            DEFAULT_OWNER_TIME_BUDGET);
    }

    public ZLinkAsyncSerialQueue(
        Executor executor,
        boolean releaseOnIncompleteStage) {
        this(
            executor,
            releaseOnIncompleteStage,
            DEFAULT_APPLICATION_MESSAGE_CAPACITY,
            DEFAULT_APPLICATION_BYTE_CAPACITY,
            DEFAULT_LIFECYCLE_MESSAGE_CAPACITY,
            DEFAULT_LIFECYCLE_BYTE_CAPACITY,
            DEFAULT_FIXED_WORK_BYTE_COST,
            DEFAULT_LIFECYCLE_BURST_LIMIT,
            DEFAULT_OWNER_TIME_BUDGET);
    }

    public ZLinkAsyncSerialQueue(boolean releaseOnIncompleteStage, int pendingCapacity) {
        this(
            null,
            releaseOnIncompleteStage,
            pendingCapacity,
            DEFAULT_APPLICATION_BYTE_CAPACITY,
            pendingCapacity,
            DEFAULT_LIFECYCLE_BYTE_CAPACITY,
            DEFAULT_FIXED_WORK_BYTE_COST,
            DEFAULT_LIFECYCLE_BURST_LIMIT,
            DEFAULT_OWNER_TIME_BUDGET);
    }

    public ZLinkAsyncSerialQueue(
        Executor executor,
        boolean releaseOnIncompleteStage,
        int pendingCapacity) {
        this(
            executor,
            releaseOnIncompleteStage,
            pendingCapacity,
            DEFAULT_APPLICATION_BYTE_CAPACITY,
            pendingCapacity,
            DEFAULT_LIFECYCLE_BYTE_CAPACITY,
            DEFAULT_FIXED_WORK_BYTE_COST,
            DEFAULT_LIFECYCLE_BURST_LIMIT,
            DEFAULT_OWNER_TIME_BUDGET);
    }

    public ZLinkAsyncSerialQueue(
        Executor executor,
        boolean releaseOnIncompleteStage,
        int applicationMessageCapacity,
        long applicationByteCapacity,
        int lifecycleMessageCapacity,
        long lifecycleByteCapacity,
        long fixedWorkByteCost,
        int lifecycleBurstLimit,
        Duration ownerTimeBudget) {
        if (applicationMessageCapacity <= 0
            || lifecycleMessageCapacity <= 0
            || applicationByteCapacity <= 0
            || lifecycleByteCapacity <= 0
            || fixedWorkByteCost <= 0
            || lifecycleBurstLimit <= 0
            || ownerTimeBudget == null
            || ownerTimeBudget.isNegative()
            || ownerTimeBudget.isZero()) {
            throw new IllegalArgumentException("serial queue limits are invalid");
        }
        if (executor == null) {
            this.ownedExecutor = Executors.newVirtualThreadPerTaskExecutor();
            this.executor = ownedExecutor;
        } else {
            this.ownedExecutor = null;
            this.executor = executor;
        }
        this.releaseOnIncompleteStage = releaseOnIncompleteStage;
        this.applicationMessageCapacity = applicationMessageCapacity;
        this.applicationByteCapacity = applicationByteCapacity;
        this.lifecycleMessageCapacity = lifecycleMessageCapacity;
        this.lifecycleByteCapacity = lifecycleByteCapacity;
        this.fixedWorkByteCost = fixedWorkByteCost;
        this.lifecycleBurstLimit = lifecycleBurstLimit;
        this.ownerTimeBudgetNanos = ownerTimeBudget.toNanos();
    }

    public void close() {
        if (ownedExecutor != null) {
            ownedExecutor.shutdown();
        }
    }

    public synchronized CompletionStage<Void> enqueue(Supplier<CompletionStage<Void>> operation) {
        return enqueueAccepted(null, 0, operation);
    }

    /**
     * Enqueues an application turn and charges its known payload length in the
     * application byte budget. The queue also charges the fixed per-turn cost;
     * callers must pass the payload length before deserializing the payload.
     */
    public synchronized CompletionStage<Void> enqueueWithPayloadBytes(
        long payloadBytes,
        Supplier<CompletionStage<Void>> operation) {
        validatePayloadBytes(payloadBytes);
        return enqueueAccepted(null, payloadBytes, operation);
    }

    /**
     * Returns whether this queue currently owns the calling thread's serial turn.
     * Internal dispatch composition uses this to avoid waiting on a turn that
     * was enqueued behind the operation currently executing on this queue.
     */
    public boolean isCurrent() {
        return CURRENT.get() == this;
    }

    /**
     * Internal lifecycle barrier that runs immediately after the active turn
     * and before previously queued application turns.
     */
    public synchronized CompletionStage<Void> enqueueBarrierNext(
        Supplier<CompletionStage<Void>> operation) {
        java.util.Objects.requireNonNull(operation, "operation");
        if (relocated) {
            return CompletableFuture.failedFuture(
                new IllegalStateException("queue owner has relocated"));
        }
        if (nextSequence == Long.MAX_VALUE) {
            throw new IllegalStateException("queue sequence exhausted");
        }
        if (!canReserve(Lane.LIFECYCLE, fixedWorkByteCost)) {
            return capacityFailure("lifecycle barrier queue is full");
        }
        reserve(Lane.LIFECYCLE, fixedWorkByteCost);
        Entry entry = new Entry(
            nextSequence++,
            null,
            operation,
            () -> { },
            new CompletableFuture<>(),
            ZLinkFlowContext.current(),
            null,
            Lane.LIFECYCLE,
            fixedWorkByteCost,
            false);
        if (relocation != null) {
            relocation.held.addLast(entry);
        } else {
            lifecyclePending.addLast(entry);
            startNext();
        }
        return entry.result;
    }

    /**
     * Internal lifecycle barrier that runs after every application turn
     * accepted before this call. Unlike application admission, this barrier
     * remains available after relocation has committed so the old owner can
     * release local resources.
     */
    public synchronized CompletionStage<Void> enqueueLifecycleBarrier(
        Supplier<CompletionStage<Void>> operation) {
        java.util.Objects.requireNonNull(operation, "operation");
        if (relocation != null) {
            return CompletableFuture.failedFuture(
                new IllegalStateException("queue relocation is in progress"));
        }
        if (nextSequence == Long.MAX_VALUE) {
            throw new IllegalStateException("queue sequence exhausted");
        }
        if (!canReserve(Lane.LIFECYCLE, fixedWorkByteCost)) {
            return capacityFailure("lifecycle barrier queue is full");
        }
        reserve(Lane.LIFECYCLE, fixedWorkByteCost);
        Entry entry = new Entry(
            nextSequence++,
            null,
            operation,
            () -> { },
            new CompletableFuture<>(),
            ZLinkFlowContext.current(),
            null,
            Lane.LIFECYCLE,
            fixedWorkByteCost,
            false);
        lifecyclePending.addLast(entry);
        startNext();
        return entry.result;
    }

    public synchronized CompletionStage<Void> enqueueRelocatable(
        byte[] record,
        Supplier<CompletionStage<Void>> operation) {
        return enqueueRelocatable(record, operation, () -> { });
    }

    public synchronized CompletionStage<Void> enqueueRelocatable(
        byte[] record,
        Supplier<CompletionStage<Void>> operation,
        Runnable relocationRelease) {
        java.util.Objects.requireNonNull(record, "record");
        java.util.Objects.requireNonNull(relocationRelease, "relocationRelease");
        if (relocated) {
            return CompletableFuture.failedFuture(
                new IllegalStateException("queue owner has relocated"));
        }
        return enqueueAccepted(record.clone(), record.length, operation, relocationRelease);
    }

    public synchronized boolean tryEnqueue(Supplier<CompletionStage<Void>> operation) {
        if (relocated || relocation != null && relocation.frozen
            || !canReserve(Lane.APPLICATION, fixedWorkByteCost)) {
            return false;
        }
        enqueueAccepted(null, 0, operation);
        return true;
    }

    /**
     * Attempts to enqueue an application turn with a known payload length.
     * The length participates in the same byte admission as the fixed turn
     * cost and is released when the turn reaches its terminal boundary.
     */
    public synchronized boolean tryEnqueueWithPayloadBytes(
        long payloadBytes,
        Supplier<CompletionStage<Void>> operation) {
        validatePayloadBytes(payloadBytes);
        if (relocated || relocation != null && relocation.frozen
            || !canRepresentWorkByteCost(payloadBytes)
            || !canReserve(Lane.APPLICATION, workByteCost(payloadBytes))) {
            return false;
        }
        enqueueAccepted(null, payloadBytes, operation);
        return true;
    }

    public synchronized boolean tryEnqueueRelocatable(
        byte[] record,
        Supplier<CompletionStage<Void>> operation) {
        java.util.Objects.requireNonNull(record, "record");
        if (relocated || relocation != null && relocation.frozen
            || !canRepresentWorkByteCost(record.length)
            || !canReserve(Lane.APPLICATION, workByteCost(record.length))) {
            return false;
        }
        enqueueAccepted(record.clone(), record.length, operation);
        return true;
    }

    public synchronized void onCapacityAvailable(Runnable callback) {
        capacityAvailable = java.util.Objects.requireNonNull(callback, "callback");
    }

    private CompletionStage<Void> enqueueAccepted(
        byte[] record,
        Supplier<CompletionStage<Void>> operation) {
        return enqueueAccepted(record, record == null ? 0 : record.length, operation, () -> { });
    }

    private CompletionStage<Void> enqueueAccepted(
        byte[] record,
        long payloadBytes,
        Supplier<CompletionStage<Void>> operation) {
        return enqueueAccepted(record, payloadBytes, operation, () -> { });
    }

    private CompletionStage<Void> enqueueAccepted(
        byte[] record,
        long payloadBytes,
        Supplier<CompletionStage<Void>> operation,
        Runnable relocationRelease) {
        java.util.Objects.requireNonNull(operation, "operation");
        validatePayloadBytes(payloadBytes);
        if (relocation != null && relocation.frozen) {
            return CompletableFuture.failedFuture(
                new IllegalStateException(
                    "queue relocation ingress is frozen"));
        }
        if (nextSequence == Long.MAX_VALUE) {
            throw new IllegalStateException("queue sequence exhausted");
        }
        if (!canRepresentWorkByteCost(payloadBytes)) {
            return capacityFailure("application payload exceeds queue byte capacity");
        }
        long byteCost = workByteCost(payloadBytes);
        if (!canReserve(Lane.APPLICATION, byteCost)) {
            return capacityFailure("application queue is full");
        }
        reserve(Lane.APPLICATION, byteCost);
        Entry entry = new Entry(
            nextSequence++,
            record,
            operation,
            relocationRelease,
            new CompletableFuture<>(),
            ZLinkFlowContext.current(),
            null,
            Lane.APPLICATION,
            byteCost,
            false);
        if (relocation != null) {
            relocation.held.addLast(entry);
        } else {
            applicationPending.addLast(entry);
            startNext();
        }
        return entry.result;
    }

    private boolean canReserve(Lane lane, long byteCost) {
        if (byteCost <= 0) {
            return false;
        }
        if (lane == Lane.APPLICATION) {
            return applicationMessages < applicationMessageCapacity
                && byteCost <= applicationByteCapacity - applicationBytes;
        }
        return lifecycleMessages < lifecycleMessageCapacity
            && byteCost <= lifecycleByteCapacity - lifecycleBytes;
    }

    private void reserve(Lane lane, long byteCost) {
        if (!canReserve(lane, byteCost)) {
            throw new IllegalStateException("serial queue reservation is unavailable");
        }
        if (lane == Lane.APPLICATION) {
            applicationMessages++;
            applicationBytes = Math.addExact(applicationBytes, byteCost);
        } else {
            lifecycleMessages++;
            lifecycleBytes = Math.addExact(lifecycleBytes, byteCost);
        }
        outstanding++;
    }

    private void release(Entry entry) {
        if (entry.lane == Lane.APPLICATION) {
            applicationMessages--;
            applicationBytes -= entry.byteCost;
        } else {
            lifecycleMessages--;
            lifecycleBytes -= entry.byteCost;
        }
        outstanding--;
    }

    private long workByteCost(int payloadLength) {
        return workByteCost((long) payloadLength);
    }

    private long workByteCost(long payloadBytes) {
        validatePayloadBytes(payloadBytes);
        return Math.addExact(fixedWorkByteCost, payloadBytes);
    }

    private boolean canRepresentWorkByteCost(long payloadBytes) {
        return payloadBytes <= Long.MAX_VALUE - fixedWorkByteCost;
    }

    private static void validatePayloadBytes(long payloadBytes) {
        if (payloadBytes < 0) {
            throw new IllegalArgumentException("payloadBytes must be non-negative");
        }
    }

    private CompletionStage<Void> capacityFailure(String message) {
        return CompletableFuture.failedFuture(
            new systems.zlink.framework.errors.ZLinkFrameworkException(
                systems.zlink.framework.errors.ZLinkFrameworkErrorKind.CAPACITY_EXCEEDED,
                message));
    }

    private boolean hasPending() {
        return !applicationPending.isEmpty()
            || !continuationPending.isEmpty()
            || !lifecyclePending.isEmpty();
    }

    private Entry takeNext() {
        boolean lifecycleReady = !lifecyclePending.isEmpty();
        boolean applicationReady = !applicationPending.isEmpty();
        boolean continuationReady = !continuationPending.isEmpty();
        if (lifecycleReady && lifecyclePending.peekFirst().relocationBoundary != null) {
            if (continuationReady) {
                lifecycleStreak = 0;
                return continuationPending.removeFirst();
            }
            return lifecyclePending.removeFirst();
        }
        if (lifecycleReady
            && (!applicationReady || lifecycleStreak < lifecycleBurstLimit)) {
            lifecycleStreak++;
            return lifecyclePending.removeFirst();
        }
        if (continuationReady) {
            lifecycleStreak = 0;
            return continuationPending.removeFirst();
        }
        if (applicationReady) {
            lifecycleStreak = 0;
            return applicationPending.removeFirst();
        }
        lifecycleStreak = 0;
        return lifecyclePending.removeFirst();
    }

    private void startNext() {
        if (active != null || !hasPending()) {
            return;
        }
        if (!lifecyclePending.isEmpty()
            && lifecyclePending.peekFirst().relocationBoundary != null
            && suspendedContinuations != 0
            && continuationPending.isEmpty()) {
            return;
        }
        Entry entry = takeNext();
        active = entry;
        if (turnClaimedAtNanos == 0) {
            turnClaimedAtNanos = System.nanoTime();
        }
        CompletionStage<Void> gate = invoke(
            entry.operation,
            entry.result,
            entry.flow);
        gate.whenComplete((ignored, error) -> finish(entry));
    }

    private void finish(Entry entry) {
        Runnable notify = null;
        List<CompletableFuture<Void>> quiescent = List.of();
        RelocationBoundary boundary = entry.relocationBoundary;
        boolean yieldToExecutor = false;
        synchronized (this) {
            if (active != entry) {
                return;
            }
            active = null;
            boolean wasApplicationFull = !canReserve(
                Lane.APPLICATION,
                fixedWorkByteCost);
            release(entry);
            if (wasApplicationFull
                && canReserve(Lane.APPLICATION, fixedWorkByteCost)) {
                notify = capacityAvailable;
            }
            yieldToExecutor = hasPending()
                && ownerTimeBudgetNanos > 0
                && System.nanoTime() - turnClaimedAtNanos >= ownerTimeBudgetNanos;
            if (yieldToExecutor) {
                turnClaimedAtNanos = 0;
            } else if (!hasPending()) {
                turnClaimedAtNanos = 0;
            } else {
                startNext();
            }
            quiescent = takeQuiescenceWaitersIfReady();
        }
        if (notify != null) {
            executor.execute(notify);
        }
        if (yieldToExecutor) {
            executor.execute(this::startNextAsync);
        }
        if (boundary != null) {
            boundary.finished.complete(null);
        }
        quiescent.forEach(waiter -> waiter.complete(null));
    }

    private void startNextAsync() {
        synchronized (this) {
            startNext();
        }
    }

    /**
     * Completes after every accepted turn and every yielded continuation has
     * reached its terminal boundary. The caller must seal external admission
     * before using this as a lifecycle barrier.
     */
    public synchronized CompletionStage<Void> awaitQuiescence() {
        if (isQuiescent()) {
            return CompletableFuture.completedFuture(null);
        }
        CompletableFuture<Void> waiter = new CompletableFuture<>();
        quiescenceWaiters.add(waiter);
        return waiter;
    }

    public synchronized Optional<RelocationSeal> trySealRelocation() {
        return trySealRelocation(null);
    }

    /**
     * Reserves the next turn boundary without running queued application
     * records. The reservation remains active until {@link
     * RelocationBoundary#release()} is called.
     */
    public synchronized Optional<RelocationBoundary>
        reserveRelocationTurnBoundary() {
        if (relocated || relocation != null) {
            return Optional.empty();
        }
        if (nextSequence == Long.MAX_VALUE) {
            throw new IllegalStateException("queue sequence exhausted");
        }
        if (!canReserve(Lane.LIFECYCLE, fixedWorkByteCost)) {
            return Optional.empty();
        }
        RelocationBoundary boundary = new RelocationBoundary(this);
        Entry entry = new Entry(
            nextSequence++,
            null,
            boundary::reach,
            () -> { },
            new CompletableFuture<>(),
            ZLinkFlowContext.current(),
            boundary,
            Lane.LIFECYCLE,
            fixedWorkByteCost,
            false);
        boundary.entry = entry;
        reserve(Lane.LIFECYCLE, fixedWorkByteCost);
        lifecyclePending.addLast(entry);
        startNext();
        return Optional.of(boundary);
    }

    /**
     * Seals this queue while the supplied lifecycle boundary owns its active
     * turn. Only the exact reservation instance can cross that boundary.
     */
    public synchronized Optional<RelocationSeal> trySealRelocation(
        RelocationBoundary boundary) {
        if (relocated || relocation != null) {
            return Optional.empty();
        }
        if (boundary != null
            && (boundary.owner != this
                || boundary.entry != active
                || !boundary.reached.isDone()
                || boundary.released.isDone())) {
            return Optional.empty();
        }
        if (active != null) {
            if (boundary == null) {
                if (CURRENT.get() != this) {
                    return Optional.empty();
                }
            }
        }
        if (suspendedContinuations != 0
            || !continuationPending.isEmpty()
            || applicationPending.stream().anyMatch(entry -> entry.record == null)) {
            return Optional.empty();
        }
        if (nextRelocationSerial == Long.MAX_VALUE) {
            throw new IllegalStateException("relocation serial exhausted");
        }
        ArrayDeque<Entry> captured = new ArrayDeque<>(applicationPending);
        applicationPending.clear();
        long serial = nextRelocationSerial++;
        RelocationSeal seal = new RelocationSeal(
            serial,
            captured.stream().map(Entry::queuedRecord).toList());
        relocation = new RelocationState(serial, seal, captured);
        return Optional.of(seal);
    }

    public synchronized boolean abortRelocation(RelocationSeal seal) {
        if (!matches(seal)) {
            return false;
        }
        ArrayDeque<Entry> restored = new ArrayDeque<>(relocation.captured);
        while (!restored.isEmpty()) {
            applicationPending.addLast(restored.removeFirst());
        }
        relocation.held.forEach(entry -> {
            if (entry.lane == Lane.LIFECYCLE) {
                lifecyclePending.addLast(entry);
            } else if (entry.continuation) {
                continuationPending.addLast(entry);
            } else {
                applicationPending.addLast(entry);
            }
        });
        relocation = null;
        startNext();
        return true;
    }

    /** Freezes the bounded ingress high-water before authority prepare. */
    public synchronized Optional<List<QueuedRecord>>
        freezeRelocationIngress(RelocationSeal seal) {
        if (!matches(seal) || relocation.frozen) {
            return Optional.empty();
        }
        relocation.frozen = true;
        return Optional.of(relocation.held.stream()
            .filter(entry -> entry.record != null)
            .map(Entry::queuedRecord)
            .toList());
    }

    public synchronized Optional<List<QueuedRecord>> commitRelocation(
        RelocationSeal seal) {
        if (!matches(seal)) {
            return Optional.empty();
        }
        List<QueuedRecord> held = relocation.held.stream()
            .filter(entry -> entry.record != null)
            .map(Entry::queuedRecord)
            .toList();
        List<Entry> released = new ArrayList<>(
            relocation.captured.size() + relocation.held.size());
        released.addAll(relocation.captured);
        released.addAll(relocation.held);
        relocation = null;
        relocated = true;
        for (Entry entry : released) {
            try {
                entry.relocationRelease.run();
                entry.result.complete(null);
            } catch (RuntimeException failure) {
                entry.result.completeExceptionally(failure);
            } finally {
                releaseRelocatedCapacity(entry);
            }
        }
        completeQuiescenceWaitersIfReady();
        return Optional.of(held);
    }

    private boolean matches(RelocationSeal seal) {
        return seal != null
            && relocation != null
            && relocation.serial == seal.serial
            && relocation.seal == seal;
    }

    private void releaseRelocatedCapacity(Entry entry) {
        boolean wasFull = !canReserve(Lane.APPLICATION, fixedWorkByteCost);
        release(entry);
        if (wasFull && canReserve(Lane.APPLICATION, fixedWorkByteCost)) {
            executor.execute(capacityAvailable);
        }
    }

    private CompletionStage<Void> invoke(
        Supplier<CompletionStage<Void>> operation,
        CompletableFuture<Void> result,
        ZLinkFlowContext.State flow) {
        CompletableFuture<Void> gate = new CompletableFuture<>();
        executor.execute(() -> {
            ZLinkAsyncSerialQueue previous = CURRENT.get();
            CompletableFuture<Void> previousGate = CURRENT_GATE.get();
            Boolean previousDeferred = CURRENT_RELEASE_DEFERRED.get();
            CURRENT.set(this);
            CURRENT_GATE.set(gate);
            CURRENT_RELEASE_DEFERRED.set(false);
            try (var serial = systems.zlink.framework.runtime.internal.handlers
                     .ZLinkSuspendInvocationContext.enterSerialExecutionTurn(
                         new SerialTurnCarrier(new SerialTurn(this, gate)));
                 ZLinkFlowContext.Scope ignored = flow == null
                     ? () -> { }
                     : ZLinkFlowContext.enter(flow)) {
                CompletionStage<Void> execution = java.util.Objects.requireNonNull(
                    operation.get(), "operation result");
                execution.whenComplete((value, error) -> {
                    if (error != null) {
                        result.completeExceptionally(error);
                    } else {
                        result.complete(null);
                    }
                    if (!releaseOnIncompleteStage) {
                        gate.complete(null);
                    }
                });
                if (releaseOnIncompleteStage
                    && !Boolean.TRUE.equals(CURRENT_RELEASE_DEFERRED.get())) {
                    gate.complete(null);
                }
            } catch (RuntimeException error) {
                result.completeExceptionally(error);
                gate.complete(null);
            } finally {
                if (previous == null) {
                    CURRENT.remove();
                } else {
                    CURRENT.set(previous);
                }
                if (previousGate == null) {
                    CURRENT_GATE.remove();
                } else {
                    CURRENT_GATE.set(previousGate);
                }
                if (previousDeferred == null) {
                    CURRENT_RELEASE_DEFERRED.remove();
                } else {
                    CURRENT_RELEASE_DEFERRED.set(previousDeferred);
                }
            }
        });
        return gate;
    }

    public static <T> CompletionStage<T> manageCurrent(CompletionStage<T> stage) {
        java.util.Objects.requireNonNull(stage, "stage");
        SerialTurn turn = currentTurn();
        ZLinkAsyncSerialQueue queue = turn == null ? null : turn.queue;
        if (queue == null) {
            return stage;
        }
        ZLinkFlowContext.State flow = ZLinkFlowContext.current();
        var application = systems.zlink.framework.runtime.internal.handlers
            .ZLinkSuspendInvocationContext.currentApplicationExecution();
        String actorDispatch = systems.zlink.framework.runtime.internal.handlers
            .ZLinkSuspendInvocationContext.currentActorDispatch();
        Object serialContext = systems.zlink.framework.runtime.internal.handlers
            .ZLinkSuspendInvocationContext.currentSerialExecutionTurn();
        CompletableFuture<T> managed = new CompletableFuture<>();
        managed.whenComplete((ignored, error) -> {
            if (managed.isCancelled()) {
                stage.toCompletableFuture().cancel(false);
            }
        });
        if (!queue.releaseOnIncompleteStage) {
            CompletableFuture<Void> gate = turn.gate;
            stage.whenComplete((value, error) -> queue.executor.execute(() -> {
                ZLinkAsyncSerialQueue previous = CURRENT.get();
                CompletableFuture<Void> previousGate = CURRENT_GATE.get();
                CURRENT.set(queue);
                if (gate == null) {
                    CURRENT_GATE.remove();
                } else {
                    CURRENT_GATE.set(gate);
                }
                try (var serial = systems.zlink.framework.runtime.internal.handlers
                         .ZLinkSuspendInvocationContext.enterSerialExecutionTurn(
                             serialContext);
                     var actor = systems.zlink.framework.runtime.internal.handlers
                         .ZLinkSuspendInvocationContext.enterActorDispatch(actorDispatch);
                     var execution = systems.zlink.framework.runtime.internal.handlers
                         .ZLinkSuspendInvocationContext.enterApplicationExecution(application);
                     ZLinkFlowContext.Scope ignored = flow == null
                         ? () -> { }
                         : ZLinkFlowContext.enter(flow)) {
                    if (error != null) {
                        managed.completeExceptionally(error);
                    } else {
                        managed.complete(value);
                    }
                } finally {
                    if (previous == null) {
                        CURRENT.remove();
                    } else {
                        CURRENT.set(previous);
                    }
                    if (previousGate == null) {
                        CURRENT_GATE.remove();
                    } else {
                        CURRENT_GATE.set(previousGate);
                    }
                }
            }));
            return managed;
        }
        queue.suspendContinuation();
        stage.whenComplete((value, error) -> {
            try {
                CompletionStage<Void> continuation = queue.enqueueContinuation(() -> {
                    updateCarrier(serialContext, currentTurn());
                    try (var serial = systems.zlink.framework.runtime.internal.handlers
                             .ZLinkSuspendInvocationContext.enterSerialExecutionTurn(
                                 serialContext);
                         var actor = systems.zlink.framework.runtime.internal.handlers
                             .ZLinkSuspendInvocationContext.enterActorDispatch(actorDispatch);
                         var execution = systems.zlink.framework.runtime.internal.handlers
                             .ZLinkSuspendInvocationContext.enterApplicationExecution(application);
                         ZLinkFlowContext.Scope ignored = flow == null
                             ? () -> { }
                             : ZLinkFlowContext.enter(flow)) {
                        if (error != null) {
                            managed.completeExceptionally(error);
                        } else {
                            managed.complete(value);
                        }
                    }
                    return CompletableFuture.completedFuture(null);
                });
                continuation.whenComplete((ignored, continuationFailure) -> {
                    if (continuationFailure != null) {
                        managed.completeExceptionally(continuationFailure);
                    }
                });
            } catch (RuntimeException continuationFailure) {
                managed.completeExceptionally(continuationFailure);
            }
        });
        return managed;
    }

    public static <T> CompletionStage<T> yieldCurrent(CompletionStage<T> stage) {
        java.util.Objects.requireNonNull(stage, "stage");
        SerialTurn turn = currentTurn();
        ZLinkAsyncSerialQueue queue = turn == null ? null : turn.queue;
        CompletableFuture<Void> gate = turn == null ? null : turn.gate;
        if (queue == null || gate == null || stage.toCompletableFuture().isDone()) {
            return stage;
        }
        ZLinkFlowContext.State flow = ZLinkFlowContext.current();
        var application = systems.zlink.framework.runtime.internal.handlers
            .ZLinkSuspendInvocationContext.currentApplicationExecution();
        String actorDispatch = systems.zlink.framework.runtime.internal.handlers
            .ZLinkSuspendInvocationContext.currentActorDispatch();
        Object serialContext = systems.zlink.framework.runtime.internal.handlers
            .ZLinkSuspendInvocationContext.currentSerialExecutionTurn();
        CompletableFuture<T> managed = new CompletableFuture<>();
        managed.whenComplete((ignored, error) -> {
            if (managed.isCancelled()) {
                stage.toCompletableFuture().cancel(false);
            }
        });
        queue.suspendContinuation();
        gate.complete(null);
        stage.whenComplete((value, error) -> {
            try {
                CompletionStage<Void> continuation = queue.enqueueContinuation(() -> {
                    updateCarrier(serialContext, currentTurn());
                    try (var serial = systems.zlink.framework.runtime.internal.handlers
                             .ZLinkSuspendInvocationContext.enterSerialExecutionTurn(
                                 serialContext);
                         var actor = systems.zlink.framework.runtime.internal.handlers
                             .ZLinkSuspendInvocationContext.enterActorDispatch(actorDispatch);
                         var execution = systems.zlink.framework.runtime.internal.handlers
                             .ZLinkSuspendInvocationContext.enterApplicationExecution(application);
                         ZLinkFlowContext.Scope ignored = flow == null
                             ? () -> { }
                             : ZLinkFlowContext.enter(flow)) {
                        if (error != null) {
                            managed.completeExceptionally(error);
                        } else {
                            managed.complete(value);
                        }
                    }
                    return CompletableFuture.completedFuture(null);
                });
                continuation.whenComplete((ignored, continuationFailure) -> {
                    if (continuationFailure != null) {
                        managed.completeExceptionally(continuationFailure);
                    }
                });
            } catch (RuntimeException continuationFailure) {
                managed.completeExceptionally(continuationFailure);
            }
        });
        return managed;
    }

    private synchronized void suspendContinuation() {
        if (suspendedContinuations == Integer.MAX_VALUE) {
            throw new IllegalStateException(
                "suspended continuation count exhausted");
        }
        suspendedContinuations++;
    }

    private synchronized CompletionStage<Void> enqueueContinuation(
        Supplier<CompletionStage<Void>> operation) {
        if (suspendedContinuations <= 0) {
            throw new IllegalStateException(
                "suspended continuation count is inconsistent");
        }
        suspendedContinuations--;
        if (nextSequence == Long.MAX_VALUE) {
            throw new IllegalStateException("queue sequence exhausted");
        }
        if (!canReserve(Lane.APPLICATION, fixedWorkByteCost)) {
            return capacityFailure("application continuation queue is full");
        }
        reserve(Lane.APPLICATION, fixedWorkByteCost);
        Entry continuation = new Entry(
            nextSequence++,
            null,
            operation,
            () -> { },
            new CompletableFuture<>(),
            ZLinkFlowContext.current(),
            null,
            Lane.APPLICATION,
            fixedWorkByteCost,
            true);
        if (relocation != null) {
            relocation.held.addLast(continuation);
        } else {
            continuationPending.addLast(continuation);
            startNext();
        }
        return continuation.result;
    }

    private boolean isQuiescent() {
        return outstanding == 0
            && suspendedContinuations == 0
            && active == null
            && !hasPending()
            && (relocation == null
                || (relocation.captured.isEmpty()
                    && relocation.held.isEmpty()));
    }

    private List<CompletableFuture<Void>> takeQuiescenceWaitersIfReady() {
        if (!isQuiescent() || quiescenceWaiters.isEmpty()) {
            return List.of();
        }
        List<CompletableFuture<Void>> ready =
            List.copyOf(quiescenceWaiters);
        quiescenceWaiters.clear();
        return ready;
    }

    private void completeQuiescenceWaitersIfReady() {
        List<CompletableFuture<Void>> ready =
            takeQuiescenceWaitersIfReady();
        ready.forEach(waiter -> waiter.complete(null));
    }

    public static Executor propagateCurrent(Executor executor) {
        java.util.Objects.requireNonNull(executor, "executor");
        return command -> {
            ZLinkAsyncSerialQueue queue = CURRENT.get();
            CompletableFuture<Void> gate = CURRENT_GATE.get();
            Boolean deferred = CURRENT_RELEASE_DEFERRED.get();
            Object serialTurn = systems.zlink.framework.runtime.internal.handlers
                .ZLinkSuspendInvocationContext.currentSerialExecutionTurn();
            var application = systems.zlink.framework.runtime.internal.handlers
                .ZLinkSuspendInvocationContext.currentApplicationExecution();
            String actorDispatch = systems.zlink.framework.runtime.internal.handlers
                .ZLinkSuspendInvocationContext.currentActorDispatch();
            executor.execute(() -> runWithContext(
                queue,
                gate,
                deferred,
                serialTurn,
                application,
                actorDispatch,
                command));
        };
    }

    private static void runWithContext(
        ZLinkAsyncSerialQueue queue,
        CompletableFuture<Void> gate,
        Boolean deferred,
        Object serialTurn,
        systems.zlink.framework.runtime.internal.handlers
            .ZLinkSuspendInvocationContext.ApplicationExecution application,
        String actorDispatch,
        Runnable command) {
        ZLinkAsyncSerialQueue previous = CURRENT.get();
        CompletableFuture<Void> previousGate = CURRENT_GATE.get();
        Boolean previousDeferred = CURRENT_RELEASE_DEFERRED.get();
        setOrRemove(CURRENT, queue);
        setOrRemove(CURRENT_GATE, gate);
        setOrRemove(CURRENT_RELEASE_DEFERRED, deferred);
        try (var serial = systems.zlink.framework.runtime.internal.handlers
                 .ZLinkSuspendInvocationContext.enterSerialExecutionTurn(serialTurn);
             var actor = systems.zlink.framework.runtime.internal.handlers
                 .ZLinkSuspendInvocationContext.enterActorDispatch(actorDispatch);
             var execution = systems.zlink.framework.runtime.internal.handlers
                 .ZLinkSuspendInvocationContext.enterApplicationExecution(application)) {
            command.run();
        } finally {
            setOrRemove(CURRENT, previous);
            setOrRemove(CURRENT_GATE, previousGate);
            setOrRemove(CURRENT_RELEASE_DEFERRED, previousDeferred);
        }
    }

    private static <T> void setOrRemove(ThreadLocal<T> local, T value) {
        if (value == null) {
            local.remove();
        } else {
            local.set(value);
        }
    }

    public static <T> CompletionStage<T> deferCurrentReleaseUntil(CompletionStage<T> entered) {
        java.util.Objects.requireNonNull(entered, "entered");
        SerialTurn turn = currentTurn();
        CompletableFuture<Void> gate = turn == null ? null : turn.gate;
        if (gate == null) {
            return entered;
        }
        CURRENT_RELEASE_DEFERRED.set(true);
        entered.whenComplete((ignored, error) -> gate.complete(null));
        return entered;
    }

    private static SerialTurn currentTurn() {
        ZLinkAsyncSerialQueue queue = CURRENT.get();
        CompletableFuture<Void> gate = CURRENT_GATE.get();
        if (queue != null && gate != null) {
            return new SerialTurn(queue, gate);
        }
        Object propagated = systems.zlink.framework.runtime.internal.handlers
            .ZLinkSuspendInvocationContext.currentSerialExecutionTurn();
        return propagated instanceof SerialTurnCarrier carrier
            ? carrier.turn
            : null;
    }

    private static void updateCarrier(Object context, SerialTurn turn) {
        if (context instanceof SerialTurnCarrier carrier && turn != null) {
            carrier.turn = turn;
        }
    }

    private enum Lane {
        APPLICATION,
        LIFECYCLE
    }

    private record SerialTurn(
        ZLinkAsyncSerialQueue queue,
        CompletableFuture<Void> gate) {
    }

    private static final class SerialTurnCarrier {
        private volatile SerialTurn turn;

        private SerialTurnCarrier(SerialTurn turn) {
            this.turn = turn;
        }
    }

    public record QueuedRecord(long sequence, byte[] payload) {
        public QueuedRecord {
            if (sequence <= 0) {
                throw new IllegalArgumentException(
                    "queue record sequence must be positive");
            }
            payload = java.util.Objects.requireNonNull(
                payload,
                "payload").clone();
        }

        @Override
        public byte[] payload() {
            return payload.clone();
        }
    }

    public record RelocationSeal(
        long serial,
        List<QueuedRecord> captured) {
        public RelocationSeal {
            if (serial <= 0) {
                throw new IllegalArgumentException(
                    "relocation seal serial must be positive");
            }
            captured = List.copyOf(captured);
        }
    }

    public static final class RelocationBoundary {
        private final ZLinkAsyncSerialQueue owner;
        private final CompletableFuture<Void> reached =
            new CompletableFuture<>();
        private final CompletableFuture<Void> released =
            new CompletableFuture<>();
        private final CompletableFuture<Void> finished =
            new CompletableFuture<>();
        private Entry entry;

        private RelocationBoundary(ZLinkAsyncSerialQueue owner) {
            this.owner = owner;
        }

        private CompletionStage<Void> reach() {
            reached.complete(null);
            return released;
        }

        public CompletionStage<Void> reached() {
            return reached;
        }

        public CompletionStage<Void> finished() {
            return finished;
        }

        public void release() {
            released.complete(null);
        }
    }

    private static final class Entry {
        private final long sequence;
        private final byte[] record;
        private final Supplier<CompletionStage<Void>> operation;
        private final Runnable relocationRelease;
        private final CompletableFuture<Void> result;
        private final ZLinkFlowContext.State flow;
        private final RelocationBoundary relocationBoundary;
        private final Lane lane;
        private final long byteCost;
        private final boolean continuation;

        private Entry(
            long sequence,
            byte[] record,
            Supplier<CompletionStage<Void>> operation,
            Runnable relocationRelease,
            CompletableFuture<Void> result,
            ZLinkFlowContext.State flow,
            RelocationBoundary relocationBoundary,
            Lane lane,
            long byteCost,
            boolean continuation) {
            this.sequence = sequence;
            this.record = record;
            this.operation = operation;
            this.relocationRelease = relocationRelease;
            this.result = result;
            this.flow = flow;
            this.relocationBoundary = relocationBoundary;
            this.lane = lane;
            this.byteCost = byteCost;
            this.continuation = continuation;
        }

        private QueuedRecord queuedRecord() {
            return new QueuedRecord(sequence, record);
        }
    }

    private static final class RelocationState {
        private final long serial;
        private final RelocationSeal seal;
        private final ArrayDeque<Entry> captured;
        private final ArrayDeque<Entry> held = new ArrayDeque<>();
        private boolean frozen;

        private RelocationState(
            long serial,
            RelocationSeal seal,
            ArrayDeque<Entry> captured) {
            this.serial = serial;
            this.seal = seal;
            this.captured = captured;
        }
    }
}
