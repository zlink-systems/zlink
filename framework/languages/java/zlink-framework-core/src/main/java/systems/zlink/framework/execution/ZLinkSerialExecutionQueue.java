package systems.zlink.framework.execution;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.runtime.internal.ZLinkCompletionBridge;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext;
import systems.zlink.framework.runtime.internal.relocation.ZLinkRetainedSerialQueueCommit;
import systems.zlink.framework.runtime.messaging.ZLinkFrameworkErrorOrigin;

import java.time.Duration;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executor;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.BooleanSupplier;
import java.util.function.Function;
import java.util.function.Predicate;
import java.util.function.Supplier;

/**
 * Serializes one logical owner's work. Host-shared application-job permits and Core byte HWM
 * control admission; this queue only orders accepted work.
 */
public final class ZLinkSerialExecutionQueue {
    public static final int DEFAULT_LIFECYCLE_BURST_LIMIT = 8;
    public static final Duration DEFAULT_OWNER_TIME_BUDGET = Duration.ofMillis(10);
    private static final ThreadLocal<ZLinkSerialExecutionQueue> CURRENT = new ThreadLocal<>();
    private static final ThreadLocal<CompletableFuture<Void>> CURRENT_GATE = new ThreadLocal<>();
    private static final ThreadLocal<Boolean> CURRENT_RELEASE_DEFERRED = new ThreadLocal<>();
    // A queue never runs a drain on its submitter's stack.  This is shared by
    // every owner; it is deliberately not an executor per Spot, Actor, or
    // session.
    private static final ExecutorService DRAIN_EXECUTOR =
            Executors.newVirtualThreadPerTaskExecutor();

    private final Executor executor;
    private final ExecutorService ownedExecutor;
    private final ZLinkExecutionLanePolicy lanePolicy;
    private final ZLinkSerialExecutionQueue timerOwner;
    private final int lifecycleBurstLimit;
    private final long ownerTimeBudgetNanos;
    private final ArrayDeque<Entry> applicationPending = new ArrayDeque<>();
    // A resumed application turn is kept separate so it can pass a relocation
    // boundary without inserting at the front of the application FIFO.
    private final ArrayDeque<Entry> continuationPending = new ArrayDeque<>();
    private final ArrayDeque<Entry> lifecyclePending = new ArrayDeque<>();
    private int lifecycleStreak;
    private long outstanding;
    private long nextSequence = 1L;
    private long nextRelocationSerial = 1L;
    private Entry active;
    private Entry suspendedLifecycle;
    private boolean drainScheduled;
    private int suspendedApplicationContinuations;
    private int suspendedLifecycleContinuations;
    private long turnClaimedAtNanos;
    private RelocationState relocation;
    private boolean closingAdmissionSealed;
    private boolean relocated;
    private final List<QuiescenceWaiter> quiescenceWaiters = new ArrayList<>();

    public synchronized void sealClosingAdmission() {
        if (relocation == null) {
            closingAdmissionSealed = true;
        }
    }

    /**
     * Close step 3 (spec 03-spot-actor/06-spot-address-messaging §7) under this queue's admission
     * lock: a retained message already waiting behind the Close keeps retained admission open for
     * the next incarnation; otherwise the Closing seal ends it in the same decision, so a retained
     * message arriving later is refused before admission.
     *
     * @return whether a retained message waits behind the Close
     */
    public synchronized boolean retainsPendingOrSealClosingAdmission() {
        LifecycleTransition transition = lifecycleTransitionLocked();
        if (transition != null
                && applicationPending.stream()
                        .anyMatch(
                                entry ->
                                        entry.message != null
                                                && transition.retains.test(entry.message))) {
            return true;
        }
        sealClosingAdmission();
        return false;
    }

    /**
     * The one admission decision of this queue (spec 03-spot-actor/06-spot-address-messaging §7): a
     * Closing seal rejects new work, a relocated owner reports the post-cut arrival, and otherwise
     * the queue accepts. A relocation seal is not a rejection; the caller holds the work.
     */
    private CompletionStage<Void> admissionFailureLocked() {
        LifecycleTransition transition = lifecycleTransitionLocked();
        boolean closing = transition != null && transition.committed.getAsBoolean();
        if (closingAdmissionSealed || closing) {
            return CompletableFuture.failedFuture(
                    ZLinkFrameworkErrorOrigin.framework(
                            closing
                                    ? ZLinkFrameworkErrorKind.NOT_FOUND
                                    : ZLinkFrameworkErrorKind.REJECTED,
                            "Spot incarnation is closing"));
        }
        return relocated ? CompletableFuture.failedFuture(new RelocatedOwnerException()) : null;
    }

    private LifecycleTransition lifecycleTransitionLocked() {
        Entry owner = suspendedLifecycle == null ? active : suspendedLifecycle;
        return owner != null && owner.operation instanceof LifecycleTransition transition
                ? transition
                : null;
    }

    /**
     * Keeps accepted messages in this owner's FIFO while a lifecycle operation replaces its
     * incarnation.
     */
    public CompletionStage<Void> enqueueLifecycleTransition(
            Supplier<CompletionStage<Void>> operation,
            Function<Object, CompletionStage<Void>> dispatch,
            BooleanSupplier committed,
            Predicate<Object> retains) {
        return enqueueBarrierNext(
                new LifecycleTransition(operation, dispatch, committed, retains), null);
    }

    /** The original message is retained; its old handler closure is never replayed. */
    public CompletionStage<Void> enqueueMessage(
            Object message,
            long payloadBytes,
            Supplier<CompletionStage<Void>> operation,
            Runnable release,
            CompletableFuture<Void> admission) {
        EnqueueResult result;
        synchronized (this) {
            Objects.requireNonNull(message, "message");
            LifecycleTransition transition = lifecycleTransitionLocked();
            boolean retained = transition != null && transition.retains.test(message);
            if (transition != null
                    && transition.committed.getAsBoolean()
                    && (!retained || closingAdmissionSealed)) {
                //  A retained message after the Closing seal has no next incarnation here: it
                //  is refused before admission, as a released owner fence.
                CompletionStage<Void> rejected =
                        retained
                                ? CompletableFuture.failedFuture(
                                        ZLinkFrameworkErrorOrigin.ownerFenceRefusal(
                                                "Spot incarnation released its authority"))
                                : admissionFailureLocked();
                if (admission != null)
                    rejected.whenComplete(
                            (ignored, failure) -> admission.completeExceptionally(failure));
                return rejected;
            }
            result = enqueueAccepted(null, payloadBytes, operation, release, message);
        }
        scheduleDrainIfNeeded(result);
        if (admission != null) admission.complete(null);
        return result.result();
    }

    public synchronized List<Object> pendingMessages() {
        return applicationPending.stream()
                .map(entry -> entry.message)
                .filter(Objects::nonNull)
                .toList();
    }

    private final class LifecycleTransition implements Supplier<CompletionStage<Void>> {
        private final Supplier<CompletionStage<Void>> operation;
        private final Function<Object, CompletionStage<Void>> dispatch;
        private final BooleanSupplier committed;
        private final Predicate<Object> retains;

        private LifecycleTransition(
                Supplier<CompletionStage<Void>> operation,
                Function<Object, CompletionStage<Void>> dispatch,
                BooleanSupplier committed,
                Predicate<Object> retains) {
            this.operation = Objects.requireNonNull(operation, "operation");
            this.dispatch = Objects.requireNonNull(dispatch, "dispatch");
            this.committed = Objects.requireNonNull(committed, "committed");
            this.retains = Objects.requireNonNull(retains, "retains");
        }

        @Override
        public CompletionStage<Void> get() {
            return operation
                    .get()
                    .whenComplete(
                            (ignored, failure) -> {
                                if (!committed.getAsBoolean()) return;
                                synchronized (ZLinkSerialExecutionQueue.this) {
                                    for (Entry entry : applicationPending) {
                                        entry.operation =
                                                failure == null
                                                        ? () -> dispatchPending(entry)
                                                        : () -> {
                                                            entry.relocationRelease.run();
                                                            return CompletableFuture.failedFuture(
                                                                    failure);
                                                        };
                                    }
                                }
                            });
        }

        private void commit() {
            List<Entry> discarded;
            synchronized (ZLinkSerialExecutionQueue.this) {
                discarded =
                        applicationPending.stream()
                                .filter(
                                        entry ->
                                                entry.message == null
                                                        || !retains.test(entry.message))
                                .toList();
                applicationPending.removeAll(discarded);
                for (Entry entry : applicationPending) {
                    entry.operation = () -> dispatchPending(entry);
                }
                discarded.forEach(ZLinkSerialExecutionQueue.this::release);
            }
            for (Entry entry : discarded) {
                entry.relocationRelease.run();
                if (entry.message != null || entry.hasRelocationRecord()) {
                    entry.result.completeExceptionally(
                            ZLinkFrameworkErrorOrigin.framework(
                                    ZLinkFrameworkErrorKind.NOT_FOUND,
                                    "Spot incarnation was closed"));
                } else entry.result.complete(null);
            }
        }

        private CompletionStage<Void> dispatchPending(Entry entry) {
            if (entry.message != null) return dispatch.apply(entry.message);
            entry.relocationRelease.run();
            return entry.hasRelocationRecord()
                    ? CompletableFuture.failedFuture(
                            ZLinkFrameworkErrorOrigin.framework(
                                    ZLinkFrameworkErrorKind.NOT_FOUND,
                                    "Spot incarnation was closed"))
                    : CompletableFuture.completedFuture(null);
        }
    }

    public void commitLifecycleTransition() {
        LifecycleTransition transition;
        synchronized (this) {
            transition = lifecycleTransitionLocked();
        }
        if (transition == null)
            throw new IllegalStateException("lifecycle transition is not active");
        transition.commit();
    }

    public synchronized CompletionStage<Void> admitIngress(
            Supplier<CompletionStage<Void>> admission) {
        Objects.requireNonNull(admission, "admission");
        CompletionStage<Void> rejection = admissionFailureLocked();
        return rejection == null ? admission.get() : rejection;
    }

    public ZLinkSerialExecutionQueue() {
        this(ZLinkExecutionLanePolicy.generic());
    }

    public ZLinkSerialExecutionQueue(ZLinkExecutionLanePolicy lanePolicy) {
        this(null, lanePolicy, DEFAULT_LIFECYCLE_BURST_LIMIT, DEFAULT_OWNER_TIME_BUDGET);
    }

    public ZLinkSerialExecutionQueue(Executor executor, ZLinkExecutionLanePolicy lanePolicy) {
        this(executor, lanePolicy, DEFAULT_LIFECYCLE_BURST_LIMIT, DEFAULT_OWNER_TIME_BUDGET);
    }

    public ZLinkSerialExecutionQueue(
            Executor executor,
            ZLinkExecutionLanePolicy lanePolicy,
            int lifecycleBurstLimit,
            Duration ownerTimeBudget) {
        this(executor, lanePolicy, lifecycleBurstLimit, ownerTimeBudget, null);
    }

    private ZLinkSerialExecutionQueue(
            Executor executor,
            ZLinkExecutionLanePolicy lanePolicy,
            int lifecycleBurstLimit,
            Duration ownerTimeBudget,
            ZLinkSerialExecutionQueue timerOwner) {
        if (lifecycleBurstLimit <= 0
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
        this.timerOwner = timerOwner;
        this.lifecycleBurstLimit = lifecycleBurstLimit;
        this.ownerTimeBudgetNanos = ownerTimeBudget.toNanos();
    }

    /** Creates a timer queue whose current turn can be identified by its owning Spot queue. */
    public static ZLinkSerialExecutionQueue spotTimer(
            Executor executor, ZLinkSerialExecutionQueue owner) {
        return new ZLinkSerialExecutionQueue(
                executor,
                ZLinkExecutionLanePolicy.spot(),
                DEFAULT_LIFECYCLE_BURST_LIMIT,
                DEFAULT_OWNER_TIME_BUDGET,
                Objects.requireNonNull(owner, "owner"));
    }

    public void close() {
        assert assertRelocationBoundariesFinished();
        if (ownedExecutor != null) {
            ownedExecutor.shutdown();
        }
    }

    private synchronized boolean assertRelocationBoundariesFinished() {
        List<String> pending = new ArrayList<>();
        if (active != null
                && active.relocationBoundary != null
                && !active.relocationBoundary.finished.isDone()) {
            pending.add("relocation boundary:" + active.sequence);
        }
        if (suspendedLifecycle != null
                && suspendedLifecycle.relocationBoundary != null
                && !suspendedLifecycle.relocationBoundary.finished.isDone()) {
            pending.add("relocation boundary:" + suspendedLifecycle.sequence);
        }
        for (Entry entry : lifecyclePending) {
            if (entry.relocationBoundary != null && !entry.relocationBoundary.finished.isDone()) {
                pending.add("relocation boundary:" + entry.sequence);
            }
        }
        if (!pending.isEmpty()) {
            throw new AssertionError("incomplete drain work: " + pending);
        }
        return true;
    }

    /**
     * Rejection raised when a turn reaches this queue after its relocation cut finished. The typed
     * form lets the ingress owner tell a post-cut arrival apart from an ordinary admission failure
     * and re-route it through the relocation forward instead of dropping it (spec
     * server/03-spot-actor/08-routing.ko.md:222).
     */
    public static final class RelocatedOwnerException extends IllegalStateException {
        private static final long serialVersionUID = 1L;

        public RelocatedOwnerException() {
            super("queue owner has relocated");
        }
    }

    public CompletionStage<Void> enqueue(
            Supplier<CompletionStage<Void>> operation, CompletableFuture<Void> admission) {
        EnqueueResult result;
        CompletionStage<Void> rejection = null;
        synchronized (this) {
            rejection = admissionFailureLocked();
            result = rejection == null ? enqueueAccepted(null, 0, operation) : null;
        }
        if (rejection != null) {
            if (admission != null) {
                rejection.whenComplete((done, failure) -> admission.completeExceptionally(failure));
            }
            return rejection;
        }
        scheduleDrainIfNeeded(result);
        if (admission != null) admission.complete(null);
        return result.result();
    }

    /** Enqueues work whose admission was already decided by its owner queue. */
    public CompletionStage<Void> enqueuePreviouslyAccepted(
            Supplier<CompletionStage<Void>> operation) {
        EnqueueResult result;
        synchronized (this) {
            result = enqueueAccepted(null, 0, operation);
        }
        scheduleDrainIfNeeded(result);
        return result.result();
    }

    /**
     * Enqueues an application turn and charges its known payload length in the application byte
     * budget. The queue also charges the fixed per-turn cost; callers must pass the payload length
     * before deserializing the payload.
     */
    public CompletionStage<Void> enqueueWithPayloadBytes(
            long payloadBytes,
            Supplier<CompletionStage<Void>> operation,
            CompletableFuture<Void> admission) {
        EnqueueResult result;
        CompletionStage<Void> rejection = null;
        synchronized (this) {
            validatePayloadBytes(payloadBytes);
            rejection = admissionFailureLocked();
            result = rejection == null ? enqueueAccepted(null, payloadBytes, operation) : null;
        }
        if (rejection != null) {
            if (admission != null) {
                rejection.whenComplete((done, failure) -> admission.completeExceptionally(failure));
            }
            return rejection;
        }
        scheduleDrainIfNeeded(result);
        if (admission != null) admission.complete(null);
        return result.result();
    }

    /**
     * Returns whether this queue currently owns the calling thread's serial turn. Internal dispatch
     * composition uses this to avoid waiting on a turn that was enqueued behind the operation
     * currently executing on this queue.
     */
    public boolean isCurrent() {
        ZLinkSerialExecutionQueue queue = CURRENT.get();
        CompletableFuture<Void> gate = CURRENT_GATE.get();
        if (queue != null || gate != null) {
            return queue == this && gate != null && !gate.isDone();
        }
        Object propagated =
                systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext
                        .currentSerialExecutionTurn();
        if (!(propagated instanceof SerialTurnCarrier carrier)) {
            return false;
        }
        SerialTurn turn = carrier.turn;
        return turn != null && turn.queue() == this && turn.gate() != null && !turn.gate().isDone();
    }

    /**
     * Internal lifecycle barrier that runs immediately after the active turn and before previously
     * queued application turns.
     */
    public CompletionStage<Void> enqueueBarrierNext(
            Supplier<CompletionStage<Void>> operation, CompletableFuture<Void> admission) {
        EnqueueResult result;
        boolean rejected;
        synchronized (this) {
            rejected = relocated;
            result = rejected ? null : enqueueBarrierNextLocked(operation);
        }
        if (rejected) {
            RelocatedOwnerException failure = new RelocatedOwnerException();
            if (admission != null) admission.completeExceptionally(failure);
            return CompletableFuture.failedFuture(failure);
        }
        scheduleDrainIfNeeded(result);
        if (admission != null) admission.complete(null);
        return result.result();
    }

    public CompletionStage<Void> enqueueLifecycleAdmission(
            Supplier<CompletionStage<Void>> operation) {
        EnqueueResult result;
        synchronized (this) {
            CompletionStage<Void> rejection = admissionFailureLocked();
            if (rejection != null) {
                return rejection;
            }
            result = enqueueBarrierNextLocked(operation);
        }
        scheduleDrainIfNeeded(result);
        return result.result();
    }

    private EnqueueResult enqueueBarrierNextLocked(Supplier<CompletionStage<Void>> operation) {
        Objects.requireNonNull(operation, "operation");
        if (nextSequence == Long.MAX_VALUE) {
            throw new IllegalStateException("queue sequence exhausted");
        }
        Entry entry =
                new Entry(
                        nextSequence++,
                        (byte[]) null,
                        operation,
                        () -> {},
                        new CompletableFuture<>(),
                        ZLinkFlowContext.current(),
                        null,
                        Lane.LIFECYCLE,
                        false);
        outstanding++;
        if (relocation != null) {
            holdRelocationEntry(entry);
            return new EnqueueResult(entry, false);
        }
        lifecyclePending.addLast(entry);
        return new EnqueueResult(entry, requestDrainLocked());
    }

    /**
     * Internal lifecycle barrier that runs after every application turn accepted before this call.
     * Unlike application admission, this barrier remains available after relocation has committed
     * so the old owner can release local resources.
     */
    public CompletionStage<Void> enqueueLifecycleBarrier(
            Supplier<CompletionStage<Void>> operation) {
        EnqueueResult result;
        synchronized (this) {
            Objects.requireNonNull(operation, "operation");
            if (relocation != null) {
                return CompletableFuture.failedFuture(
                        new IllegalStateException("queue relocation is in progress"));
            }
            if (nextSequence == Long.MAX_VALUE) {
                throw new IllegalStateException("queue sequence exhausted");
            }
            Entry entry =
                    new Entry(
                            nextSequence++,
                            (byte[]) null,
                            operation,
                            () -> {},
                            new CompletableFuture<>(),
                            ZLinkFlowContext.current(),
                            null,
                            Lane.LIFECYCLE,
                            false);
            outstanding++;
            lifecyclePending.addLast(entry);
            result = new EnqueueResult(entry, requestDrainLocked());
        }
        scheduleDrainIfNeeded(result);
        return result.result();
    }

    public CompletionStage<Void> enqueueRelocatable(
            byte[] record,
            Supplier<CompletionStage<Void>> operation,
            Runnable relocationRelease,
            CompletableFuture<Void> admission) {
        EnqueueResult result;
        CompletionStage<Void> rejection;
        synchronized (this) {
            Objects.requireNonNull(record, "record");
            Objects.requireNonNull(relocationRelease, "relocationRelease");
            rejection = admissionFailureLocked();
            result =
                    rejection == null
                            ? enqueueAccepted(
                                    record.clone(), record.length, operation, relocationRelease)
                            : null;
        }
        if (rejection != null) {
            if (admission != null) {
                rejection.whenComplete((done, failure) -> admission.completeExceptionally(failure));
            }
            return rejection;
        }
        scheduleDrainIfNeeded(result);
        if (admission != null) admission.complete(null);
        return result.result();
    }

    /**
     * Enqueues a relocatable turn without materializing its relocation record until a relocation
     * seal captures the turn.
     */
    public CompletionStage<Void> enqueueRelocatableLazyRecord(
            Supplier<byte[]> record,
            long recordSizeHint,
            Supplier<CompletionStage<Void>> operation,
            Runnable relocationRelease,
            CompletableFuture<Void> admission) {
        EnqueueResult result = null;
        CompletionStage<Void> immediate = null;
        boolean accepted = false;
        synchronized (this) {
            Objects.requireNonNull(record, "record");
            Objects.requireNonNull(operation, "operation");
            Objects.requireNonNull(relocationRelease, "relocationRelease");
            validatePayloadBytes(recordSizeHint);
            immediate = admissionFailureLocked();
            if (immediate == null) {
                if (nextSequence == Long.MAX_VALUE) {
                    throw new IllegalStateException("queue sequence exhausted");
                }
                accepted = true;
                if (relocation != null) {
                    Entry held = holdRelocationIngress(record, operation, relocationRelease);
                    result = new EnqueueResult(held, false);
                    // Account for the held job after leaving the execution gate.
                    immediate = held.result;
                }
            }
            Entry entry =
                    immediate == null
                            ? new Entry(
                                    nextSequence++,
                                    record,
                                    operation,
                                    relocationRelease,
                                    new CompletableFuture<>(),
                                    ZLinkFlowContext.current(),
                                    null,
                                    Lane.APPLICATION,
                                    false)
                            : null;
            if (entry != null) {
                outstanding++;
                applicationPending.addLast(entry);
            }
            if (entry != null) result = new EnqueueResult(entry, requestDrainLocked());
        }
        if (result != null) scheduleDrainIfNeeded(result);
        if (admission != null) {
            if (accepted) admission.complete(null);
            else
                immediate.whenComplete((done, failure) -> admission.completeExceptionally(failure));
        }
        if (immediate != null) return immediate;
        return result.result();
    }

    public boolean tryEnqueue(Supplier<CompletionStage<Void>> operation) {
        EnqueueResult result;
        synchronized (this) {
            if (admissionFailureLocked() != null) {
                return false;
            }
            result = enqueueAccepted(null, 0, operation);
        }
        scheduleDrainIfNeeded(result);
        return true;
    }

    /**
     * Attempts to enqueue an application turn with a known payload length. The length participates
     * in the same byte admission as the fixed turn cost and is released when the turn reaches its
     * terminal boundary.
     */
    public boolean tryEnqueueWithPayloadBytes(
            long payloadBytes, Supplier<CompletionStage<Void>> operation) {
        EnqueueResult result;
        synchronized (this) {
            validatePayloadBytes(payloadBytes);
            if (admissionFailureLocked() != null) {
                return false;
            }
            result = enqueueAccepted(null, payloadBytes, operation);
        }
        scheduleDrainIfNeeded(result);
        return true;
    }

    public boolean tryEnqueueRelocatable(byte[] record, Supplier<CompletionStage<Void>> operation) {
        EnqueueResult result;
        synchronized (this) {
            Objects.requireNonNull(record, "record");
            if (admissionFailureLocked() != null) {
                return false;
            }
            result = enqueueAccepted(record.clone(), record.length, operation);
        }
        scheduleDrainIfNeeded(result);
        return true;
    }

    private EnqueueResult enqueueAccepted(
            byte[] record, long payloadBytes, Supplier<CompletionStage<Void>> operation) {
        return enqueueAccepted(record, payloadBytes, operation, () -> {});
    }

    private EnqueueResult enqueueAccepted(
            byte[] record,
            long payloadBytes,
            Supplier<CompletionStage<Void>> operation,
            Runnable relocationRelease) {
        return enqueueAccepted(record, payloadBytes, operation, relocationRelease, null);
    }

    private EnqueueResult enqueueAccepted(
            byte[] record,
            long payloadBytes,
            Supplier<CompletionStage<Void>> operation,
            Runnable relocationRelease,
            Object message) {
        Objects.requireNonNull(operation, "operation");
        validatePayloadBytes(payloadBytes);
        if (relocated) {
            return new EnqueueResult(
                    CompletableFuture.failedFuture(new RelocatedOwnerException()), false, null);
        }
        if (nextSequence == Long.MAX_VALUE) {
            throw new IllegalStateException("queue sequence exhausted");
        }
        if (relocation != null) {
            return new EnqueueResult(
                    holdRelocationIngress(record, operation, relocationRelease, message), false);
        }
        Entry entry =
                new Entry(
                        nextSequence++,
                        record,
                        operation,
                        relocationRelease,
                        new CompletableFuture<>(),
                        ZLinkFlowContext.current(),
                        null,
                        Lane.APPLICATION,
                        false);
        outstanding++;
        entry.message = message;
        applicationPending.addLast(entry);
        return new EnqueueResult(entry, requestDrainLocked());
    }

    private Entry holdRelocationIngress(
            byte[] record,
            Supplier<CompletionStage<Void>> operation,
            Runnable relocationRelease,
            Object message) {
        Entry entry =
                new Entry(
                        nextSequence++,
                        record,
                        operation,
                        relocationRelease,
                        new CompletableFuture<>(),
                        ZLinkFlowContext.current(),
                        null,
                        Lane.APPLICATION,
                        false);
        outstanding++;
        entry.message = message;
        holdRelocationEntry(entry);
        return entry;
    }

    private Entry holdRelocationIngress(
            Supplier<byte[]> record,
            Supplier<CompletionStage<Void>> operation,
            Runnable relocationRelease) {
        Entry entry =
                new Entry(
                        nextSequence++,
                        record,
                        operation,
                        relocationRelease,
                        new CompletableFuture<>(),
                        ZLinkFlowContext.current(),
                        null,
                        Lane.APPLICATION,
                        false);
        outstanding++;
        holdRelocationEntry(entry);
        return entry;
    }

    private void holdRelocationEntry(Entry entry) {
        if (relocation == null) {
            throw new IllegalStateException("relocation ingress requires an active seal");
        }
        if (relocation.acceptanceEpoch == Long.MAX_VALUE) {
            throw new IllegalStateException("relocation ingress epoch exhausted");
        }
        relocation.held.addLast(entry);
        relocation.acceptanceEpoch++;
    }

    private void release(Entry entry) {
        if (entry.applicationJobOwnership != null) {
            entry.applicationJobOwnership.close();
        }
    }

    private static void validatePayloadBytes(long payloadBytes) {
        if (payloadBytes < 0) {
            throw new IllegalArgumentException("payloadBytes must be non-negative");
        }
    }

    private boolean hasPending() {
        return !applicationPending.isEmpty()
                || !continuationPending.isEmpty()
                || !lifecyclePending.isEmpty();
    }

    private Entry takeNext() {
        if (suspendedLifecycle != null) {
            Entry ownerContinuation = null;
            Entry applicationContinuation = null;
            for (Entry candidate : continuationPending) {
                if (candidate.origin == suspendedLifecycle) {
                    if (ownerContinuation == null) {
                        ownerContinuation = candidate;
                    }
                } else if (applicationContinuation == null) {
                    applicationContinuation = candidate;
                }
                if (ownerContinuation != null && applicationContinuation != null) {
                    break;
                }
            }
            if (ownerContinuation != null
                    && (suspendedLifecycle.operation instanceof LifecycleTransition
                            || (applicationContinuation == null && applicationPending.isEmpty())
                            || lifecycleStreak < lifecycleBurstLimit)) {
                lifecycleStreak++;
                continuationPending.remove(ownerContinuation);
                return ownerContinuation;
            }
            if (suspendedLifecycle.operation instanceof LifecycleTransition) {
                return null;
            }
            if (applicationContinuation != null) {
                lifecycleStreak = 0;
                continuationPending.remove(applicationContinuation);
                return applicationContinuation;
            }
            if (!applicationPending.isEmpty()) {
                lifecycleStreak = 0;
                return applicationPending.removeFirst();
            }
            return null;
        }
        boolean lifecycleReady = !lifecyclePending.isEmpty();
        boolean applicationReady = !applicationPending.isEmpty();
        boolean continuationReady = !continuationPending.isEmpty();
        if (lifecycleReady
                && (lifecyclePending.peekFirst().relocationBoundary != null
                        || lifecyclePending.peekFirst().operation instanceof LifecycleTransition)) {
            if (continuationReady) {
                lifecycleStreak = 0;
                return continuationPending.removeFirst();
            }
            return lifecyclePending.removeFirst();
        }
        if (lifecycleReady && (!applicationReady || lifecycleStreak < lifecycleBurstLimit)) {
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

    private boolean requestDrainLocked() {
        if (drainScheduled || active != null || !hasPending()) {
            return false;
        }
        if (suspendedLifecycle != null
                && suspendedLifecycle.operation instanceof LifecycleTransition
                && continuationPending.stream()
                        .noneMatch(entry -> entry.origin == suspendedLifecycle)) {
            return false;
        }
        if (suspendedLifecycle != null
                && continuationPending.isEmpty()
                && applicationPending.isEmpty()) {
            return false;
        }
        if (!lifecyclePending.isEmpty()
                && lifecyclePending.peekFirst().relocationBoundary != null
                && suspendedContinuations() != 0
                && continuationPending.isEmpty()) {
            return false;
        }
        drainScheduled = true;
        return true;
    }

    private void scheduleDrainIfNeeded(EnqueueResult result) {
        scheduleDrainIfNeeded(result.scheduleDrain(), result.applicationJobOwnership());
    }

    private void scheduleDrainIfNeeded(
            boolean scheduleDrain, ZLinkApplicationJobContext.QueuedOwnership ownership) {
        if (ownership != null) ownership.markQueued();
        if (!scheduleDrain) {
            return;
        }
        try {
            DRAIN_EXECUTOR.execute(this::drainScheduled);
        } catch (RuntimeException rejected) {
            throw new IllegalStateException("serial queue drain executor rejected", rejected);
        }
    }

    private void drainScheduled() {
        try {
            executor.execute(
                    () -> {
                        Entry entry;
                        synchronized (this) {
                            drainScheduled = false;
                            entry = takeNextForDrainLocked();
                        }
                        if (entry != null) drainBatch(entry);
                    });
        } catch (RuntimeException rejected) {
            Entry entry;
            synchronized (this) {
                drainScheduled = false;
                entry = takeNextForDrainLocked();
            }
            if (entry != null) {
                entry.result.completeExceptionally(rejected);
                finish(entry, false);
            }
        }
    }

    private void drainBatch(Entry first) {
        Entry entry = first;
        while (entry != null) {
            CompletableFuture<Void> invocation = invokeInline(entry).toCompletableFuture();
            if (!invocation.isDone()) {
                Entry suspended = entry;
                invocation.whenComplete(
                        (ignored, error) -> {
                            if (suspended.lane == Lane.LIFECYCLE && !suspended.result.isDone()) {
                                suspendLifecycle(suspended);
                            } else {
                                finish(suspended, false);
                            }
                        });
                return;
            }
            if (entry.lane == Lane.LIFECYCLE && !entry.result.isDone()) {
                suspendLifecycle(entry);
                return;
            }
            entry = finish(entry, true);
        }
    }

    private void suspendLifecycle(Entry entry) {
        boolean scheduleDrain;
        synchronized (this) {
            if (active != entry || suspendedLifecycle != null) {
                throw new IllegalStateException("lifecycle suspension is inconsistent");
            }
            active = null;
            suspendedLifecycle = entry;
            scheduleDrain = requestDrainLocked();
        }
        entry.result.whenComplete((ignored, failure) -> finishSuspendedLifecycle(entry));
        scheduleDrainIfNeeded(scheduleDrain, null);
    }

    private void finishSuspendedLifecycle(Entry entry) {
        release(entry);
        boolean scheduleDrain;
        List<CompletableFuture<Void>> quiescent;
        synchronized (this) {
            if (suspendedLifecycle != entry) {
                return;
            }
            suspendedLifecycle = null;
            outstanding--;
            scheduleDrain = requestDrainLocked();
            quiescent = takeQuiescenceWaitersIfReady();
        }
        scheduleDrainIfNeeded(scheduleDrain, null);
        completeBoundary(entry);
        quiescent.forEach(waiter -> waiter.complete(null));
    }

    private Entry takeNextForDrainLocked() {
        if (active != null || !hasPending()) {
            return null;
        }
        if (!lifecyclePending.isEmpty()
                && lifecyclePending.peekFirst().relocationBoundary != null
                && suspendedContinuations() != 0
                && continuationPending.isEmpty()) {
            return null;
        }
        Entry entry = takeNext();
        if (entry == null) {
            return null;
        }
        active = entry;
        if (turnClaimedAtNanos == 0) {
            turnClaimedAtNanos = System.nanoTime();
        }
        return entry;
    }

    private Entry finish(Entry entry, boolean continueBatch) {
        release(entry);
        List<CompletableFuture<Void>> quiescent = List.of();
        boolean scheduleDrain = false;
        Entry next = null;
        synchronized (this) {
            if (active != entry) {
                return null;
            }
            active = null;
            outstanding--;
            boolean yieldToExecutor =
                    hasPending()
                            && ownerTimeBudgetNanos > 0
                            && System.nanoTime() - turnClaimedAtNanos >= ownerTimeBudgetNanos;
            if (yieldToExecutor) {
                turnClaimedAtNanos = 0;
            } else if (!hasPending()) {
                turnClaimedAtNanos = 0;
            }
            if (continueBatch && !yieldToExecutor) {
                next = takeNextForDrainLocked();
            } else {
                scheduleDrain = requestDrainLocked();
            }
            quiescent = takeQuiescenceWaitersIfReady();
        }
        scheduleDrainIfNeeded(scheduleDrain, null);
        completeBoundary(entry);
        quiescent.forEach(waiter -> waiter.complete(null));
        return next;
    }

    private static void completeBoundary(Entry entry) {
        if (entry.relocationBoundary != null) {
            entry.relocationBoundary.finished.complete(null);
        }
    }

    /** Which accepted work a quiescence waiter waits for. */
    public enum Quiescence {
        /** Every accepted turn and yielded continuation of both lanes. */
        ALL,
        /**
         * Every accepted application turn and its continuations. Lifecycle work, including the
         * lifecycle item that waits, is not part of this boundary.
         */
        APPLICATION
    }

    /**
     * Completes after every accepted turn and every yielded continuation has reached its terminal
     * boundary. The caller must seal external admission before using this as a lifecycle barrier.
     */
    public CompletionStage<Void> awaitQuiescence() {
        return awaitQuiescence(Quiescence.ALL);
    }

    /**
     * Completes when the accepted work that {@code scope} names has reached its terminal boundary.
     * The caller must seal external admission before using this as a lifecycle barrier.
     */
    public synchronized CompletionStage<Void> awaitQuiescence(Quiescence scope) {
        Objects.requireNonNull(scope, "scope");
        if (isQuiescent(scope)) {
            return CompletableFuture.completedFuture(null);
        }
        CompletableFuture<Void> waiter = new CompletableFuture<>();
        quiescenceWaiters.add(new QuiescenceWaiter(scope, waiter));
        return waiter;
    }

    public synchronized Optional<RelocationSeal> trySealRelocation() {
        return trySealRelocation((RelocationBoundary) null);
    }

    /**
     * Reserves the next turn boundary without running queued application records. The reservation
     * remains active until {@link RelocationBoundary#release()} is called.
     */
    public Optional<RelocationBoundary> reserveRelocationTurnBoundary() {
        ZLinkApplicationJobContext.QueuedOwnership ownership;
        Optional<RelocationBoundary> result;
        boolean scheduleDrain;
        synchronized (this) {
            if (relocated || relocation != null) {
                return Optional.empty();
            }
            if (nextSequence == Long.MAX_VALUE) {
                throw new IllegalStateException("queue sequence exhausted");
            }
            RelocationBoundary boundary = new RelocationBoundary(this);
            Entry entry =
                    new Entry(
                            nextSequence++,
                            (byte[]) null,
                            boundary::reach,
                            () -> {},
                            new CompletableFuture<>(),
                            ZLinkFlowContext.current(),
                            boundary,
                            Lane.LIFECYCLE,
                            false);
            boundary.entry = entry;
            outstanding++;
            lifecyclePending.addLast(entry);
            result = Optional.of(boundary);
            scheduleDrain = requestDrainLocked();
            ownership = entry.applicationJobOwnership;
        }
        scheduleDrainIfNeeded(scheduleDrain, ownership);
        return result;
    }

    /**
     * Seals this queue while the supplied lifecycle boundary owns its active turn. Only the exact
     * reservation instance can cross that boundary.
     */
    public synchronized Optional<RelocationSeal> trySealRelocation(RelocationBoundary boundary) {
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
     * Binds the exact turn that is active on the calling thread so a later asynchronous
     * continuation can seal this queue underneath it. A deferred Actor Join runs its complete
     * cross-node relocation while its mailbox barrier stays the active turn; reserving another
     * lifecycle boundary from inside that operation would queue the boundary behind the barrier
     * itself and never reach it.
     */
    public synchronized Optional<ActiveTurnSealHandle> captureActiveTurnSealHandle() {
        if (CURRENT.get() != this || active == null) {
            return Optional.empty();
        }
        return Optional.of(new ActiveTurnSealHandle(this, active));
    }

    /** Captures the queue that owns the calling thread's current lifecycle turn. */
    public static Optional<ActiveTurnSealHandle> captureCurrentActiveTurnSealHandle() {
        ZLinkSerialExecutionQueue current = CURRENT.get();
        return current == null ? Optional.empty() : current.captureActiveTurnSealHandle();
    }

    /** Reports whether the calling turn belongs to a timer queue of this Spot. */
    public static boolean isCurrentTimerOf(ZLinkSerialExecutionQueue owner) {
        ZLinkSerialExecutionQueue current = CURRENT.get();
        return current != null && current.timerOwner == owner;
    }

    /** Seals this queue while the captured turn is still the active turn. */
    public synchronized Optional<RelocationSeal> trySealRelocation(ActiveTurnSealHandle handle) {
        if (relocated || relocation != null) {
            return Optional.empty();
        }
        if (handle == null || handle.owner != this || handle.entry != active) {
            return Optional.empty();
        }
        return sealNowLocked();
    }

    public static final class ActiveTurnSealHandle {
        private final ZLinkSerialExecutionQueue owner;
        private final Entry entry;

        private ActiveTurnSealHandle(ZLinkSerialExecutionQueue owner, Entry entry) {
            this.owner = owner;
            this.entry = entry;
        }
    }

    private Optional<RelocationSeal> sealNowLocked() {
        if (suspendedContinuations() != 0
                || !continuationPending.isEmpty()
                || applicationPending.stream().anyMatch(entry -> !entry.hasRelocationRecord())) {
            return Optional.empty();
        }
        if (nextRelocationSerial == Long.MAX_VALUE) {
            throw new IllegalStateException("relocation serial exhausted");
        }
        ArrayDeque<Entry> captured = new ArrayDeque<>(applicationPending);
        applicationPending.clear();
        long serial = nextRelocationSerial++;
        RelocationSeal seal =
                new RelocationSeal(serial, captured.stream().map(Entry::queuedRecord).toList());
        relocation = new RelocationState(serial, seal, captured);
        return Optional.of(seal);
    }

    public boolean abortRelocation(RelocationSeal seal) {
        boolean scheduleDrain;
        synchronized (this) {
            if (!matches(seal) || relocation.retained != null) {
                return false;
            }
            ArrayDeque<Entry> restored = new ArrayDeque<>(relocation.captured);
            while (!restored.isEmpty()) {
                applicationPending.addLast(restored.removeFirst());
            }
            relocation.held.forEach(
                    entry -> {
                        if (entry.lane == Lane.LIFECYCLE) {
                            lifecyclePending.addLast(entry);
                        } else if (entry.continuation) {
                            continuationPending.addLast(entry);
                        } else {
                            applicationPending.addLast(entry);
                        }
                    });
            relocation = null;
            scheduleDrain = requestDrainLocked();
        }
        scheduleDrainIfNeeded(scheduleDrain, null);
        return true;
    }

    /** Captures the current ingress high-water before authority prepare. */
    public synchronized Optional<List<QueuedRecord>> freezeRelocationIngress(RelocationSeal seal) {
        if (!matches(seal) || relocation.frozen) {
            return Optional.empty();
        }
        relocation.frozen = true;
        return Optional.of(
                relocation.held.stream()
                        .filter(Entry::hasRelocationRecord)
                        .map(Entry::queuedRecord)
                        .toList());
    }

    public Optional<List<QueuedRecord>> commitRelocation(RelocationSeal seal) {
        Optional<RetainedCommit> retained = retainRelocationCommit(seal);
        if (retained.isEmpty()) {
            return Optional.empty();
        }
        RetainedCommit commit = retained.orElseThrow();
        if (!ZLinkRetainedSerialQueueCommit.capture(commit.records(), commit)) {
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
     * Detaches a committed relocation while retaining source resources until the target has durably
     * replayed the returned ingress records.
     */
    private synchronized Optional<RetainedCommit> retainRelocationCommit(RelocationSeal seal) {
        if (!matches(seal) || relocation.retained != null) {
            return Optional.empty();
        }
        List<QueuedRecord> held =
                relocation.held.stream()
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
            throw new IllegalStateException("serial queue relocation capture is not terminal");
        }
        for (Entry entry : commit.entries) {
            RuntimeException failure = null;
            try {
                entry.relocationRelease.run();
            } catch (RuntimeException error) {
                failure = error;
            } finally {
                release(entry);
                synchronized (this) {
                    outstanding--;
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

    private CompletionStage<Void> invokeInline(Entry entry) {
        CompletableFuture<Void> gate = new CompletableFuture<>();
        CompletableFuture<Void> invocationReturned = new CompletableFuture<>();
        boolean releaseGateOnIncompleteStage =
                lanePolicy.releasesGateOnIncompleteStage() && entry.relocationBoundary == null;
        ZLinkSerialExecutionQueue previous = CURRENT.get();
        CompletableFuture<Void> previousGate = CURRENT_GATE.get();
        Boolean previousDeferred = CURRENT_RELEASE_DEFERRED.get();
        CURRENT.set(this);
        CURRENT_GATE.set(gate);
        CURRENT_RELEASE_DEFERRED.set(false);
        try (var serial =
                        systems.zlink.framework.runtime.internal.handlers
                                .ZLinkSuspendInvocationContext.enterSerialExecutionTurn(
                                new SerialTurnCarrier(new SerialTurn(this, gate, entry)));
                ZLinkFlowContext.Scope ignored =
                        entry.flow == null ? () -> {} : ZLinkFlowContext.enter(entry.flow);
                ZLinkApplicationJobContext.Scope applicationJob =
                        ZLinkApplicationJobContext.enterQueued(entry.applicationJobOwnership)) {
            CompletionStage<Void> execution =
                    Objects.requireNonNull(entry.operation.get(), "operation result");
            // A relocation boundary is itself the turn a relocation seal owns; it stays active
            // until released instead of returning the turn.
            if (lanePolicy instanceof ZLinkExecutionLanePolicy.Spot
                    && entry.lane == Lane.LIFECYCLE
                    && entry.relocationBoundary == null
                    && !execution.toCompletableFuture().isDone()
                    && !gate.isDone()) {
                execution = yieldCurrent(execution);
            }
            execution.whenComplete(
                    (value, error) -> {
                        if (error != null) {
                            entry.result.completeExceptionally(error);
                        } else {
                            entry.result.complete(null);
                        }
                        if (!releaseGateOnIncompleteStage) {
                            gate.complete(null);
                        }
                    });
            if (releaseGateOnIncompleteStage
                    && !Boolean.TRUE.equals(CURRENT_RELEASE_DEFERRED.get())) {
                gate.complete(null);
            }
        } catch (RuntimeException | Error error) {
            entry.result.completeExceptionally(error);
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
            invocationReturned.complete(null);
        }
        // A Yield may release the logical turn while operation.get() is still
        // assembling dependent stages. Keep the physical drain entry until
        // that invocation has returned, otherwise a completed managed stage
        // can run a late dependent inline outside its continuation turn.
        return CompletableFuture.allOf(gate, invocationReturned);
    }

    public static <T> CompletionStage<T> manageCurrent(CompletionStage<T> stage) {
        Objects.requireNonNull(stage, "stage");
        SerialTurn turn = currentTurn();
        ZLinkSerialExecutionQueue queue = turn == null ? null : turn.queue;
        if (queue == null) {
            return stage;
        }
        ZLinkFlowContext.State flow = ZLinkFlowContext.current();
        var application =
                systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext
                        .currentApplicationExecution();
        String actorDispatch =
                systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext
                        .currentActorDispatch();
        Object serialContext =
                systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext
                        .currentSerialExecutionTurn();
        CompletableFuture<T> managed = new CompletableFuture<>();
        ZLinkCompletionBridge.forwardCancellation(managed, stage);
        if (!queue.lanePolicy.releasesGateOnIncompleteStage()) {
            CompletableFuture<Void> gate = turn.gate;
            stage.whenComplete(
                    (value, error) -> {
                        try {
                            queue.executor.execute(
                                    () -> {
                                        ZLinkSerialExecutionQueue previous = CURRENT.get();
                                        CompletableFuture<Void> previousGate = CURRENT_GATE.get();
                                        CURRENT.set(queue);
                                        if (gate == null) {
                                            CURRENT_GATE.remove();
                                        } else {
                                            CURRENT_GATE.set(gate);
                                        }
                                        try (var serial =
                                                        systems.zlink.framework.runtime.internal
                                                                .handlers
                                                                .ZLinkSuspendInvocationContext
                                                                .enterSerialExecutionTurn(
                                                                        serialContext);
                                                var actor =
                                                        systems.zlink.framework.runtime.internal
                                                                .handlers
                                                                .ZLinkSuspendInvocationContext
                                                                .enterActorDispatch(actorDispatch);
                                                var execution =
                                                        systems.zlink.framework.runtime.internal
                                                                .handlers
                                                                .ZLinkSuspendInvocationContext
                                                                .enterApplicationExecution(
                                                                        application);
                                                ZLinkFlowContext.Scope ignored =
                                                        flow == null
                                                                ? () -> {}
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
        queue.suspendContinuation(turn.entry);
        stage.whenComplete(
                (value, error) -> {
                    try {
                        CompletionStage<Void> continuation =
                                queue.enqueueContinuation(
                                        turn.entry,
                                        () -> {
                                            updateCarrier(serialContext, currentTurn());
                                            try (var serial =
                                                            systems.zlink.framework.runtime.internal
                                                                    .handlers
                                                                    .ZLinkSuspendInvocationContext
                                                                    .enterSerialExecutionTurn(
                                                                            serialContext);
                                                    var actor =
                                                            systems.zlink.framework.runtime.internal
                                                                    .handlers
                                                                    .ZLinkSuspendInvocationContext
                                                                    .enterActorDispatch(
                                                                            actorDispatch);
                                                    var execution =
                                                            systems.zlink.framework.runtime.internal
                                                                    .handlers
                                                                    .ZLinkSuspendInvocationContext
                                                                    .enterApplicationExecution(
                                                                            application);
                                                    ZLinkFlowContext.Scope ignored =
                                                            flow == null
                                                                    ? () -> {}
                                                                    : ZLinkFlowContext.enter(
                                                                            flow)) {
                                                if (error != null) {
                                                    managed.completeExceptionally(error);
                                                } else {
                                                    managed.complete(value);
                                                }
                                            }
                                            return CompletableFuture.completedFuture(null);
                                        });
                        continuation.whenComplete(
                                (ignored, continuationFailure) -> {
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

    /**
     * Returns a stage that completes when the turn running on the current serial queue ends by any
     * path. A turn that yielded ends with its last continuation.
     */
    public static CompletionStage<Void> currentTurnCompletion() {
        SerialTurn turn = currentTurn();
        if (turn == null || turn.entry == null) {
            throw new IllegalStateException("operation requires a serial handler turn");
        }
        return turn.entry.turnOrigin().result.handle((ignored, failure) -> null);
    }

    public static <T> CompletionStage<T> yieldCurrent(CompletionStage<T> stage) {
        Objects.requireNonNull(stage, "stage");
        SerialTurn turn = currentTurn();
        ZLinkSerialExecutionQueue queue = turn == null ? null : turn.queue;
        CompletableFuture<Void> gate = turn == null ? null : turn.gate;
        if (queue == null || gate == null || stage.toCompletableFuture().isDone()) {
            return stage;
        }
        ZLinkFlowContext.State flow = ZLinkFlowContext.current();
        var application =
                systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext
                        .currentApplicationExecution();
        String actorDispatch =
                systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext
                        .currentActorDispatch();
        Object serialContext =
                systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext
                        .currentSerialExecutionTurn();
        CompletableFuture<T> managed = new CompletableFuture<>();
        ZLinkCompletionBridge.forwardCancellation(managed, stage);
        queue.suspendContinuation(turn.entry);
        gate.complete(null);
        stage.whenComplete(
                (value, error) -> {
                    try {
                        CompletionStage<Void> continuation =
                                queue.enqueueContinuation(
                                        turn.entry,
                                        () -> {
                                            updateCarrier(serialContext, currentTurn());
                                            try (var serial =
                                                            systems.zlink.framework.runtime.internal
                                                                    .handlers
                                                                    .ZLinkSuspendInvocationContext
                                                                    .enterSerialExecutionTurn(
                                                                            serialContext);
                                                    var actor =
                                                            systems.zlink.framework.runtime.internal
                                                                    .handlers
                                                                    .ZLinkSuspendInvocationContext
                                                                    .enterActorDispatch(
                                                                            actorDispatch);
                                                    var execution =
                                                            systems.zlink.framework.runtime.internal
                                                                    .handlers
                                                                    .ZLinkSuspendInvocationContext
                                                                    .enterApplicationExecution(
                                                                            application);
                                                    ZLinkFlowContext.Scope ignored =
                                                            flow == null
                                                                    ? () -> {}
                                                                    : ZLinkFlowContext.enter(
                                                                            flow)) {
                                                if (error != null) {
                                                    managed.completeExceptionally(error);
                                                } else {
                                                    managed.complete(value);
                                                }
                                            }
                                            return CompletableFuture.completedFuture(null);
                                        });
                        continuation.whenComplete(
                                (ignored, continuationFailure) -> {
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

    private synchronized void suspendContinuation(Entry turn) {
        if (suspendedContinuations() == Integer.MAX_VALUE) {
            throw new IllegalStateException("suspended continuation count exhausted");
        }
        if (isLifecycleTurn(turn)) {
            suspendedLifecycleContinuations++;
        } else {
            suspendedApplicationContinuations++;
        }
    }

    private int suspendedContinuations() {
        return suspendedApplicationContinuations + suspendedLifecycleContinuations;
    }

    private static boolean isLifecycleTurn(Entry turn) {
        return turn != null && turn.turnOrigin().lane == Lane.LIFECYCLE;
    }

    private CompletionStage<Void> enqueueContinuation(
            Entry origin, Supplier<CompletionStage<Void>> operation) {
        ZLinkApplicationJobContext.QueuedOwnership ownership;
        CompletionStage<Void> result;
        boolean scheduleDrain = false;
        synchronized (this) {
            if (isLifecycleTurn(origin)) {
                if (suspendedLifecycleContinuations <= 0) {
                    throw new IllegalStateException("suspended continuation count is inconsistent");
                }
                suspendedLifecycleContinuations--;
            } else {
                if (suspendedApplicationContinuations <= 0) {
                    throw new IllegalStateException("suspended continuation count is inconsistent");
                }
                suspendedApplicationContinuations--;
            }
            if (nextSequence == Long.MAX_VALUE) {
                throw new IllegalStateException("queue sequence exhausted");
            }
            Entry continuation =
                    new Entry(
                            nextSequence++,
                            (byte[]) null,
                            operation,
                            () -> {},
                            new CompletableFuture<>(),
                            ZLinkFlowContext.current(),
                            null,
                            Lane.APPLICATION,
                            true);
            continuation.origin = origin == null ? null : origin.turnOrigin();
            outstanding++;
            if (relocation != null) {
                holdRelocationEntry(continuation);
            } else {
                continuationPending.addLast(continuation);
                scheduleDrain = requestDrainLocked();
            }
            result = continuation.result;
            ownership = continuation.applicationJobOwnership;
        }
        scheduleDrainIfNeeded(scheduleDrain, ownership);
        return result;
    }

    private boolean isQuiescent(Quiescence scope) {
        boolean relocationDrained =
                relocation == null || (relocation.captured.isEmpty() && relocation.held.isEmpty());
        if (scope == Quiescence.ALL) {
            return outstanding == 0
                    && suspendedContinuations() == 0
                    && active == null
                    && !hasPending()
                    && relocationDrained;
        }
        return applicationPending.isEmpty()
                && suspendedApplicationContinuations == 0
                && (active == null || isLifecycleTurn(active))
                && continuationPending.stream().allMatch(ZLinkSerialExecutionQueue::isLifecycleTurn)
                && relocationDrained;
    }

    private List<CompletableFuture<Void>> takeQuiescenceWaitersIfReady() {
        if (quiescenceWaiters.isEmpty()) {
            return List.of();
        }
        List<CompletableFuture<Void>> ready = new ArrayList<>();
        quiescenceWaiters.removeIf(
                waiter -> {
                    if (!isQuiescent(waiter.scope())) {
                        return false;
                    }
                    ready.add(waiter.completion());
                    return true;
                });
        return ready;
    }

    private void completeQuiescenceWaitersIfReady() {
        List<CompletableFuture<Void>> ready = takeQuiescenceWaitersIfReady();
        ready.forEach(waiter -> waiter.complete(null));
    }

    public static Executor propagateCurrent(Executor executor) {
        Objects.requireNonNull(executor, "executor");
        return command -> {
            ZLinkSerialExecutionQueue queue = CURRENT.get();
            CompletableFuture<Void> gate = CURRENT_GATE.get();
            Boolean deferred = CURRENT_RELEASE_DEFERRED.get();
            Object serialTurn =
                    systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext
                            .currentSerialExecutionTurn();
            var application =
                    systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext
                            .currentApplicationExecution();
            String actorDispatch =
                    systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext
                            .currentActorDispatch();
            executor.execute(
                    () ->
                            runWithContext(
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
            ZLinkSerialExecutionQueue queue,
            CompletableFuture<Void> gate,
            Boolean deferred,
            Object serialTurn,
            systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext
                            .ApplicationExecution
                    application,
            String actorDispatch,
            Runnable command) {
        ZLinkSerialExecutionQueue previous = CURRENT.get();
        CompletableFuture<Void> previousGate = CURRENT_GATE.get();
        Boolean previousDeferred = CURRENT_RELEASE_DEFERRED.get();
        setOrRemove(CURRENT, queue);
        setOrRemove(CURRENT_GATE, gate);
        setOrRemove(CURRENT_RELEASE_DEFERRED, deferred);
        try (var serial =
                        systems.zlink.framework.runtime.internal.handlers
                                .ZLinkSuspendInvocationContext.enterSerialExecutionTurn(
                                serialTurn);
                var actor =
                        systems.zlink.framework.runtime.internal.handlers
                                .ZLinkSuspendInvocationContext.enterActorDispatch(actorDispatch);
                var execution =
                        systems.zlink.framework.runtime.internal.handlers
                                .ZLinkSuspendInvocationContext.enterApplicationExecution(
                                application)) {
            SerialTurn turn = currentTurn();
            try (var job =
                    ZLinkApplicationJobContext.enterQueued(
                            turn == null || turn.entry == null
                                    ? null
                                    : turn.entry.applicationJobOwnership)) {
                command.run();
            }
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
        ZLinkSerialExecutionQueue queue = CURRENT.get();
        CompletableFuture<Void> gate = CURRENT_GATE.get();
        Object propagated =
                systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext
                        .currentSerialExecutionTurn();
        SerialTurn turn;
        if (queue != null && gate != null) {
            if (propagated instanceof SerialTurnCarrier carrier
                    && carrier.turn.queue == queue
                    && carrier.turn.gate == gate) {
                turn = carrier.turn;
            } else {
                turn = gate.isDone() ? null : new SerialTurn(queue, gate, null);
            }
        } else {
            turn = propagated instanceof SerialTurnCarrier carrier ? carrier.turn : null;
        }
        return turn != null && turn.entry != null && turn.entry.turnOrigin().result.isDone()
                ? null
                : turn;
    }

    private static void updateCarrier(Object context, SerialTurn turn) {
        if (context instanceof SerialTurnCarrier carrier && turn != null) {
            carrier.turn = turn;
        }
    }

    private record QuiescenceWaiter(Quiescence scope, CompletableFuture<Void> completion) {}

    private enum Lane {
        APPLICATION,
        LIFECYCLE
    }

    private record SerialTurn(
            ZLinkSerialExecutionQueue queue, CompletableFuture<Void> gate, Entry entry) {}

    private static final class SerialTurnCarrier {
        private volatile SerialTurn turn;

        private SerialTurnCarrier(SerialTurn turn) {
            this.turn = turn;
        }
    }

    private static final class RetainedCommit implements ZLinkRetainedSerialQueueCommit.Owner {
        private final ZLinkSerialExecutionQueue owner;
        private final List<QueuedRecord> records;
        private List<Entry> entries;
        private final AtomicBoolean completed = new AtomicBoolean();

        private RetainedCommit(ZLinkSerialExecutionQueue owner, List<QueuedRecord> records) {
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
                if (owner.relocation == null || owner.relocation.retained != this) {
                    throw new IllegalStateException(
                            "serial queue retained relocation is not active");
                }
                List<QueuedRecord> current =
                        owner.relocation.held.stream()
                                .filter(Entry::hasRelocationRecord)
                                .map(Entry::queuedRecord)
                                .toList();
                return ZLinkRetainedSerialQueueCommit.cut(
                        this, owner.relocation.acceptanceEpoch, current);
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
                    throw new IllegalStateException("serial queue durable cut is not established");
                }
                List<Entry> released =
                        new ArrayList<>(
                                owner.relocation.captured.size() + owner.relocation.held.size());
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
                        && (owner.relocation != null && owner.relocation.retained == this
                                || owner.relocation == null && owner.relocated && entries != null);
            }
        }

        @Override
        public void abort() {
            boolean scheduleDrain;
            synchronized (owner) {
                if (!canAbort()) {
                    throw new IllegalStateException(
                            "serial queue retained relocation cannot be restored");
                }
                List<Entry> restored;
                if (owner.relocation != null) {
                    restored =
                            new ArrayList<>(
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
                scheduleDrain = owner.requestDrainLocked();
            }
            owner.scheduleDrainIfNeeded(scheduleDrain, null);
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
                throw new IllegalArgumentException("queue record sequence must be positive");
            }
            payload = Objects.requireNonNull(payload, "payload").clone();
        }

        @Override
        public byte[] payload() {
            return payload.clone();
        }
    }

    public record RelocationSeal(long serial, List<QueuedRecord> captured) {
        public RelocationSeal {
            if (serial <= 0) {
                throw new IllegalArgumentException("relocation seal serial must be positive");
            }
            captured = List.copyOf(captured);
        }
    }

    public static final class RelocationBoundary {
        private final ZLinkSerialExecutionQueue owner;
        private final CompletableFuture<Void> reached = new CompletableFuture<>();
        private final CompletableFuture<Void> released = new CompletableFuture<>();
        private final CompletableFuture<Void> finished = new CompletableFuture<>();
        private Entry entry;

        private RelocationBoundary(ZLinkSerialExecutionQueue owner) {
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
        private Supplier<CompletionStage<Void>> operation;
        private Object message;
        private final Runnable relocationRelease;
        private final CompletableFuture<Void> result;
        private final ZLinkFlowContext.State flow;
        private final ZLinkApplicationJobContext.QueuedOwnership applicationJobOwnership;
        private final RelocationBoundary relocationBoundary;
        private final Lane lane;
        private final boolean continuation;
        // The first entry of the turn this continuation resumes; null for that first entry.
        private Entry origin;

        private Entry turnOrigin() {
            return origin == null ? this : origin;
        }

        private Entry(
                long sequence,
                byte[] record,
                Supplier<CompletionStage<Void>> operation,
                Runnable relocationRelease,
                CompletableFuture<Void> result,
                ZLinkFlowContext.State flow,
                RelocationBoundary relocationBoundary,
                Lane lane,
                boolean continuation) {
            this.sequence = sequence;
            this.record = record;
            this.lazyRecord = null;
            this.operation = operation;
            this.relocationRelease = relocationRelease;
            this.result = result;
            this.flow = flow;
            this.applicationJobOwnership = ZLinkApplicationJobContext.transferQueuedOwnership();
            this.relocationBoundary = relocationBoundary;
            this.lane = lane;
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
                boolean continuation) {
            this.sequence = sequence;
            this.record = null;
            this.lazyRecord = Objects.requireNonNull(lazyRecord, "lazyRecord");
            this.operation = operation;
            this.relocationRelease = relocationRelease;
            this.result = result;
            this.flow = flow;
            this.applicationJobOwnership = ZLinkApplicationJobContext.transferQueuedOwnership();
            this.relocationBoundary = relocationBoundary;
            this.lane = lane;
            this.continuation = continuation;
        }

        private boolean hasRelocationRecord() {
            return record != null || lazyRecord != null;
        }

        private synchronized QueuedRecord queuedRecord() {
            if (record == null) {
                record =
                        Objects.requireNonNull(
                                lazyRecord.get(), "relocation record supplier returned null");
            }
            return new QueuedRecord(sequence, record);
        }
    }

    private record EnqueueResult(
            CompletionStage<Void> result,
            boolean scheduleDrain,
            ZLinkApplicationJobContext.QueuedOwnership applicationJobOwnership) {
        EnqueueResult(Entry entry, boolean scheduleDrain) {
            this(entry.result, scheduleDrain, entry.applicationJobOwnership);
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

        private RelocationState(long serial, RelocationSeal seal, ArrayDeque<Entry> captured) {
            this.serial = serial;
            this.seal = seal;
            this.captured = captured;
        }
    }
}
