package systems.zlink.framework.runtime.internal.execution;

import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.internal.ZLinkCompletionBridge;
import systems.zlink.framework.runtime.internal.calls.ZLinkBlockingCalls;

import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.Executor;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicInteger;
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

    private final ConcurrentLinkedQueue<WorkItem> mailbox = new ConcurrentLinkedQueue<>();
    private final AtomicInteger scheduled = new AtomicInteger();
    // The sign bit seals admission; the remaining bits count submissions still publishing a turn.
    private final AtomicInteger admission = new AtomicInteger();
    private final CompletableFuture<Void> completed = new CompletableFuture<>();
    private final Executor executor;

    public ZLinkStateLane() {
        this(DEFAULT_EXECUTOR);
    }

    public ZLinkStateLane(Executor executor) {
        this.executor = Objects.requireNonNull(executor, "executor");
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
        return ZLinkCompletionBridge.publishOutsideOwner(submit(work, false));
    }

    public <T> CompletionStage<T> runNowOrQueue(Supplier<T> work) {
        return ZLinkCompletionBridge.publishOutsideOwner(submit(work, true));
    }

    /** Synchronous registration boundary; its private result cannot acquire dependents. */
    public <T> T awaitTurn(Supplier<T> work) {
        Objects.requireNonNull(work, "work");
        throwIfReentrant();
        if (!tryAdmit()) throw new IllegalStateException("state lane is closed");
        CompletableFuture<T> result;
        try {
            if (tryClaimInlineTurn()) {
                try {
                    return callWithCurrent(this, work);
                } catch (CompletionException failure) {
                    // Preserve the synchronous boundary's existing failure unwrapping.
                    return ZLinkCompletionBridge.await(CompletableFuture.failedFuture(failure));
                } finally {
                    releaseInlineTurn();
                }
            }
            result = new CompletableFuture<>();
            mailbox.add(completeWork(work, result));
            scheduleDrain();
        } finally {
            releaseAdmission();
        }
        return ZLinkCompletionBridge.await(result);
    }

    private <T> CompletableFuture<T> submit(Supplier<T> work, boolean runIdleTurnNow) {
        Objects.requireNonNull(work, "work");
        throwIfReentrant();
        if (!tryAdmit()) throw new IllegalStateException("state lane is closed");
        CompletableFuture<T> result;
        try {
            result = new CompletableFuture<>();
            WorkItem turn = completeWork(work, result);
            if (runIdleTurnNow && tryRunInline(turn)) {
                return result;
            }
            mailbox.add(turn);
            scheduleDrain();
        } finally {
            releaseAdmission();
        }
        return result;
    }

    /** Returns null when the owner is busy; a returned stage has already run its turn. */
    public <T> CompletionStage<T> tryRunNow(Supplier<T> work) {
        Objects.requireNonNull(work, "work");
        throwIfReentrant();
        if (!tryAdmit()) throw new IllegalStateException("state lane is closed");
        try {
            CompletableFuture<T> result = new CompletableFuture<>();
            return tryRunInline(completeWork(work, result)) ? result : null;
        } finally {
            releaseAdmission();
        }
    }

    private <T> WorkItem completeWork(Supplier<T> work, CompletableFuture<T> result) {
        return () ->
                callWithCurrent(
                        this,
                        () -> {
                            try {
                                result.complete(work.get());
                            } catch (RuntimeException | Error error) {
                                result.completeExceptionally(
                                        error instanceof CompletionException
                                                ? error
                                                : new CompletionException(error));
                            }
                            return TURN_FINISHED;
                        });
    }

    private boolean tryRunInline(WorkItem turn) {
        if (!tryClaimInlineTurn()) return false;
        try {
            turn.run();
            return true;
        } finally {
            releaseInlineTurn();
        }
    }

    private boolean tryClaimInlineTurn() {
        if (!scheduled.compareAndSet(0, 1)) return false;
        if (!mailbox.isEmpty()) {
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
        scheduled.set(0);
        if (!mailbox.isEmpty()) {
            scheduleDrain();
        }
        completeCloseIfDrained();
    }

    public boolean tryPost(Supplier<? extends CompletionStage<Void>> work) {
        Objects.requireNonNull(work, "work");
        if (!tryAdmit()) return false;
        try {
            mailbox.add(
                    () -> {
                        try {
                            return callWithCurrent(
                                    this, () -> Objects.requireNonNull(work.get(), "work result"));
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
        admission.getAndUpdate(state -> state | CLOSED);
        completeCloseIfDrained();
        return completed;
    }

    public static Executor propagateCurrent(Executor executor) {
        Objects.requireNonNull(executor, "executor");
        return command -> {
            ZLinkStateLane lane = CURRENT.get();
            executor.execute(() -> runWithCurrent(lane, command));
        };
    }

    private boolean tryAdmit() {
        int state = admission.get();
        while (state >= 0) {
            if (admission.compareAndSet(state, state + 1)) return true;
            state = admission.get();
        }
        return false;
    }

    private void releaseAdmission() {
        admission.decrementAndGet();
        completeCloseIfDrained();
    }

    private void completeCloseIfDrained() {
        if (admission.get() == CLOSED && scheduled.get() == 0 && mailbox.isEmpty()) {
            completed.complete(null);
        }
    }

    private void scheduleDrain() {
        if (scheduled.compareAndSet(0, 1)) {
            try {
                executor.execute(this::drain);
            } catch (RuntimeException rejected) {
                scheduled.set(0);
                throw rejected;
            }
        }
    }

    private void drain() {
        runNext(0);
    }

    private void runNext(int processed) {
        while (processed < DRAIN_BATCH_LIMIT) {
            WorkItem work = mailbox.poll();
            if (work == null) break;
            CompletionStage<Void> execution;
            try {
                execution = work.run();
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
        scheduled.set(0);
        if (!mailbox.isEmpty()) scheduleDrain();
        completeCloseIfDrained();
    }

    private void resumeDrain(int processed) {
        try {
            executor.execute(() -> runNext(processed));
        } catch (RuntimeException rejected) {
            scheduled.set(0);
            if (admission.get() < 0) completed.completeExceptionally(rejected);
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

    @FunctionalInterface
    private interface WorkItem {
        CompletionStage<Void> run();
    }
}
