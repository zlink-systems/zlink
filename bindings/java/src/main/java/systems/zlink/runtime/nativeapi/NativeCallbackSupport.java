/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.nativeapi;

import java.util.concurrent.ExecutorService;
import java.util.concurrent.TimeUnit;

public final class NativeCallbackSupport implements AutoCloseable {
    private final String executorName;
    private volatile ExecutorService executor;
    private volatile RuntimeException failure;

    public NativeCallbackSupport(String executorName) {
        this.executorName = executorName;
    }

    public ExecutorService executor() {
        return executor;
    }

    public ExecutorLease ensureExecutor() {
        ExecutorService current = executor;
        if (current != null) {
            return new ExecutorLease(current, false);
        }
        current = RuntimeResources.daemonSingleThreadExecutor(executorName);
        executor = current;
        return new ExecutorLease(current, true);
    }

    public ExecutorService replaceExecutor() {
        ExecutorService next = RuntimeResources.daemonSingleThreadExecutor(
            executorName);
        executor = next;
        return next;
    }

    public void restoreExecutor(ExecutorService previous,
                                ExecutorService failed) {
        if (executor == failed) {
            executor = previous;
        }
        RuntimeResources.shutdownExecutor(failed);
    }

    public void clearExecutorIfCreated(ExecutorLease lease) {
        if (!lease.created()) {
            return;
        }
        if (executor == lease.executor()) {
            executor = null;
        }
        RuntimeResources.shutdownExecutor(lease.executor());
    }

    public void ensureNoFailure() {
        RuntimeException currentFailure = failure;
        if (currentFailure != null) {
            throw currentFailure;
        }
    }

    public void recordFailure(RuntimeException nextFailure) {
        failure = nextFailure;
        Thread current = Thread.currentThread();
        Thread.UncaughtExceptionHandler uncaught =
            current.getUncaughtExceptionHandler();
        if (uncaught != null) {
            uncaught.uncaughtException(current, nextFailure);
        }
    }

    @Override
    public void close() {
        failure = null;
        RuntimeResources.shutdownExecutor(executor);
        executor = null;
    }

    public void close(long timeout, TimeUnit unit) {
        failure = null;
        RuntimeResources.shutdownExecutor(executor, timeout, unit);
        executor = null;
    }

    public record ExecutorLease(ExecutorService executor, boolean created) {}
}
