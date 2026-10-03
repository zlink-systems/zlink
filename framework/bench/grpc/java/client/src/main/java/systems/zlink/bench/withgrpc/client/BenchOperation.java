/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.client;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;

/** One request or one send, in whichever stack the cell is measuring. */
@FunctionalInterface
public interface BenchOperation {
    CompletableFuture<Void> invoke(int payloadSize, byte phase, long sequence);

    /**
     * Waits until {@code completion} is done or {@code timeout} passes, and returns whether it is
     * done. A failed completion throws its {@link ExecutionException}. The stack that owns
     * the completion decides how it makes progress while waiting; the default relies on the
     * completion's own threads.
     */
    default boolean await(CompletableFuture<Void> completion, Duration timeout)
        throws InterruptedException, ExecutionException {
        try {
            completion.get(timeout.toNanos(), TimeUnit.NANOSECONDS);
            return true;
        } catch (TimeoutException notDone) {
            return false;
        }
    }
}
