package systems.zlink.framework.runtime.internal.execution;

import java.util.concurrent.Executor;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.function.Consumer;

/** Keeps inline and CallerRuns executors off the submitting infrastructure turn. */
public final class ZLinkExecutionSubmission {
    private static final ExecutorService DRAIN_EXECUTOR =
            Executors.newVirtualThreadPerTaskExecutor();

    private ZLinkExecutionSubmission() {}

    public static void submit(Executor executor, Runnable work, Consumer<RuntimeException> reject) {
        if (executor instanceof ExecutorService
                && !(executor instanceof ThreadPoolExecutor pool
                        && !(pool.getRejectedExecutionHandler()
                                instanceof ThreadPoolExecutor.AbortPolicy))) {
            execute(executor, work, reject);
        } else {
            try {
                DRAIN_EXECUTOR.execute(() -> execute(executor, work, reject));
            } catch (RuntimeException rejected) {
                reject.accept(rejected);
            }
        }
    }

    private static void execute(
            Executor executor, Runnable work, Consumer<RuntimeException> reject) {
        try {
            executor.execute(work);
        } catch (RuntimeException rejected) {
            reject.accept(rejected);
        }
    }
}
