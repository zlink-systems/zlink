package systems.zlink.framework.execution;

import java.util.ArrayDeque;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executor;
import java.util.function.Supplier;

/**
 * Serial execution line for a single Spot or Entry Spot.
 *
 * <p>Two kinds of operations are accepted.
 *
 * <ul>
 *   <li><b>Dispatch operations</b> ({@link #enqueue}) carry handler callbacks such as
 *       actor packets, route packets, timers, and lifecycle callbacks. They are
 *       non-reentrant: the next dispatch operation does not start until the
 *       {@link CompletionStage} returned by the previous one completes, even when the
 *       handler finishes asynchronously.</li>
 *   <li><b>Continuation operations</b> ({@link #enqueueContinuation}) carry framework
 *       completions that re-enter the Spot line, such as request replies and worker
 *       results. They are serialized with every other operation, but they are allowed
 *       to run while a dispatch operation is gated on its asynchronous completion;
 *       otherwise a handler awaiting its own request reply could never complete.</li>
 * </ul>
 *
 * <p>At most one operation slice executes at a time. For a dispatch operation the
 * slice is its synchronous part (until it returns its stage); for a continuation the
 * slice spans its full stage. Continuation operations always run on the configured
 * executor so backend callback threads never execute user code directly.
 */
public final class ZLinkSpotDispatchQueue {
    private final Executor continuationExecutor;
    private final Object lock = new Object();
    private final ArrayDeque<QueuedOperation> dispatchOperations = new ArrayDeque<>();
    private final ArrayDeque<QueuedOperation> continuationOperations = new ArrayDeque<>();
    private boolean sliceRunning;
    private boolean gated;
    private boolean pumping;

    public ZLinkSpotDispatchQueue(Executor continuationExecutor) {
        this.continuationExecutor =
            Objects.requireNonNull(continuationExecutor, "continuationExecutor");
    }

    /**
     * Enqueues a non-reentrant dispatch operation. The returned stage completes with
     * the outcome of the operation's stage.
     */
    public CompletionStage<Void> enqueue(Supplier<CompletionStage<Void>> operation) {
        return submit(new QueuedOperation(
            Objects.requireNonNull(operation, "operation"),
            true));
    }

    /**
     * Enqueues a continuation operation that may pass the non-reentrancy gate of a
     * pending dispatch operation. The operation runs on the continuation executor.
     */
    public CompletionStage<Void> enqueueContinuation(Supplier<CompletionStage<Void>> operation) {
        return submit(new QueuedOperation(
            Objects.requireNonNull(operation, "operation"),
            false));
    }

    private CompletionStage<Void> submit(QueuedOperation queued) {
        synchronized (lock) {
            if (queued.dispatch) {
                dispatchOperations.add(queued);
            } else {
                continuationOperations.add(queued);
            }
        }
        pump();
        return queued.completion;
    }

    private void pump() {
        synchronized (lock) {
            if (pumping) {
                return;
            }
            pumping = true;
        }
        while (true) {
            QueuedOperation next;
            synchronized (lock) {
                next = pickNext();
                if (next == null) {
                    pumping = false;
                    return;
                }
                sliceRunning = true;
                if (next.dispatch) {
                    gated = true;
                }
            }
            if (next.dispatch) {
                runDispatch(next);
            } else {
                scheduleContinuation(next);
            }
        }
    }

    private QueuedOperation pickNext() {
        if (sliceRunning) {
            return null;
        }
        QueuedOperation continuation = continuationOperations.poll();
        if (continuation != null) {
            return continuation;
        }
        if (!gated) {
            return dispatchOperations.poll();
        }
        return null;
    }

    private void runDispatch(QueuedOperation operation) {
        CompletionStage<Void> stage = invoke(operation);
        synchronized (lock) {
            sliceRunning = false;
        }
        stage.whenComplete((value, error) -> {
            synchronized (lock) {
                gated = false;
            }
            complete(operation, value, error);
            pump();
        });
    }

    private void scheduleContinuation(QueuedOperation operation) {
        try {
            continuationExecutor.execute(() -> {
                CompletionStage<Void> stage = invoke(operation);
                stage.whenComplete((value, error) -> {
                    synchronized (lock) {
                        sliceRunning = false;
                    }
                    complete(operation, value, error);
                    pump();
                });
            });
        } catch (RuntimeException ex) {
            synchronized (lock) {
                sliceRunning = false;
            }
            operation.completion.completeExceptionally(ex);
        }
    }

    private static CompletionStage<Void> invoke(QueuedOperation operation) {
        try {
            return Objects.requireNonNull(operation.operation.get(), "operation result");
        } catch (RuntimeException ex) {
            return CompletableFuture.failedFuture(ex);
        }
    }

    private static void complete(QueuedOperation operation, Void value, Throwable error) {
        if (error != null) {
            operation.completion.completeExceptionally(error);
        } else {
            operation.completion.complete(value);
        }
    }

    private static final class QueuedOperation {
        final Supplier<CompletionStage<Void>> operation;
        final boolean dispatch;
        final CompletableFuture<Void> completion = new CompletableFuture<>();

        QueuedOperation(Supplier<CompletionStage<Void>> operation, boolean dispatch) {
            this.operation = operation;
            this.dispatch = dispatch;
        }
    }
}
