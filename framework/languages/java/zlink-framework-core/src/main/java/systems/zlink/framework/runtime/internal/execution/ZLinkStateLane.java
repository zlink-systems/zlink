package systems.zlink.framework.runtime.internal.execution;

import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext;

import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.Executor;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.Function;
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
    private static final ThreadLocal<ZLinkStateLane> CURRENT = new ThreadLocal<>();
    // Lane identity and FIFO belong to the mailbox, not to an executor.
    // In particular, a request-scoped handler owner needs no private executor.
    private static final ExecutorService DEFAULT_EXECUTOR =
            Executors.newVirtualThreadPerTaskExecutor();

    private final ConcurrentLinkedQueue<WorkItem> mailbox = new ConcurrentLinkedQueue<>();
    private final AtomicInteger scheduled = new AtomicInteger();
    private final AtomicInteger closed = new AtomicInteger();
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
        if (CURRENT.get() != null) {
            throw new AssertionError("synchronous wait from a state lane");
        }
        if (systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext
                        .currentSerialExecutionTurn()
                != null) {
            throw new AssertionError("synchronous wait from a serial turn");
        }
        String thread = Thread.currentThread().getName();
        if (thread.equals("zlink-stream-recv") || thread.equals("zlink-java-channel-runtime")) {
            throw new AssertionError("synchronous wait from a receive thread");
        }
        return true;
    }

    public boolean isOnLane() {
        return CURRENT.get() == this;
    }

    public <T> CompletionStage<T> runAsync(Supplier<T> work) {
        return submit(work, false);
    }

    /**
     * Runs one turn for a caller that waits for its result. An idle lane runs the turn on the
     * caller, so the caller does not wait; a busy lane queues it like {@link #runAsync}.
     */
    public <T> CompletionStage<T> runNowOrQueue(Supplier<T> work) {
        return submit(work, true);
    }

    private <T> CompletionStage<T> submit(Supplier<T> work, boolean runIdleTurnNow) {
        Objects.requireNonNull(work, "work");
        throwIfReentrant();
        throwIfClosed();

        CompletableFuture<T> result = new CompletableFuture<>();
        WorkItem turn =
                () -> {
                    try {
                        result.complete(callWithCurrent(this, work));
                    } catch (RuntimeException | Error error) {
                        result.completeExceptionally(
                                error instanceof CompletionException
                                        ? error
                                        : new CompletionException(error));
                    }
                    return CompletableFuture.completedFuture(null);
                };
        // The scheduled claim keeps an inline turn exclusive and behind queued turns.
        if (runIdleTurnNow && scheduled.compareAndSet(0, 1)) {
            try {
                if (mailbox.isEmpty() && closed.get() == 0) {
                    turn.run();
                    return result;
                }
            } finally {
                releaseInlineTurn();
            }
        }
        mailbox.add(turn);
        scheduleDrain();
        // No caller can observe the private result before submission returns.
        // A completed turn needs no completion task; a pending turn must still
        // publish outside the lane so a dependent can reenter it and wait.
        if (result.isDone()) {
            return result;
        }
        return result.handleAsync((value, error) -> result.join());
    }

    /**
     * Admits one queue entry in one lane turn without making the caller wait for the lane.
     *
     * <p>{@code owner} runs first and chooses the owning queue from the lane's state. {@code
     * admission} then enqueues into that queue with the caller's flow and application-job
     * reservation, so the entry is the one the calling thread would have created. The lane's FIFO
     * keeps the callers' submission order. A reservation that no queue took is returned when the
     * turn ends.
     */
    public <S, T> CompletionStage<T> admitAsync(
            Supplier<S> owner, Function<S, ? extends CompletionStage<T>> admission) {
        Objects.requireNonNull(owner, "owner");
        Objects.requireNonNull(admission, "admission");
        ZLinkFlowContext.State flow = ZLinkFlowContext.current();
        ZLinkApplicationJobContext.QueuedOwnership reservation =
                ZLinkApplicationJobContext.transferToQueuedJob();
        CompletionStage<CompletionStage<T>> admitted;
        try {
            admitted =
                    runAsync(
                            () -> {
                                S selected = owner.get();
                                try (ZLinkFlowContext.Scope ignoredFlow =
                                                ZLinkFlowContext.enter(flow);
                                        ZLinkApplicationJobContext.Scope ignoredJob =
                                                ZLinkApplicationJobContext.enterQueued(
                                                        reservation)) {
                                    return admission.apply(selected);
                                }
                            });
        } catch (RuntimeException | Error rejected) {
            closeUnqueued(reservation);
            throw rejected;
        }
        return admitted.whenComplete((ignored, error) -> closeUnqueued(reservation))
                .thenCompose(Function.identity());
    }

    private static void closeUnqueued(ZLinkApplicationJobContext.QueuedOwnership reservation) {
        if (reservation != null) {
            // A no-op once a queue entry took the reservation.
            reservation.close();
        }
    }

    public CompletionStage<Void> runAsync(Runnable work) {
        Objects.requireNonNull(work, "work");
        return runAsync(
                () -> {
                    work.run();
                    return null;
                });
    }

    public boolean tryPost(Supplier<? extends CompletionStage<Void>> work) {
        Objects.requireNonNull(work, "work");
        if (closed.get() != 0) {
            return false;
        }

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
        if (closed.compareAndSet(0, 1)) {
            if (scheduled.get() == 0 && mailbox.isEmpty()) {
                completed.complete(null);
            } else {
                scheduleDrain();
            }
        }
        return completed;
    }

    public static Executor propagateCurrent(Executor executor) {
        Objects.requireNonNull(executor, "executor");
        return command -> {
            ZLinkStateLane lane = CURRENT.get();
            executor.execute(() -> runWithCurrent(lane, command));
        };
    }

    private void throwIfClosed() {
        if (closed.get() != 0) {
            throw new IllegalStateException("state lane is closed");
        }
    }

    private void releaseInlineTurn() {
        scheduled.set(0);
        if (!mailbox.isEmpty()) {
            scheduleDrain();
        } else if (closed.get() != 0) {
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
        WorkItem work = processed == DRAIN_BATCH_LIMIT ? null : mailbox.poll();
        if (work == null) {
            scheduled.set(0);
            if (!mailbox.isEmpty()) {
                scheduleDrain();
            } else if (closed.get() != 0) {
                completed.complete(null);
            }
            return;
        }

        CompletionStage<Void> execution;
        try {
            execution = work.run();
        } catch (RuntimeException | Error error) {
            execution = CompletableFuture.failedFuture(error);
        }
        execution.whenComplete(
                (ignored, error) -> {
                    try {
                        executor.execute(() -> runNext(processed + 1));
                    } catch (RuntimeException rejected) {
                        scheduled.set(0);
                        if (closed.get() != 0) {
                            completed.completeExceptionally(rejected);
                        }
                    }
                });
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
        if (lane == null) {
            CURRENT.remove();
        } else {
            CURRENT.set(lane);
        }
        try {
            return command.get();
        } finally {
            if (previous == null) {
                CURRENT.remove();
            } else {
                CURRENT.set(previous);
            }
        }
    }

    @FunctionalInterface
    private interface WorkItem {
        CompletionStage<Void> run();
    }
}
