/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.messaging;

import java.time.Duration;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;

/** Common stage for builders that set a timeout and then submit. */
public interface TimeoutSubmitOperation<TResult, TCallback> {
    TimeoutSubmitOperation<TResult, TCallback> timeout(Duration timeout);

    CompletionStage<TResult> submit();

    boolean submit(TCallback callback);

    /**
     * Submits and blocks the current thread until the result completes.
     *
     * <p>Intended for virtual threads: when a virtual thread blocks, the
     * JVM parks it and frees the carrier (platform) thread for other work,
     * so this does not waste a thread the way blocking a platform thread
     * would. The framework's own async path uses {@link #submit()}; this
     * is a convenience for sample and virtual-thread code that reads more
     * naturally as a sequential call.
     *
     * @return the completed result
     */
    default TResult await() {
        try {
            return submit().toCompletableFuture().join();
        } catch (CompletionException e) {
            Throwable cause = e.getCause();
            if (cause instanceof RuntimeException re) {
                throw re;
            }
            if (cause instanceof Error err) {
                throw err;
            }
            throw e;
        }
    }
}
