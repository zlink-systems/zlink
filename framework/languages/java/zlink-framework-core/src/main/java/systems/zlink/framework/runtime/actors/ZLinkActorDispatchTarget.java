package systems.zlink.framework.runtime.actors;

import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.internal.relocation.ZLinkRetainedSerialQueueCommit;

import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.Supplier;

/**
 * Runtime-internal Actor queue surface supplied by the owning Spot coordinator. The node-wide Actor
 * dispatch facade uses this surface for admission and relocation bookkeeping without owning an
 * Actor queue.
 */
public interface ZLinkActorDispatchTarget {
    record ActivationSnapshot(ZLinkActorDispatchTarget target, Object incarnation) {}

    CompletionStage<ZLinkActorSerialExecutor> claimActorQueue(String actorId);

    CompletionStage<Void> executeActor(
            ZLinkActorSerialExecutor activation,
            long payloadBytes,
            Supplier<CompletionStage<Void>> operation,
            CompletableFuture<Void> admission);

    CompletionStage<Void> executeActor(
            ZLinkActorSerialExecutor activation,
            byte[] acceptedJournalRecord,
            Supplier<CompletionStage<Void>> operation,
            Runnable relocationRelease,
            CompletableFuture<Void> admission);

    CompletionStage<Void> executeActorLazyRecord(
            ZLinkActorSerialExecutor activation,
            Supplier<byte[]> acceptedJournalRecord,
            long acceptedJournalRecordSizeHint,
            Supplier<CompletionStage<Void>> operation,
            Runnable relocationRelease,
            CompletableFuture<Void> admission);

    CompletionStage<Void> removeActorQueueAsync(
            String actorId, ZLinkActorSerialExecutor activation);

    CompletionStage<Void> executeActor(
            String actorId,
            Supplier<CompletionStage<Void>> operation,
            CompletableFuture<Void> admission);

    CompletionStage<Void> executeActor(
            String actorId,
            long payloadBytes,
            Supplier<CompletionStage<Void>> operation,
            CompletableFuture<Void> admission);

    CompletionStage<Void> executeActor(
            String actorId,
            byte[] acceptedJournalRecord,
            Supplier<CompletionStage<Void>> operation,
            Runnable relocationRelease,
            CompletableFuture<Void> admission);

    CompletionStage<Void> executeActorLazyRecord(
            String actorId,
            Supplier<byte[]> acceptedJournalRecord,
            long acceptedJournalRecordSizeHint,
            Supplier<CompletionStage<Void>> operation,
            Runnable relocationRelease,
            CompletableFuture<Void> admission);

    CompletionStage<Void> executeActorLifecycle(
            String actorId, Supplier<CompletionStage<Void>> operation);

    CompletionStage<Void> executeActorLifecycleNext(
            String actorId,
            Supplier<CompletionStage<Void>> operation,
            CompletableFuture<Void> admission);

    boolean isActorQueueCurrent(String actorId);

    Optional<ZLinkSerialExecutionQueue.RelocationSeal> trySealActorRelocation(String actorId);

    ZLinkSerialExecutionQueue actorRelocationLane(String actorId);

    CompletionStage<ZLinkSerialExecutionQueue> actorRelocationLaneAsync(String actorId);

    boolean abortActorRelocation(String actorId, ZLinkSerialExecutionQueue.RelocationSeal seal);

    CompletionStage<Boolean> abortActorRelocationAsync(
            String actorId, ZLinkSerialExecutionQueue.RelocationSeal seal);

    Optional<List<ZLinkSerialExecutionQueue.QueuedRecord>> commitActorRelocation(
            String actorId, ZLinkSerialExecutionQueue.RelocationSeal seal);

    Optional<ZLinkRetainedSerialQueueCommit.Commit> retainActorRelocationCommit(
            String actorId, ZLinkSerialExecutionQueue.RelocationSeal seal);

    CompletionStage<Optional<ZLinkRetainedSerialQueueCommit.Commit>>
            retainActorRelocationCommitAsync(
                    String actorId, ZLinkSerialExecutionQueue.RelocationSeal seal);

    Optional<List<ZLinkSerialExecutionQueue.QueuedRecord>> freezeActorRelocationIngress(
            String actorId, ZLinkSerialExecutionQueue.RelocationSeal seal);

    CompletionStage<Void> awaitActorQuiescence(String actorId);

    void removeActorQueue(String actorId);

    CompletionStage<Void> removeActorQueueAsync(String actorId);
}
