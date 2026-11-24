package systems.zlink.framework.runtime.streams;

import systems.zlink.framework.execution.ZLinkExecutionLanePolicy;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;

import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executor;
import java.util.function.Supplier;

/** Owns the one serial execution queue associated with one STREAM session. */
final class ZLinkSessionSerialExecutor {
    private final ZLinkSerialExecutionQueue queue;

    ZLinkSessionSerialExecutor(Executor executor) {
        queue = new ZLinkSerialExecutionQueue(executor, ZLinkExecutionLanePolicy.session());
    }

    CompletionStage<Void> executeApplication(Supplier<CompletionStage<Void>> operation) {
        return queue.enqueue(operation, null);
    }

    CompletionStage<Void> executeControl(Supplier<CompletionStage<Void>> operation) {
        return queue.enqueueLifecycleBarrier(operation);
    }

    // The STREAM registry's state lane serializes all close callers for this queue.
    boolean sealAdmission() {
        if (admissionSealed()) return false;
        queue.sealClosingAdmission();
        return true;
    }

    boolean admissionSealed() {
        return !queue.steady();
    }

    CompletionStage<Void> acceptedPackets() {
        return queue.awaitQuiescence(ZLinkSerialExecutionQueue.Quiescence.APPLICATION);
    }

    CompletionStage<Void> executeFinal(Supplier<CompletionStage<Void>> operation) {
        return queue.enqueuePreviouslyAccepted(operation);
    }

    CompletionStage<Void> executeLifecycleNext(Supplier<CompletionStage<Void>> operation) {
        return queue.enqueueBarrierNext(operation, null);
    }

    CompletionStage<Void> awaitQuiescence() {
        return queue.awaitQuiescence();
    }
}
