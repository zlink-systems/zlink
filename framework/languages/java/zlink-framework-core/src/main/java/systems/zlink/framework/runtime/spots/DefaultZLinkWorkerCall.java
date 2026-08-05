package systems.zlink.framework.runtime.spots;

import java.time.Duration;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.errors.ZLinkWorkerFailedException;
import systems.zlink.framework.errors.ZLinkWorkerQueueFullException;
import systems.zlink.framework.errors.ZLinkWorkerTimeoutException;
import systems.zlink.framework.execution.ZLinkWorkerPool;
import systems.zlink.framework.spots.ZLinkWorkerCall;
import systems.zlink.framework.spots.ZLinkWorkerTask;

/**
 * Runtime {@code runWorker(...)} call. The work runs on the shared elastic worker
 * pool; the awaitable terminator completes the returned stage, and the callback
 * terminator posts both callbacks to the owning Spot serial line. A late result
 * after a timeout is dropped without invoking user callbacks.
 */
final class DefaultZLinkWorkerCall<T> implements ZLinkWorkerCall<T> {
    private final ZLinkWorkerPool pool;
    private final ZLinkWorkerTask<T> work;
    private final AtomicBoolean terminated = new AtomicBoolean();
    private Duration timeout;

    DefaultZLinkWorkerCall(
        ZLinkWorkerPool pool,
        ZLinkWorkerTask<T> work) {
        this.pool = Objects.requireNonNull(pool, "pool");
        this.work = Objects.requireNonNull(work, "work");
    }

    @Override
    public ZLinkWorkerCall<T> timeout(Duration timeout) {
        if (timeout == null || timeout.isNegative() || timeout.isZero()) {
            throw new ZLinkConfigurationException("worker timeout must be positive");
        }
        this.timeout = timeout;
        return this;
    }

    @Override
    public CompletionStage<T> submit() {
        ensureSingleTerminator();
        CompletableFuture<T> result = new CompletableFuture<>();
        start(result);
        return systems.zlink.framework.execution.ZLinkAsyncSerialQueue.manageCurrent(result);
    }

    @Override
    public CompletionStage<T> yield() {
        systems.zlink.framework.runtime.internal.handlers
            .ZLinkSuspendInvocationContext.requireYieldAllowed("CPU worker");
        return systems.zlink.framework.execution.ZLinkAsyncSerialQueue
            .yieldCurrent(submit());
    }

    private void start(CompletableFuture<T> result) {
        AtomicBoolean settled = new AtomicBoolean();
        DefaultZLinkWorkerCancellation cancellation =
            new DefaultZLinkWorkerCancellation();
        ScheduledFuture<?> timeoutFuture = timeout == null
            ? null
            : pool.scheduleTimeout(() -> {
                if (settled.compareAndSet(false, true)) {
                    cancellation.cancel();
                    result.completeExceptionally(new ZLinkWorkerTimeoutException(
                        "worker call timed out after " + timeout));
                }
            }, timeout);
        AtomicReference<Runnable> unregisterShutdown =
            new AtomicReference<>(() -> { });
        result.whenComplete((ignored, error) -> {
            if (result.isCancelled() && settled.compareAndSet(false, true)) {
                cancellation.cancel();
                cancelTimeout(timeoutFuture);
            }
            unregisterShutdown.get().run();
        });
        unregisterShutdown.set(pool.registerShutdownListener(() -> {
            result.cancel(false);
            cancellation.cancel();
        }));
        if (result.isDone()) {
            unregisterShutdown.get().run();
        }
        try {
            pool.execute(() -> {
                T value;
                try {
                    value = work.run(cancellation);
                } catch (CancellationException ex) {
                    if (settled.compareAndSet(false, true)) {
                        cancelTimeout(timeoutFuture);
                        result.cancel(false);
                    }
                    return;
                } catch (Exception ex) {
                    if (settled.compareAndSet(false, true)) {
                        cancelTimeout(timeoutFuture);
                        result.completeExceptionally(
                            new ZLinkWorkerFailedException("worker call failed", ex));
                    }
                    return;
                }
                // Late completion after a timeout: the settle flag is already
                // taken, so the result is dropped without user callbacks.
                if (settled.compareAndSet(false, true)) {
                    cancelTimeout(timeoutFuture);
                    result.complete(value);
                }
            });
        } catch (RejectedExecutionException ex) {
            cancellation.cancel();
            cancelTimeout(timeoutFuture);
            if (settled.compareAndSet(false, true)) {
                result.completeExceptionally(
                    new ZLinkWorkerQueueFullException("worker queue is full"));
            }
        }
    }

    private void ensureSingleTerminator() {
        if (!terminated.compareAndSet(false, true)) {
            throw new IllegalStateException(
                "runWorker call already has a terminator; call submit once");
        }
    }

    private static void cancelTimeout(ScheduledFuture<?> timeoutFuture) {
        if (timeoutFuture != null) {
            timeoutFuture.cancel(false);
        }
    }
}
