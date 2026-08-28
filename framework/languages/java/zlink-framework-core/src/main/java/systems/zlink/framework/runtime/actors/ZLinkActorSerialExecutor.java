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
public final class ZLinkActorSerialExecutor {
    private final ZLinkSerialExecutionQueue queue;

    public ZLinkActorSerialExecutor(Executor executor) {
        queue = executor == null
            ? new ZLinkSerialExecutionQueue(ZLinkExecutionLanePolicy.actorDelivery())
            : new ZLinkSerialExecutionQueue(
                executor, ZLinkExecutionLanePolicy.actorDelivery());
    }

    public CompletionStage<Void> executeActor(Supplier<CompletionStage<Void>> operation) {
        return queue.enqueue(operation);
    }

    public CompletionStage<Void> executeActor(
        long payloadBytes,
        Supplier<CompletionStage<Void>> operation) {
        return queue.enqueueWithPayloadBytes(payloadBytes, operation);
    }

    public CompletionStage<Void> executeActor(
        byte[] acceptedJournalRecord,
        Supplier<CompletionStage<Void>> operation,
        Runnable relocationRelease) {
        return acceptedJournalRecord == null || acceptedJournalRecord.length == 0
            ? executeActor(operation)
            : queue.enqueueRelocatable(
                acceptedJournalRecord, operation, relocationRelease);
    }

    public CompletionStage<Void> executeActorLazyRecord(
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

    public CompletionStage<Void> executeLifecycle(
        Supplier<CompletionStage<Void>> operation) {
        return queue.enqueueLifecycleBarrier(operation);
    }

    public CompletionStage<Void> executeLifecycleNext(
        Supplier<CompletionStage<Void>> operation) {
        return queue.enqueueBarrierNext(operation);
    }

    public boolean isCurrent() { return queue.isCurrent(); }
    public Optional<ZLinkSerialExecutionQueue.RelocationSeal> trySealRelocation() {
        return queue.trySealRelocation();
    }
    public boolean abortRelocation(ZLinkSerialExecutionQueue.RelocationSeal seal) {
        return queue.abortRelocation(seal);
    }
    public Optional<List<ZLinkSerialExecutionQueue.QueuedRecord>> commitRelocation(
        ZLinkSerialExecutionQueue.RelocationSeal seal) {
        return queue.commitRelocation(seal);
    }
    public Optional<ZLinkRetainedSerialQueueCommit.Commit> retainRelocationCommit(
        ZLinkSerialExecutionQueue.RelocationSeal seal) {
        return ZLinkRetainedSerialQueueCommit.retain(queue, seal);
    }
    public Optional<List<ZLinkSerialExecutionQueue.QueuedRecord>> freezeRelocationIngress(
        ZLinkSerialExecutionQueue.RelocationSeal seal) {
        return queue.freezeRelocationIngress(seal);
    }
    public CompletionStage<Void> awaitQuiescence() { return queue.awaitQuiescence(); }

    public ZLinkSerialExecutionQueue relocationLane() { return queue; }

    public void close() { queue.close(); }
}
