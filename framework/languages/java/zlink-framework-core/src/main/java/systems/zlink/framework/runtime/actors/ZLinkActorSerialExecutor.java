package systems.zlink.framework.runtime.actors;

import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executor;
import java.util.function.Supplier;
import systems.zlink.framework.execution.ZLinkExecutionLanePolicy;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.internal.relocation.ZLinkRetainedSerialQueueCommit;

/** Owns the one serial execution queue associated with one Actor activation. */
final class ZLinkActorSerialExecutor {
    private final ZLinkSerialExecutionQueue queue;

    ZLinkActorSerialExecutor(Executor executor) {
        queue = executor == null
            ? new ZLinkSerialExecutionQueue(ZLinkExecutionLanePolicy.actorDelivery())
            : new ZLinkSerialExecutionQueue(
                executor, ZLinkExecutionLanePolicy.actorDelivery());
    }

    CompletionStage<Void> executeActor(Supplier<CompletionStage<Void>> operation) {
        return queue.enqueue(operation);
    }

    CompletionStage<Void> executeActor(
        long payloadBytes,
        Supplier<CompletionStage<Void>> operation) {
        return queue.enqueueWithPayloadBytes(payloadBytes, operation);
    }

    CompletionStage<Void> executeActor(
        byte[] acceptedJournalRecord,
        Supplier<CompletionStage<Void>> operation,
        Runnable relocationRelease) {
        return acceptedJournalRecord == null || acceptedJournalRecord.length == 0
            ? executeActor(operation)
            : queue.enqueueRelocatable(
                acceptedJournalRecord, operation, relocationRelease);
    }

    CompletionStage<Void> executeActorLazyRecord(
        Supplier<byte[]> acceptedJournalRecord,
        long acceptedJournalRecordSizeHint,
        Supplier<CompletionStage<Void>> operation,
        Runnable relocationRelease) {
        return queue.enqueueRelocatableLazyRecord(
            acceptedJournalRecord,
            acceptedJournalRecordSizeHint,
            operation,
            relocationRelease);
    }

    CompletionStage<Void> executeLifecycle(
        Supplier<CompletionStage<Void>> operation) {
        return queue.enqueueLifecycleBarrier(operation);
    }

    CompletionStage<Void> executeLifecycleNext(
        Supplier<CompletionStage<Void>> operation) {
        return queue.enqueueBarrierNext(operation);
    }

    boolean isCurrent() { return queue.isCurrent(); }
    Optional<ZLinkSerialExecutionQueue.RelocationSeal> trySealRelocation() {
        return queue.trySealRelocation();
    }
    boolean abortRelocation(ZLinkSerialExecutionQueue.RelocationSeal seal) {
        return queue.abortRelocation(seal);
    }
    Optional<List<ZLinkSerialExecutionQueue.QueuedRecord>> commitRelocation(
        ZLinkSerialExecutionQueue.RelocationSeal seal) {
        return queue.commitRelocation(seal);
    }
    Optional<ZLinkRetainedSerialQueueCommit.Commit> retainRelocationCommit(
        ZLinkSerialExecutionQueue.RelocationSeal seal) {
        return ZLinkRetainedSerialQueueCommit.retain(queue, seal);
    }
    Optional<List<ZLinkSerialExecutionQueue.QueuedRecord>> freezeRelocationIngress(
        ZLinkSerialExecutionQueue.RelocationSeal seal) {
        return queue.freezeRelocationIngress(seal);
    }
    CompletionStage<Void> awaitQuiescence() { return queue.awaitQuiescence(); }

    ZLinkSerialExecutionQueue relocationLane() { return queue; }
}
