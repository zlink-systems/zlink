package systems.zlink.framework.runtime.streams;

import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executor;
import java.util.function.Supplier;
import systems.zlink.framework.execution.ZLinkExecutionLanePolicy;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;

/** Owns the one serial execution queue associated with one STREAM session. */
final class ZLinkSessionSerialExecutor {
    private final ZLinkSerialExecutionQueue queue;

    ZLinkSessionSerialExecutor(Executor executor) {
        queue = new ZLinkSerialExecutionQueue(
            executor, ZLinkExecutionLanePolicy.session());
    }

    CompletionStage<Void> executeApplication(
        Supplier<CompletionStage<Void>> operation) {
        return queue.enqueue(operation);
    }

    CompletionStage<Void> executeControl(
        Supplier<CompletionStage<Void>> operation) {
        return queue.enqueue(operation);
    }

    CompletionStage<Void> executeInfrastructure(
        Supplier<CompletionStage<Void>> operation) {
        return queue.enqueue(operation);
    }

    CompletionStage<Void> executeFinal(Supplier<CompletionStage<Void>> operation) {
        return queue.enqueue(operation);
    }

    CompletionStage<Void> executeLifecycleNext(
        Supplier<CompletionStage<Void>> operation) {
        return queue.enqueueBarrierNext(operation);
    }

    CompletionStage<Void> awaitQuiescence() { return queue.awaitQuiescence(); }
}
