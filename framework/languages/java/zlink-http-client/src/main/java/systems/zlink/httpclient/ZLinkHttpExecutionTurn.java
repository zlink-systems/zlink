/* SPDX-License-Identifier: Apache-2.0 */
package systems.zlink.httpclient;

import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executor;
import java.util.function.Supplier;

public interface ZLinkHttpExecutionTurn {
    <T> CompletionStage<T> async(CompletionStage<T> operation);

    /**
     * Admits Yield before invoking the operation factory. Rejection must not submit the request.
     */
    <T> CompletionStage<T> yield(Supplier<CompletionStage<T>> operation);

    /**
     * Captures the scheduler for callback completion in a new turn on the current execution line.
     */
    Executor callbackScheduler();
}
