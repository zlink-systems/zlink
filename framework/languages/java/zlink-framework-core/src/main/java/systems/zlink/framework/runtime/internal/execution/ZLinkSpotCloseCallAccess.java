package systems.zlink.framework.runtime.internal.execution;

import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue;

import java.util.List;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.BiFunction;
import java.util.function.Supplier;

/** Internal access to runtime execution controls owned by the execution queue. */
public final class ZLinkSpotCloseCallAccess {
    private static volatile SpotCloseCallAccess access;

    private ZLinkSpotCloseCallAccess() {}

    @FunctionalInterface
    public interface ExecutionPolicy {
        void check(boolean application);
    }

    /** Implemented by the execution queue, which owns the package-private controls. */
    public interface SpotCloseCallAccess {
        void ensureAllowed(ZLinkSerialExecutionQueue queue);

        void executionPolicy(ZLinkSerialExecutionQueue queue, ExecutionPolicy policy);

        void requireExecution(ZLinkSerialExecutionQueue queue, boolean application);

        CompletionStage<Void> enqueueActorWithPayloadBytes(
                ZLinkSerialExecutionQueue queue,
                long payloadBytes,
                Supplier<CompletionStage<Void>> operation,
                Runnable release,
                CompletableFuture<Void> admission);

        CompletionStage<List<CompletableFuture<Void>>> admitDurablePrefix(
                ZLinkSerialExecutionQueue queue,
                List<ZLinkSerialExecutionQueue.QueuedRecord> records,
                BiFunction<
                                ZLinkSerialExecutionQueue.QueuedRecord,
                                CompletableFuture<Void>,
                                Supplier<CompletionStage<Void>>>
                        replay,
                ZLinkApplicationJobQueue jobs);
    }

    /** Registers the queue implementation during its class initialization. */
    public static synchronized void registerAccess(SpotCloseCallAccess implementation) {
        if (access != null) {
            throw new IllegalStateException("Spot Close access is already registered");
        }
        access = Objects.requireNonNull(implementation, "implementation");
    }

    /** Enforces the queue's Spot Close execution gate check. */
    public static void ensureAllowed(ZLinkSerialExecutionQueue queue) {
        selectedAccess().ensureAllowed(Objects.requireNonNull(queue, "queue"));
    }

    public static void executionPolicy(ZLinkSerialExecutionQueue queue, ExecutionPolicy policy) {
        selectedAccess().executionPolicy(Objects.requireNonNull(queue, "queue"), policy);
    }

    public static void requireExecution(ZLinkSerialExecutionQueue queue, boolean application) {
        selectedAccess().requireExecution(Objects.requireNonNull(queue, "queue"), application);
    }

    /** Enqueues accepted Actor work whose payload is released when the work ends. */
    public static CompletionStage<Void> enqueueActorWithPayloadBytes(
            ZLinkSerialExecutionQueue queue,
            long payloadBytes,
            Supplier<CompletionStage<Void>> operation,
            Runnable release,
            CompletableFuture<Void> admission) {
        return selectedAccess()
                .enqueueActorWithPayloadBytes(
                        Objects.requireNonNull(queue, "queue"),
                        payloadBytes,
                        operation,
                        release,
                        admission);
    }

    public static CompletionStage<List<CompletableFuture<Void>>> admitDurablePrefix(
            ZLinkSerialExecutionQueue queue,
            List<ZLinkSerialExecutionQueue.QueuedRecord> records,
            BiFunction<
                            ZLinkSerialExecutionQueue.QueuedRecord,
                            CompletableFuture<Void>,
                            Supplier<CompletionStage<Void>>>
                    replay,
            ZLinkApplicationJobQueue jobs) {
        return selectedAccess()
                .admitDurablePrefix(Objects.requireNonNull(queue, "queue"), records, replay, jobs);
    }

    private static SpotCloseCallAccess selectedAccess() {
        SpotCloseCallAccess selected = access;
        if (selected == null) {
            // The queue instance implies its class initializer registered this bridge.
            throw new IllegalStateException("Spot Close access is not registered");
        }
        return selected;
    }
}
