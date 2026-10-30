package systems.zlink.framework.runtime.internal.execution;

import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.internal.ZLinkCompletionBridge;
import systems.zlink.framework.runtime.internal.calls.ZLinkBlockingCalls;

import java.lang.invoke.MethodHandles;
import java.lang.invoke.VarHandle;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.Executor;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.function.BiConsumer;
import java.util.function.Supplier;

/**
 * Single-owner execution lane for a component's mutable state.
 *
 * <p>Every read and write of a component's state runs through one lane turn. The lane runs at most
 * one turn at a time, so its state can use ordinary, unsynchronized collections. This is
 * intentionally separate from {@link ZLinkSerialExecutionQueue}, which owns application execution,
 * relocation, and lifecycle admission.
 *
 * <p>A lane is not reentrant. Reentering it from one of its turns would wait behind that turn and
 * hang, so {@link #runAsync(Supplier)} throws at the reentrant call site instead. Java {@link
 * ThreadLocal} values do not automatically flow through arbitrary {@link CompletionStage}
 * continuations; work that schedules an asynchronous continuation on an executor must use {@link
 * #propagateCurrent(Executor)} to retain this diagnostic ownership marker.
 */
public final class ZLinkStateLane {
    private static final int DRAIN_BATCH_LIMIT = 100;
    private static final int CLOSED = Integer.MIN_VALUE;
    // A private mailbox signal, never returned as an operation's completion result.
    private static final CompletableFuture<Void> TURN_FINISHED =
            CompletableFuture.completedFuture(null);
    private static final ThreadLocal<ZLinkStateLane> CURRENT = new ThreadLocal<>();
    // Lane identity and FIFO belong to the mailbox, not to an executor.
    // In particular, a request-scoped handler owner needs no private executor.
    private static final ExecutorService DEFAULT_EXECUTOR =
            Executors.newVirtualThreadPerTaskExecutor();

    private static final VarHandle MAILBOX;
    private static final VarHandle SCHEDULED;
    private static final VarHandle ADMISSION;
    private static final VarHandle COMPLETED;

    static {
        try {
            var lookup = MethodHandles.lookup();
            MAILBOX =
                    lookup.findVarHandle(
                            ZLinkStateLane.class, "mailbox", ConcurrentLinkedQueue.class);
            SCHEDULED = lookup.findVarHandle(ZLinkStateLane.class, "scheduled", int.class);
            ADMISSION = lookup.findVarHandle(ZLinkStateLane.class, "admission", int.class);
            COMPLETED =
                    lookup.findVarHandle(
                            ZLinkStateLane.class, "completed", CompletableFuture.class);
        } catch (ReflectiveOperationException error) {
            throw new ExceptionInInitializerError(error);
        }
    }

    private volatile ConcurrentLinkedQueue<WorkItem> mailbox;
    private volatile int scheduled;
    // The sign bit seals admission; the remaining bits count submissions still publishing a turn.
    private volatile int admission;
    private volatile CompletableFuture<Void> completed;
    private final Executor executor;

    public ZLinkStateLane() {
        this(DEFAULT_EXECUTOR);
    }

    public ZLinkStateLane(Executor executor) {
        this(executor, null);
    }

    ZLinkStateLane(ConcurrentLinkedQueue<WorkItem> mailbox) {
        this(DEFAULT_EXECUTOR, mailbox);
    }

    ZLinkStateLane(Executor executor, ConcurrentLinkedQueue<WorkItem> mailbox) {
        this.executor = Objects.requireNonNull(executor, "executor");
        this.mailbox = mailbox;
    }

    int queuedTurnCountForTest() {
        var queue = mailbox;
        return queue == null ? 0 : queue.size();
    }

    boolean sharesExecutorWith(ZLinkStateLane other) {
        return executor == other.executor;
    }

    public static ZLinkStateLane current() {
        return CURRENT.get();
    }

    /** Assertion-only check for a synchronous wait on infrastructure or serial execution. */
    public static boolean assertMayBlock() {
        ZLinkBlockingCalls.requireMayBlock();
        return true;
    }

    public boolean isOnLane() {
        return CURRENT.get() == this;
    }

    public <T> CompletionStage<T> runAsync(Supplier<T> work) {
        return publishOutsideLane(submit(work, false));
    }

    public <T> CompletionStage<T> runNowOrQueue(Supplier<T> work) {
        return publishOutsideLane(submit(work, true));
    }

    /** Synchronous registration boundary; the caller executes its own turn. */
    public <T> T awaitTurn(Supplier<T> work) {
        Objects.requireNonNull(work, "work");
        throwIfReentrant();
        if (!tryAdmit()) throw new IllegalStateException("state lane is closed");
        WaitingTurn turn;
        try {
            if (tryClaimInlineTurn()) return runSynchronousTurn(work);
            turn = new WaitingTurn();
            mailbox().add(turn);
            scheduleDrain();
        } finally {
            releaseAdmission();
        }
        ZLinkCompletionBridge.await(turn);
        return runSynchronousTurn(work);
    }

    private <T> T runSynchronousTurn(Supplier<T> work) {
        try {
            return callWithCurrent(this, work);
        } catch (CompletionException failure) {
            // Preserve the synchronous boundary's existing failure unwrapping.
            return ZLinkCompletionBridge.await(CompletableFuture.failedFuture(failure));
        } finally {
            releaseInlineTurn();
        }
    }

    private <T> CompletableFuture<T> submit(Supplier<T> work, boolean runIdleTurnNow) {
        Objects.requireNonNull(work, "work");
        throwIfReentrant();
        if (!tryAdmit()) throw new IllegalStateException("state lane is closed");
        CompletableFuture<T> result;
        try {
            if (runIdleTurnNow && (result = tryRunInline(work)) != null) {
                return result;
            }
            var queued = new ResultWork<>(work);
            result = queued;
            mailbox().add(queued);
            scheduleDrain();
        } finally {
            releaseAdmission();
        }
        return result;
    }

    private static <T> CompletionStage<T> publishOutsideLane(CompletableFuture<T> result) {
        // No caller can observe the private result before submission returns.
        // A completed turn needs no completion task; a pending turn must still
        // publish outside the lane so a dependent can reenter it and wait.
        if (result.isDone()) {
            return result;
        }
        // Observers may hold a source gate while waiting for this completion. Keep its
        // publication on infrastructure execution, independent of the observation dispatcher.
        return result.handleAsync((value, error) -> result.join(), DEFAULT_EXECUTOR);
    }

    /** Registers the publication itself so a raced terminal cannot inline it in an owner turn. */
    public static <T> void publishOutsideLane(
            CompletableFuture<T> result, BiConsumer<? super T, ? super Throwable> publication) {
        if (result.isDone() && CURRENT.get() == null) result.whenComplete(publication);
        else result.whenCompleteAsync(publication, DEFAULT_EXECUTOR);
    }

    /** Returns null when the owner is busy; a returned stage has already run its turn. */
    public <T> CompletionStage<T> tryRunNow(Supplier<T> work) {
        Objects.requireNonNull(work, "work");
        throwIfReentrant();
        if (!tryAdmit()) throw new IllegalStateException("state lane is closed");
        try {
            return tryRunInline(work);
        } finally {
            releaseAdmission();
        }
    }

    private static final class ResultWork<T> extends CompletableFuture<T> implements WorkItem {
        private Supplier<T> work;

        private ResultWork(Supplier<T> work) {
            this.work = work;
        }

        @Override
        public CompletionStage<Void> run(ZLinkStateLane owner) {
            Supplier<T> invocation = work;
            // The completed result must not retain the invocation or its payload captures.
            work = null;
            completeWork(owner, invocation, this);
            return TURN_FINISHED;
        }
    }

    private static <T> void completeWork(
            ZLinkStateLane owner, Supplier<T> work, CompletableFuture<T> result) {
        try {
            result.complete(callWithCurrent(owner, work));
        } catch (RuntimeException | Error error) {
            result.completeExceptionally(
                    error instanceof CompletionException ? error : new CompletionException(error));
        }
    }

    private <T> CompletableFuture<T> tryRunInline(Supplier<T> work) {
        if (!tryClaimInlineTurn()) return null;
        try {
            var result = new CompletableFuture<T>();
            completeWork(this, work, result);
            return result;
        } finally {
            releaseInlineTurn();
        }
    }

    private boolean tryClaimInlineTurn() {
        if (!SCHEDULED.compareAndSet(this, 0, 1)) return false;
        if (!mailboxEmpty()) {
            releaseInlineTurn();
            return false;
        }
        return true;
    }

    public CompletionStage<Void> runAsync(Runnable work) {
        Objects.requireNonNull(work, "work");
        return runAsync(
                () -> {
                    work.run();
                    return null;
                });
    }

    private void releaseInlineTurn() {
        scheduled = 0;
        if (!mailboxEmpty()) {
            scheduleDrain();
            return;
        }
        completeCloseIfDrained();
    }

    public boolean tryPost(Supplier<? extends CompletionStage<Void>> work) {
        Objects.requireNonNull(work, "work");
        if (!tryAdmit()) return false;
        try {
            mailbox()
                    .add(
                            owner -> {
                                try {
                                    return callWithCurrent(
                                            owner,
                                            () ->
                                                    Objects.requireNonNull(
                                                            work.get(), "work result"));
                                } catch (RuntimeException | Error error) {
                                    return CompletableFuture.failedFuture(error);
                                }
                            });
            scheduleDrain();
            return true;
        } finally {
            releaseAdmission();
        }
    }

    public void throwIfReentrant() {
        if (isOnLane()) {
            throw new IllegalStateException(
                    "This code already runs on the state lane it is trying to enter. Call the"
                            + " component's private state method directly instead of re-entering its"
                            + " public surface.");
        }
    }

    public CompletionStage<Void> closeAsync() {
        CompletableFuture<Void> completion = completed;
        if (completion == null) {
            completion = new CompletableFuture<>();
            if (!COMPLETED.compareAndSet(this, null, completion)) completion = completed;
        }
        ADMISSION.getAndBitwiseOr(this, CLOSED);
        completeCloseIfDrained();
        return completion;
    }

    public static Executor propagateCurrent(Executor executor) {
        Objects.requireNonNull(executor, "executor");
        return command -> {
            ZLinkStateLane lane = CURRENT.get();
            executor.execute(() -> runWithCurrent(lane, command));
        };
    }

    private boolean tryAdmit() {
        int state = admission;
        while (state >= 0) {
            if (ADMISSION.compareAndSet(this, state, state + 1)) return true;
            state = admission;
        }
        return false;
    }

    private void releaseAdmission() {
        ADMISSION.getAndAdd(this, -1);
        completeCloseIfDrained();
    }

    private void completeCloseIfDrained() {
        CompletableFuture<Void> completion = completed;
        if (completion != null && admission == CLOSED && scheduled == 0 && mailboxEmpty()) {
            completion.complete(null);
        }
    }

    private ConcurrentLinkedQueue<WorkItem> mailbox() {
        ConcurrentLinkedQueue<WorkItem> queue = mailbox;
        if (queue == null) {
            queue = new ConcurrentLinkedQueue<>();
            if (!MAILBOX.compareAndSet(this, null, queue)) queue = mailbox;
        }
        return queue;
    }

    private boolean mailboxEmpty() {
        ConcurrentLinkedQueue<WorkItem> queue = mailbox;
        return queue == null || queue.isEmpty();
    }

    private void scheduleDrain() {
        if (SCHEDULED.compareAndSet(this, 0, 1)) {
            try {
                dispatchNext(0);
            } catch (RuntimeException rejected) {
                scheduled = 0;
                throw rejected;
            }
        }
    }

    private void dispatchNext(int processed) {
        if (grantWaitingTurn(mailbox().peek())) return;
        executor.execute(() -> runNext(processed));
    }

    // Only the scheduled owner examines/removes the head. Completing the private grant
    // transfers that same claim to the waiting caller; never touch it after completion.
    private boolean grantWaitingTurn(WorkItem work) {
        if (work instanceof WaitingTurn turn) {
            mailbox().poll();
            turn.complete(null);
            return true;
        }
        return false;
    }

    private void runNext(int processed) {
        while (processed < DRAIN_BATCH_LIMIT) {
            WorkItem work = mailbox().peek();
            if (work == null) break;
            if (grantWaitingTurn(work)) return;
            mailbox().poll();
            CompletionStage<Void> execution;
            try {
                execution = work.run(this);
            } catch (RuntimeException | Error error) {
                execution = CompletableFuture.failedFuture(error);
            }
            processed++;
            if (!execution.toCompletableFuture().isDone()) {
                int next = processed;
                execution.whenComplete((ignored, error) -> resumeDrain(next));
                return;
            }
        }
        releaseInlineTurn();
    }

    private void resumeDrain(int processed) {
        try {
            dispatchNext(processed);
        } catch (RuntimeException rejected) {
            scheduled = 0;
            if (admission < 0) completed.completeExceptionally(rejected);
        }
    }

    private static void runWithCurrent(ZLinkStateLane lane, Runnable command) {
        callWithCurrent(
                lane,
                () -> {
                    command.run();
                    return null;
                });
    }

    private static <T> T callWithCurrent(ZLinkStateLane lane, Supplier<T> command) {
        ZLinkStateLane previous = CURRENT.get();
        CURRENT.set(lane);
        try {
            return command.get();
        } finally {
            // Keep the thread's diagnostic slot, without retaining an inactive owner.
            CURRENT.set(previous);
        }
    }

    private static final class WaitingTurn extends CompletableFuture<Void> implements WorkItem {
        // grantWaitingTurn consumes this item before the asynchronous execution path.
        @Override
        public CompletionStage<Void> run(ZLinkStateLane owner) {
            return this;
        }
    }

    @FunctionalInterface
    interface WorkItem {
        CompletionStage<Void> run(ZLinkStateLane owner);
    }
}
