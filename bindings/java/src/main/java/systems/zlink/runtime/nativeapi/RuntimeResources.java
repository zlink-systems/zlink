/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.nativeapi;

import java.lang.foreign.Arena;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;

public final class RuntimeResources {
    private RuntimeResources() {}

    public static void closeArena(Arena arena) {
        if (arena != null && arena.scope().isAlive()) {
            arena.close();
        }
    }

    public static ExecutorService daemonSingleThreadExecutor(
        String threadName) {
        return Executors.newSingleThreadExecutor(runnable -> {
            Thread thread = new Thread(runnable, threadName);
            thread.setDaemon(true);
            return thread;
        });
    }

    public static void shutdownExecutor(ExecutorService executor) {
        if (executor != null) {
            executor.shutdown();
        }
    }

    public static void shutdownExecutor(ExecutorService executor,
                                        long timeout,
                                        TimeUnit unit) {
        if (executor == null) {
            return;
        }
        executor.shutdown();
        try {
            executor.awaitTermination(timeout, unit);
        } catch (InterruptedException ex) {
            Thread.currentThread().interrupt();
        }
    }
}
