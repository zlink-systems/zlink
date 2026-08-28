package systems.zlink.framework.runtime.actors;

import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletionStage;
import java.util.function.Supplier;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.internal.relocation.ZLinkRetainedSerialQueueCommit;

/**
 * Runtime-internal Actor queue surface supplied by the owning Spot
 * coordinator.  The node-wide Actor dispatch facade uses this surface for
 * admission and relocation bookkeeping without owning an Actor queue.
 */
public interface ZLinkActorDispatchTarget {
    CompletionStage<Void> executeActor(
        String actorId,
        Supplier<CompletionStage<Void>> operation);

    CompletionStage<Void> executeActor(
        String actorId,
        long payloadBytes,
        Supplier<CompletionStage<Void>> operation);

    CompletionStage<Void> executeActor(
        String actorId,
        byte[] acceptedJournalRecord,
        Supplier<CompletionStage<Void>> operation,
        Runnable relocationRelease);

    CompletionStage<Void> executeActorLazyRecord(
        String actorId,
        Supplier<byte[]> acceptedJournalRecord,
        long acceptedJournalRecordSizeHint,
        Supplier<CompletionStage<Void>> operation,
        Runnable relocationRelease);

    CompletionStage<Void> executeActorLifecycle(
        String actorId,
        Supplier<CompletionStage<Void>> operation);

    CompletionStage<Void> executeActorLifecycleNext(
        String actorId,
        Supplier<CompletionStage<Void>> operation);

    boolean isActorQueueCurrent(String actorId);

    Optional<ZLinkSerialExecutionQueue.RelocationSeal>
        trySealActorRelocation(String actorId);

    ZLinkSerialExecutionQueue actorRelocationLane(String actorId);

    boolean abortActorRelocation(
        String actorId,
        ZLinkSerialExecutionQueue.RelocationSeal seal);

    Optional<List<ZLinkSerialExecutionQueue.QueuedRecord>>
        commitActorRelocation(
            String actorId,
            ZLinkSerialExecutionQueue.RelocationSeal seal);

    Optional<ZLinkRetainedSerialQueueCommit.Commit>
        retainActorRelocationCommit(
            String actorId,
            ZLinkSerialExecutionQueue.RelocationSeal seal);

    Optional<List<ZLinkSerialExecutionQueue.QueuedRecord>>
        freezeActorRelocationIngress(
            String actorId,
            ZLinkSerialExecutionQueue.RelocationSeal seal);

    CompletionStage<Void> awaitActorQuiescence(String actorId);

    void removeActorQueue(String actorId);
}
