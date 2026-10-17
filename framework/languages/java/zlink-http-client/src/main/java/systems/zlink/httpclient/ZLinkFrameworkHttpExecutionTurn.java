/* SPDX-License-Identifier: Apache-2.0 */
package systems.zlink.httpclient;

import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext;

import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executor;
import java.util.function.Supplier;

public final class ZLinkFrameworkHttpExecutionTurn implements ZLinkHttpExecutionTurn {
    @Override
    public <T> CompletionStage<T> async(CompletionStage<T> operation) {
        return ZLinkSerialExecutionQueue.manageCurrent(operation);
    }

    @Override
    public <T> CompletionStage<T> yield(Supplier<CompletionStage<T>> operation) {
        Objects.requireNonNull(operation, "operation");
        ZLinkSuspendInvocationContext.requireYieldAllowed("HTTP request");
        CompletionStage<T> pending;
        try {
            pending = operation.get();
        } catch (Throwable error) {
            pending = CompletableFuture.failedFuture(error);
        }
        return ZLinkSerialExecutionQueue.yieldCurrent(pending);
    }

    @Override
    public Executor callbackScheduler() {
        return ZLinkSerialExecutionQueue.captureCurrentCallbackExecutor();
    }
}
