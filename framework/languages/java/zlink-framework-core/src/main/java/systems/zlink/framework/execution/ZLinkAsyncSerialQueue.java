package systems.zlink.framework.execution;
import java.util.IdentityHashMap;
import java.util.Map;
import java.util.Objects;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executor;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;
import java.time.Duration;
import java.util.function.Supplier;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext;
import systems.zlink.framework.runtime.internal.relocation
    .ZLinkRetainedSerialQueueCommit;

/**
 * Serializes one logical owner's work and bounds only that owner's queued and
 * claimed records. These reservations are independent from Core byte HWM and
 * from the host-wide application-job supply authority.
 */
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
    private final ZLinkExecutionLanePolicy lanePolicy;
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
    private long relocationApplicationBacklog;
    private long relocationApplicationBytes;
    private final Map<Entry, Long> relocationEntryCosts =
        new IdentityHashMap<>();
    private int lifecycleStreak;
    private long outstanding;
    private long nextSequence = 1L;
    private long nextRelocationSerial = 1L;
    private Entry active;
    private int suspendedContinuations;
    private long turnClaimedAtNanos;
    private RelocationState relocation;
    private boolean relocated;
    private final List<CompletableFuture<Void>> quiescenceWaiters =
        new ArrayList<>();

    public ZLinkAsyncSerialQueue() {
        this(ZLinkExecutionLanePolicy.generic());
    }

    public ZLinkAsyncSerialQueue(ZLinkExecutionLanePolicy lanePolicy) {
        this(
            null,
            lanePolicy,
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
        ZLinkExecutionLanePolicy lanePolicy) {
        this(
            executor,
            lanePolicy,
            DEFAULT_APPLICATION_MESSAGE_CAPACITY,
            DEFAULT_APPLICATION_BYTE_CAPACITY,
            DEFAULT_LIFECYCLE_MESSAGE_CAPACITY,
            DEFAULT_LIFECYCLE_BYTE_CAPACITY,
            DEFAULT_FIXED_WORK_BYTE_COST,
            DEFAULT_LIFECYCLE_BURST_LIMIT,
            DEFAULT_OWNER_TIME_BUDGET);
    }

    public ZLinkAsyncSerialQueue(
        ZLinkExecutionLanePolicy lanePolicy,
        int pendingCapacity) {
        this(
            null,
            lanePolicy,
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
        ZLinkExecutionLanePolicy lanePolicy,
        int pendingCapacity) {
        this(
            executor,
            lanePolicy,
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
        ZLinkExecutionLanePolicy lanePolicy,
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
        this.lanePolicy = Objects.requireNonNull(lanePolicy, "lanePolicy");
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
        if (relocated) {
            return CompletableFuture.failedFuture(
                new IllegalStateException("queue owner has relocated"));
        }
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
        if (relocated) {
            return CompletableFuture.failedFuture(
                new IllegalStateException("queue owner has relocated"));
        }
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
        Objects.requireNonNull(operation, "operation");
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
            (byte[]) null,
            operation,
            () -> { },
            new CompletableFuture<>(),
            ZLinkFlowContext.current(),
            null,
            Lane.LIFECYCLE,
            fixedWorkByteCost,
            false);
        if (relocation != null) {
            holdRelocationEntry(entry);
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
        Objects.requireNonNull(operation, "operation");
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
            (byte[]) null,
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
        Objects.requireNonNull(record, "record");
        Objects.requireNonNull(relocationRelease, "relocationRelease");
        if (relocated) {
            return CompletableFuture.failedFuture(
                new IllegalStateException("queue owner has relocated"));
        }
        return enqueueAccepted(record.clone(), record.length, operation, relocationRelease);
    }

    /**
     * Enqueues a relocatable turn without materializing its relocation record
     * until a relocation seal captures the turn.
     */
    public synchronized CompletionStage<Void> enqueueRelocatableLazyRecord(
        Supplier<byte[]> record,
        long recordSizeHint,
        Supplier<CompletionStage<Void>> operation,
        Runnable relocationRelease) {
        Objects.requireNonNull(record, "record");
        Objects.requireNonNull(operation, "operation");
        Objects.requireNonNull(relocationRelease, "relocationRelease");
        validatePayloadBytes(recordSizeHint);
        if (relocated) {
            return CompletableFuture.failedFuture(
                new IllegalStateException("queue owner has relocated"));
        }
        if (nextSequence == Long.MAX_VALUE) {
            throw new IllegalStateException("queue sequence exhausted");
        }
        if (!canRepresentWorkByteCost(recordSizeHint)) {
            return capacityFailure("application payload exceeds queue byte capacity");
        }
        long byteCost = workByteCost(recordSizeHint);
        if (relocation != null) {
            return holdRelocationIngress(
                record,
                byteCost,
                operation,
                relocationRelease);
        }
        if (!canReserve(Lane.APPLICATION, byteCost)) {
            return capacityFailure("application queue is full");
        }
        reserve(Lane.APPLICATION, byteCost);
        Entry entry = new Entry(
            nextSequence++, record, operation, relocationRelease,
            new CompletableFuture<>(), ZLinkFlowContext.current(), null,
            Lane.APPLICATION, byteCost, false);
        applicationPending.addLast(entry);
        startNext();
        return entry.result;
    }

    public synchronized boolean tryEnqueue(Supplier<CompletionStage<Void>> operation) {
        if (relocated) {
            return false;
        }
        if (!canAcceptApplicationWork(fixedWorkByteCost)) {
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
        if (relocated) {
            return false;
        }
        if (!canRepresentWorkByteCost(payloadBytes)
            || !canAcceptApplicationWork(workByteCost(payloadBytes))) {
            return false;
        }
        enqueueAccepted(null, payloadBytes, operation);
        return true;
    }

    public synchronized boolean tryEnqueueRelocatable(
        byte[] record,
        Supplier<CompletionStage<Void>> operation) {
        Objects.requireNonNull(record, "record");
        if (relocated) {
            return false;
        }
        if (!canRepresentWorkByteCost(record.length)
            || !canAcceptApplicationWork(workByteCost(record.length))) {
            return false;
        }
        enqueueAccepted(record.clone(), record.length, operation);
        return true;
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
        Objects.requireNonNull(operation, "operation");
        validatePayloadBytes(payloadBytes);
        if (relocated) {
            return CompletableFuture.failedFuture(
                new IllegalStateException("queue owner has relocated"));
        }
        if (nextSequence == Long.MAX_VALUE) {
            throw new IllegalStateException("queue sequence exhausted");
        }
        if (!canRepresentWorkByteCost(payloadBytes)) {
            return capacityFailure("application payload exceeds queue byte capacity");
        }
        long byteCost = workByteCost(payloadBytes);
        if (relocation != null) {
            return holdRelocationIngress(
                record,
                byteCost,
                operation,
                relocationRelease);
        }
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
        applicationPending.addLast(entry);
        startNext();
        return entry.result;
    }

    private CompletionStage<Void> holdRelocationIngress(
        byte[] record,
        long byteCost,
        Supplier<CompletionStage<Void>> operation,
        Runnable relocationRelease) {
        if (!canHoldRelocationCost(byteCost)) {
            return capacityFailure("application queue byte accounting overflow");
        }
        Entry entry = new Entry(
            nextSequence++,
            record,
            operation,
            relocationRelease,
            new CompletableFuture<>(),
            ZLinkFlowContext.current(),
            null,
            Lane.APPLICATION,
            0L,
            false);
        outstanding++;
        relocationApplicationBacklog++;
        relocationApplicationBytes += byteCost;
        relocationEntryCosts.put(entry, byteCost);
        holdRelocationEntry(entry);
        return entry.result;
    }

    private CompletionStage<Void> holdRelocationIngress(
        Supplier<byte[]> record,
        long byteCost,
        Supplier<CompletionStage<Void>> operation,
        Runnable relocationRelease) {
        if (!canHoldRelocationCost(byteCost)) {
            return capacityFailure("application queue byte accounting overflow");
        }
        Entry entry = new Entry(
            nextSequence++,
            record,
            operation,
            relocationRelease,
            new CompletableFuture<>(),
            ZLinkFlowContext.current(),
            null,
            Lane.APPLICATION,
            0L,
            false);
        outstanding++;
        relocationApplicationBacklog++;
        relocationApplicationBytes += byteCost;
        relocationEntryCosts.put(entry, byteCost);
        holdRelocationEntry(entry);
        return entry.result;
    }

    private void holdRelocationEntry(Entry entry) {
        if (relocation == null) {
            throw new IllegalStateException(
                "relocation ingress requires an active seal");
        }
        if (relocation.acceptanceEpoch == Long.MAX_VALUE) {
            throw new IllegalStateException(
                "relocation ingress epoch exhausted");
        }
        relocation.held.addLast(entry);
        relocation.acceptanceEpoch++;
    }

    private boolean canReserve(Lane lane, long byteCost) {
        if (byteCost <= 0) {
            return false;
        }
        if (lane == Lane.APPLICATION) {
            return applicationMessages < applicationMessageCapacity
                && relocationApplicationBacklog
                    < applicationMessageCapacity - applicationMessages
                && fitsWithinCapacity(
                    applicationBytes,
                    relocationApplicationBytes,
                    byteCost,
                    applicationByteCapacity);
        }
        return lifecycleMessages < lifecycleMessageCapacity
            && lifecycleBytes <= lifecycleByteCapacity
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
        if (entry.applicationJobOwnership != null) {
            entry.applicationJobOwnership.close();
        }
        if (entry.capacityReserved) {
            if (entry.lane == Lane.APPLICATION) {
                applicationMessages--;
                applicationBytes -= entry.byteCost;
            } else {
                lifecycleMessages--;
                lifecycleBytes -= entry.byteCost;
            }
        } else if (entry.lane == Lane.APPLICATION) {
            relocationApplicationBacklog--;
            Long cost = relocationEntryCosts.remove(entry);
            if (cost == null) {
                throw new IllegalStateException(
                    "relocation backlog entry has no byte cost");
            }
            relocationApplicationBytes -= cost;
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

    private boolean canAcceptApplicationWork(long byteCost) {
        return relocation == null
            ? canReserve(Lane.APPLICATION, byteCost)
            : canHoldRelocationCost(byteCost);
    }

    private boolean canHoldRelocationCost(long byteCost) {
        return byteCost > 0
            && relocationApplicationBytes <= Long.MAX_VALUE - byteCost;
    }

    private static boolean fitsWithinCapacity(
        long first,
        long second,
        long added,
        long capacity) {
        if (first > capacity) {
            return false;
        }
        long remaining = capacity - first;
        if (second > remaining) {
            return false;
        }
        return added <= remaining - second;
    }

    private static void validatePayloadBytes(long payloadBytes) {
        if (payloadBytes < 0) {
            throw new IllegalArgumentException("payloadBytes must be non-negative");
        }
    }

    private CompletionStage<Void> capacityFailure(String message) {
        return CompletableFuture.failedFuture(
            new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.CAPACITY_EXCEEDED,
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
            entry.flow,
            entry.applicationJobOwnership);
        gate.whenComplete((ignored, error) -> finish(entry));
    }

    private void finish(Entry entry) {
        List<CompletableFuture<Void>> quiescent = List.of();
        RelocationBoundary boundary = entry.relocationBoundary;
        boolean yieldToExecutor = false;
        synchronized (this) {
            if (active != entry) {
                return;
            }
            active = null;
            release(entry);
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
        if (yieldToExecutor) {
            try {
                executor.execute(this::startNextAsync);
            } catch (RuntimeException rejected) {
                startNextAsync();
            }
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
        return trySealRelocation((RelocationBoundary) null);
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
            (byte[]) null,
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
        return sealNowLocked();
    }

    /**
     * Binds the exact turn that is active on the calling thread so a later
     * asynchronous continuation can seal this queue underneath it. A
     * deferred Actor Join runs its complete cross-node relocation while its
     * mailbox barrier stays the active turn; reserving another lifecycle
     * boundary from inside that operation would queue the boundary behind
     * the barrier itself and never reach it.
     */
    public synchronized Optional<ActiveTurnSealHandle>
        captureActiveTurnSealHandle() {
        if (CURRENT.get() != this || active == null) {
            return Optional.empty();
        }
        return Optional.of(new ActiveTurnSealHandle(this, active));
    }

    /** Seals this queue while the captured turn is still the active turn. */
    public synchronized Optional<RelocationSeal> trySealRelocation(
        ActiveTurnSealHandle handle) {
        if (relocated || relocation != null) {
            return Optional.empty();
        }
        if (handle == null || handle.owner != this || handle.entry != active) {
            return Optional.empty();
        }
        return sealNowLocked();
    }

    public static final class ActiveTurnSealHandle {
        private final ZLinkAsyncSerialQueue owner;
        private final Entry entry;

        private ActiveTurnSealHandle(ZLinkAsyncSerialQueue owner, Entry entry) {
            this.owner = owner;
            this.entry = entry;
        }
    }

    private Optional<RelocationSeal> sealNowLocked() {
        if (suspendedContinuations != 0
            || !continuationPending.isEmpty()
            || applicationPending.stream().anyMatch(
                entry -> !entry.hasRelocationRecord())) {
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
        if (!matches(seal) || relocation.retained != null) {
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

    /** Captures the current ingress high-water before authority prepare. */
    public synchronized Optional<List<QueuedRecord>>
        freezeRelocationIngress(RelocationSeal seal) {
        if (!matches(seal) || relocation.frozen) {
            return Optional.empty();
        }
        relocation.frozen = true;
        return Optional.of(relocation.held.stream()
            .filter(Entry::hasRelocationRecord)
            .map(Entry::queuedRecord)
            .toList());
    }

    public Optional<List<QueuedRecord>> commitRelocation(
        RelocationSeal seal) {
        Optional<RetainedCommit> retained = retainRelocationCommit(seal);
        if (retained.isEmpty()) {
            return Optional.empty();
        }
        RetainedCommit commit = retained.orElseThrow();
        if (!ZLinkRetainedSerialQueueCommit.capture(
            commit.records(), commit)) {
            ZLinkRetainedSerialQueueCommit.Cut cut = commit.cut();
            synchronized (this) {
                if (!commit.matches(cut)) {
                    throw new IllegalStateException(
                        "serial queue relocation cut changed during commit");
                }
                commit.establish(cut);
                commit.finish(cut);
            }
            commit.complete();
        }
        return Optional.of(commit.records());
    }

    /**
     * Detaches a committed relocation while retaining source resources until
     * the target has durably replayed the returned ingress records.
     */
    private synchronized Optional<RetainedCommit> retainRelocationCommit(
        RelocationSeal seal) {
        if (!matches(seal) || relocation.retained != null) {
            return Optional.empty();
        }
        List<QueuedRecord> held = relocation.held.stream()
            .filter(Entry::hasRelocationRecord)
            .map(Entry::queuedRecord)
            .toList();
        RetainedCommit retained = new RetainedCommit(this, held);
        relocation.retained = retained;
        return Optional.of(retained);
    }

    private void completeRelocationCommit(RetainedCommit commit) {
        if (!commit.completed.compareAndSet(false, true)) {
            return;
        }
        if (commit.entries == null) {
            throw new IllegalStateException(
                "serial queue relocation capture is not terminal");
        }
        for (Entry entry : commit.entries) {
            RuntimeException failure = null;
            try {
                entry.relocationRelease.run();
            } catch (RuntimeException error) {
                failure = error;
            } finally {
                synchronized (this) {
                    release(entry);
                }
            }
            if (failure == null) {
                entry.result.complete(null);
            } else {
                entry.result.completeExceptionally(failure);
            }
        }
        List<CompletableFuture<Void>> quiescent;
        synchronized (this) {
            quiescent = takeQuiescenceWaitersIfReady();
        }
        quiescent.forEach(waiter -> waiter.complete(null));
    }

    private boolean matches(RelocationSeal seal) {
        return seal != null
            && relocation != null
            && relocation.serial == seal.serial
            && relocation.seal == seal;
    }

    private CompletionStage<Void> invoke(
        Supplier<CompletionStage<Void>> operation,
        CompletableFuture<Void> result,
        ZLinkFlowContext.State flow,
        ZLinkApplicationJobContext.QueuedOwnership applicationJobOwnership) {
        CompletableFuture<Void> gate = new CompletableFuture<>();
        try {
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
                         : ZLinkFlowContext.enter(flow);
                     ZLinkApplicationJobContext.Scope applicationJob =
                         ZLinkApplicationJobContext.enterQueued(
                             applicationJobOwnership)) {
                    CompletionStage<Void> execution = Objects.requireNonNull(
                        operation.get(), "operation result");
                    execution.whenComplete((value, error) -> {
                        if (error != null) {
                            result.completeExceptionally(error);
                        } else {
                            result.complete(null);
                        }
                        if (!lanePolicy.releasesGateOnIncompleteStage()) {
                            gate.complete(null);
                        }
                    });
                    if (lanePolicy.releasesGateOnIncompleteStage()
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
        } catch (RuntimeException rejected) {
            result.completeExceptionally(rejected);
            gate.complete(null);
        }
        return gate;
    }

    public static <T> CompletionStage<T> manageCurrent(CompletionStage<T> stage) {
        Objects.requireNonNull(stage, "stage");
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
        if (!queue.lanePolicy.releasesGateOnIncompleteStage()) {
            CompletableFuture<Void> gate = turn.gate;
            stage.whenComplete((value, error) -> {
                try {
                    queue.executor.execute(() -> {
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
                    });
                } catch (RuntimeException rejected) {
                    managed.completeExceptionally(rejected);
                    if (gate != null) {
                        gate.complete(null);
                    }
                }
            });
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
                        // A suspended managed turn has no continuation left
                        // to release its gate after admission failure. Release
                        // the original turn so the queue cannot remain active
                        // forever when relocation/capacity closes the lane.
                        if (turn.gate != null) {
                            turn.gate.complete(null);
                        }
                    }
                });
            } catch (RuntimeException continuationFailure) {
                managed.completeExceptionally(continuationFailure);
                if (turn.gate != null) {
                    turn.gate.complete(null);
                }
            }
        });
        return managed;
    }

    public static <T> CompletionStage<T> yieldCurrent(CompletionStage<T> stage) {
        Objects.requireNonNull(stage, "stage");
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
            (byte[]) null,
            operation,
            () -> { },
            new CompletableFuture<>(),
            ZLinkFlowContext.current(),
            null,
            Lane.APPLICATION,
            fixedWorkByteCost,
            true);
        if (relocation != null) {
            holdRelocationEntry(continuation);
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
        Objects.requireNonNull(executor, "executor");
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
        Objects.requireNonNull(entered, "entered");
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

    private static final class RetainedCommit
        implements ZLinkRetainedSerialQueueCommit.Owner {
        private final ZLinkAsyncSerialQueue owner;
        private final List<QueuedRecord> records;
        private List<Entry> entries;
        private final AtomicBoolean completed = new AtomicBoolean();

        private RetainedCommit(
            ZLinkAsyncSerialQueue owner,
            List<QueuedRecord> records) {
            this.owner = owner;
            this.records = List.copyOf(records);
        }

        private List<QueuedRecord> records() {
            return records;
        }

        @Override
        public Object monitor() {
            return owner;
        }

        @Override
        public ZLinkRetainedSerialQueueCommit.Cut cut() {
            synchronized (owner) {
                if (owner.relocation == null
                    || owner.relocation.retained != this) {
                    throw new IllegalStateException(
                        "serial queue retained relocation is not active");
                }
                List<QueuedRecord> current = owner.relocation.held.stream()
                    .filter(Entry::hasRelocationRecord)
                    .map(Entry::queuedRecord)
                    .toList();
                return ZLinkRetainedSerialQueueCommit.cut(
                    this,
                    owner.relocation.acceptanceEpoch,
                    current);
            }
        }

        @Override
        public boolean matches(ZLinkRetainedSerialQueueCommit.Cut cut) {
            synchronized (owner) {
                return cut != null
                    && cut.belongsTo(this)
                    && owner.relocation != null
                    && owner.relocation.retained == this
                    && owner.relocation.acceptanceEpoch == cut.epoch();
            }
        }

        @Override
        public void establish(ZLinkRetainedSerialQueueCommit.Cut cut) {
            synchronized (owner) {
                if (!matches(cut)) {
                    throw new IllegalStateException(
                        "serial queue durable cut is no longer current");
                }
                durableCut = true;
            }
        }

        @Override
        public void finish(ZLinkRetainedSerialQueueCommit.Cut cut) {
            synchronized (owner) {
                if (!matches(cut)) {
                    throw new IllegalStateException(
                        "serial queue relocation cut is no longer current");
                }
                if (!durableCut) {
                    throw new IllegalStateException(
                        "serial queue durable cut is not established");
                }
                List<Entry> released = new ArrayList<>(
                    owner.relocation.captured.size()
                        + owner.relocation.held.size());
                released.addAll(owner.relocation.captured);
                released.addAll(owner.relocation.held);
                entries = List.copyOf(released);
                owner.relocation = null;
                owner.relocated = true;
            }
        }

        @Override
        public boolean canAbort() {
            synchronized (owner) {
                return !completed.get()
                    && (owner.relocation != null
                            && owner.relocation.retained == this
                        || owner.relocation == null
                            && owner.relocated
                            && entries != null);
            }
        }

        @Override
        public void abort() {
            synchronized (owner) {
                if (!canAbort()) {
                    throw new IllegalStateException(
                        "serial queue retained relocation cannot be restored");
                }
                List<Entry> restored;
                if (owner.relocation != null) {
                    restored = new ArrayList<>(
                        owner.relocation.captured.size()
                            + owner.relocation.held.size());
                    restored.addAll(owner.relocation.captured);
                    restored.addAll(owner.relocation.held);
                    owner.relocation = null;
                } else {
                    restored = entries;
                    owner.relocated = false;
                }
                entries = null;
                durableCut = false;
                for (Entry entry : restored) {
                    if (entry.lane == Lane.LIFECYCLE) {
                        owner.lifecyclePending.addLast(entry);
                    } else if (entry.continuation) {
                        owner.continuationPending.addLast(entry);
                    } else {
                        owner.applicationPending.addLast(entry);
                    }
                }
                owner.startNext();
            }
        }

        @Override
        public void complete() {
            owner.completeRelocationCommit(this);
        }

        private boolean durableCut;
    }

    public record QueuedRecord(long sequence, byte[] payload) {
        public QueuedRecord {
            if (sequence <= 0) {
                throw new IllegalArgumentException(
                    "queue record sequence must be positive");
            }
            payload = Objects.requireNonNull(
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
        private byte[] record;
        private final Supplier<byte[]> lazyRecord;
        private final Supplier<CompletionStage<Void>> operation;
        private final Runnable relocationRelease;
        private final CompletableFuture<Void> result;
        private final ZLinkFlowContext.State flow;
        private final ZLinkApplicationJobContext.QueuedOwnership
            applicationJobOwnership;
        private final RelocationBoundary relocationBoundary;
        private final Lane lane;
        private final long byteCost;
        private final boolean capacityReserved;
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
            this.lazyRecord = null;
            this.operation = operation;
            this.relocationRelease = relocationRelease;
            this.result = result;
            this.flow = flow;
            this.applicationJobOwnership =
                ZLinkApplicationJobContext.transferToQueuedJob();
            this.relocationBoundary = relocationBoundary;
            this.lane = lane;
            this.byteCost = byteCost;
            this.capacityReserved = byteCost > 0L;
            this.continuation = continuation;
        }

        private Entry(
            long sequence,
            Supplier<byte[]> lazyRecord,
            Supplier<CompletionStage<Void>> operation,
            Runnable relocationRelease,
            CompletableFuture<Void> result,
            ZLinkFlowContext.State flow,
            RelocationBoundary relocationBoundary,
            Lane lane,
            long byteCost,
            boolean continuation) {
            this.sequence = sequence;
            this.record = null;
            this.lazyRecord = Objects.requireNonNull(
                lazyRecord, "lazyRecord");
            this.operation = operation;
            this.relocationRelease = relocationRelease;
            this.result = result;
            this.flow = flow;
            this.applicationJobOwnership =
                ZLinkApplicationJobContext.transferToQueuedJob();
            this.relocationBoundary = relocationBoundary;
            this.lane = lane;
            this.byteCost = byteCost;
            this.capacityReserved = byteCost > 0L;
            this.continuation = continuation;
        }

        private boolean hasRelocationRecord() {
            return record != null || lazyRecord != null;
        }

        private synchronized QueuedRecord queuedRecord() {
            if (record == null) {
                record = Objects.requireNonNull(
                    lazyRecord.get(), "relocation record supplier returned null");
            }
            return new QueuedRecord(sequence, record);
        }
    }

    private static final class RelocationState {
        private final long serial;
        private final RelocationSeal seal;
        private final ArrayDeque<Entry> captured;
        private final ArrayDeque<Entry> held = new ArrayDeque<>();
        private boolean frozen;
        private long acceptanceEpoch;
        private RetainedCommit retained;

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
