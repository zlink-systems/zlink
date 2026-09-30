package systems.zlink.framework.runtime.internal.calls;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext;

import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.function.Supplier;

/** Application-thread blocking terminals over the existing submission and completion owners. */
public final class ZLinkBlockingCalls {
    private static final ThreadLocal<Boolean> INFRASTRUCTURE_THREAD = new ThreadLocal<>();

    private ZLinkBlockingCalls() {}

    /** Marks a dedicated infrastructure thread once, before its executor starts running work. */
    public static void markInfrastructureThread() {
        INFRASTRUCTURE_THREAD.set(true);
    }

    public static Runnable infrastructureTask(Runnable task) {
        return () -> {
            markInfrastructureThread();
            task.run();
        };
    }

    public static <T> T submit(Supplier<? extends CompletionStage<T>> submission) {
        // Check before invoking the supplier: even claiming the call's single-use gate
        // would make a rejected runtime-context invocation observable to a later caller.
        requireMayBlock();
        try {
            return submission.get().toCompletableFuture().join();
        } catch (CompletionException failure) {
            Throwable cause = failure;
            while (cause instanceof CompletionException && cause.getCause() != null) {
                cause = cause.getCause();
            }
            if (cause instanceof RuntimeException runtime) {
                throw runtime;
            }
            if (cause instanceof Error error) {
                throw error;
            }
            throw failure;
        }
    }

    public static void requireMayBlock() {
        if (isRuntimeExecutionContext()) {
            throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.INVALID_OPERATION,
                    "Blocking submission is only valid on an application thread");
        }
    }

    private static boolean isRuntimeExecutionContext() {
        return Boolean.TRUE.equals(INFRASTRUCTURE_THREAD.get())
                || ZLinkApplicationJobContext.current().isPresent()
                || ZLinkSuspendInvocationContext.currentApplicationExecution() != null
                || ZLinkSuspendInvocationContext.currentSerialExecutionTurn() != null
                || ZLinkStateLane.current() != null;
    }
}
