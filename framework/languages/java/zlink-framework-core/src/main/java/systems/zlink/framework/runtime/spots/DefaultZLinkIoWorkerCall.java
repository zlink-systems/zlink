package systems.zlink.framework.runtime.spots;

import java.time.Duration;
import java.util.Objects;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.errors.ZLinkWorkerFailedException;
import systems.zlink.framework.errors.ZLinkWorkerTimeoutException;
import systems.zlink.framework.execution.ZLinkWorkerPool;
import systems.zlink.framework.spots.ZLinkIoWorkerTask;
import systems.zlink.framework.spots.ZLinkWorkerCall;

final class DefaultZLinkIoWorkerCall<T> implements ZLinkWorkerCall<T> {
    private final ZLinkWorkerPool pool;
    private final ZLinkIoWorkerTask<T> work;
    private final AtomicBoolean terminated = new AtomicBoolean();
    private Duration timeout;

    DefaultZLinkIoWorkerCall(ZLinkWorkerPool pool, ZLinkIoWorkerTask<T> work) {
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
        if (!terminated.compareAndSet(false, true)) {
            throw new IllegalStateException(
                "runIoWorker call already has a terminator; call one terminator once");
        }
        CompletableFuture<T> result = new CompletableFuture<>();
        AtomicBoolean settled = new AtomicBoolean();
        DefaultZLinkWorkerCancellation cancellation =
            new DefaultZLinkWorkerCancellation();
        ScheduledFuture<?> timeoutFuture = timeout == null ? null : pool.scheduleTimeout(() -> {
            if (settled.compareAndSet(false, true)) {
                cancellation.cancel();
                result.completeExceptionally(new ZLinkWorkerTimeoutException(
                "I/O worker call timed out after " + timeout));
            }
        }, timeout);
        AtomicReference<Runnable> unregisterShutdown =
            new AtomicReference<>(() -> { });
        result.whenComplete((ignored, error) -> {
            if (result.isCancelled() && settled.compareAndSet(false, true)) {
                cancellation.cancel();
                if (timeoutFuture != null) {
                    timeoutFuture.cancel(false);
                }
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
            CompletionStage<T> stage = Objects.requireNonNull(
                work.run(cancellation),
                "I/O worker result");
            stage.whenComplete((value, error) -> {
                if (!settled.compareAndSet(false, true)) {
                    return;
                }
                if (timeoutFuture != null) {
                    timeoutFuture.cancel(false);
                }
                if (error instanceof CancellationException
                    && cancellation.isCancellationRequested()) {
                    result.cancel(false);
                } else if (error != null) {
                    result.completeExceptionally(
                        new ZLinkWorkerFailedException("I/O worker call failed", error));
                } else {
                    result.complete(value);
                }
            });
        } catch (CancellationException error) {
            if (settled.compareAndSet(false, true)) {
                if (timeoutFuture != null) {
                    timeoutFuture.cancel(false);
                }
                result.cancel(false);
            }
        } catch (Exception error) {
            if (settled.compareAndSet(false, true)) {
                if (timeoutFuture != null) {
                    timeoutFuture.cancel(false);
                }
                result.completeExceptionally(
                    new ZLinkWorkerFailedException("I/O worker call failed", error));
            }
        }
        return systems.zlink.framework.execution.ZLinkAsyncSerialQueue.manageCurrent(result);
    }

    @Override
    public CompletionStage<T> yield() {
        systems.zlink.framework.runtime.internal.handlers
            .ZLinkSuspendInvocationContext.requireYieldAllowed("I/O worker");
        return systems.zlink.framework.execution.ZLinkAsyncSerialQueue
            .yieldCurrent(submit());
    }
}
