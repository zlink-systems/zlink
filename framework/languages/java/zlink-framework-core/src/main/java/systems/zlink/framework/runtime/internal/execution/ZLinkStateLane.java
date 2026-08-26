package systems.zlink.framework.runtime.internal.execution;

import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.Executor;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.Supplier;
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue;

/**
 * Single-owner execution lane for a component's mutable state.
 *
 * <p>Every read and write of a component's state runs through one lane turn. The lane runs at
 * most one turn at a time, so its state can use ordinary, unsynchronized collections. This is
 * intentionally separate from {@link ZLinkAsyncSerialQueue}, which owns application execution,
 * relocation, and lifecycle admission.</p>
 *
 * <p>A lane is not reentrant. Reentering it from one of its turns would wait behind that turn and
 * hang, so {@link #runAsync(Supplier)} throws at the reentrant call site instead. Java
 * {@link ThreadLocal} values do not automatically flow through arbitrary {@link CompletionStage}
 * continuations; work that schedules an asynchronous continuation on an executor must use
 * {@link #propagateCurrent(Executor)} to retain this diagnostic ownership marker.</p>
 */
public final class ZLinkStateLane {
    private static final int DRAIN_BATCH_LIMIT = 100;
    private static final ThreadLocal<ZLinkStateLane> CURRENT = new ThreadLocal<>();

    private final ConcurrentLinkedQueue<WorkItem> mailbox = new ConcurrentLinkedQueue<>();
    private final AtomicInteger scheduled = new AtomicInteger();
    private final AtomicInteger closed = new AtomicInteger();
    private final CompletableFuture<Void> completed = new CompletableFuture<>();
    private final ExecutorService ownedExecutor;
    private final Executor executor;

    public ZLinkStateLane() {
        ownedExecutor = Executors.newVirtualThreadPerTaskExecutor();
        executor = ownedExecutor;
    }

    public ZLinkStateLane(Executor executor) {
        this.executor = Objects.requireNonNull(executor, "executor");
        ownedExecutor = null;
    }

    public static ZLinkStateLane current() {
        return CURRENT.get();
    }

    public boolean isOnLane() {
        return CURRENT.get() == this;
    }

    public <T> CompletionStage<T> runAsync(Supplier<T> work) {
        Objects.requireNonNull(work, "work");
        throwIfReentrant();
        throwIfClosed();

        CompletableFuture<T> result = new CompletableFuture<>();
        mailbox.add(() -> {
            try {
                T value = work.get();
                // CompletableFuture's non-async dependents run on the completing thread. Complete
                // this future asynchronously so caller continuations never inherit the lane's
                // ThreadLocal ownership marker.
                result.completeAsync(() -> value);
            } catch (RuntimeException | Error error) {
                result.completeAsync(() -> {
                    throw error;
                });
            }
            return CompletableFuture.completedFuture(null);
        });
        scheduleDrain();
        return result;
    }

    public CompletionStage<Void> runAsync(Runnable work) {
        Objects.requireNonNull(work, "work");
        return runAsync(() -> {
            work.run();
            return null;
        });
    }

    public boolean tryPost(Supplier<? extends CompletionStage<Void>> work) {
        Objects.requireNonNull(work, "work");
        if (closed.get() != 0) {
            return false;
        }

        mailbox.add(() -> {
            try {
                return Objects.requireNonNull(work.get(), "work result");
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
                "This code already runs on the state lane it is trying to enter. Call the "
                    + "component's private state method directly instead of re-entering its "
                    + "public surface.");
        }
    }

    public CompletionStage<Void> closeAsync() {
        if (closed.compareAndSet(0, 1)) {
            if (scheduled.get() == 0 && mailbox.isEmpty()) {
                completed.complete(null);
                closeOwnedExecutor();
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
                closeOwnedExecutor();
            }
            return;
        }

        CompletionStage<Void> execution;
        try {
            execution = callWithCurrent(this, work::run);
        } catch (RuntimeException | Error error) {
            execution = CompletableFuture.failedFuture(error);
        }
        execution.whenComplete((ignored, error) -> {
            try {
                executor.execute(() -> runNext(processed + 1));
            } catch (RuntimeException rejected) {
                scheduled.set(0);
                if (closed.get() != 0) {
                    completed.completeExceptionally(rejected);
                    closeOwnedExecutor();
                }
            }
        });
    }

    private void closeOwnedExecutor() {
        if (ownedExecutor != null) {
            ownedExecutor.shutdown();
        }
    }

    private static void runWithCurrent(ZLinkStateLane lane, Runnable command) {
        callWithCurrent(lane, () -> {
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
