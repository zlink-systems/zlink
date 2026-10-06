package systems.zlink.framework.runtime.spots;

import systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode;
import systems.zlink.framework.execution.ZLinkExecutionLanePolicy;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.actors.ZLinkActorDispatchTarget;
import systems.zlink.framework.runtime.actors.ZLinkActorSerialExecutor;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.relocation.ZLinkRetainedSerialQueueCommit;

import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executor;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.Function;
import java.util.function.Supplier;

/** Coordinates the Spot queue and its named timer execution queues. */
public final class ZLinkSpotSerialExecutor implements ZLinkActorDispatchTarget {
    private final ZLinkSerialExecutionQueue spotQueue;
    private final ZLinkSerialExecutionQueue infrastructureQueue;
    private final Executor serialExecutor;
    private final boolean sharedSpotGate;
    private final ZLinkSerialExecutionQueue.SharedSpotGate sharedExecution;
    private final AtomicBoolean closed = new AtomicBoolean();
    // Named child queues are C2 state: clearing this map and completing its
    // queues must happen as one state-lane turn when Spot shutdown is wired.
    private final ZLinkStateLane stateLane = new ZLinkStateLane();
    private final Map<String, ZLinkSerialExecutionQueue> timerQueues = new LinkedHashMap<>();
    private final Map<String, ZLinkActorSerialExecutor> actorQueues = new LinkedHashMap<>();

    public ZLinkSpotSerialExecutor(
            ZLinkSerialExecutionQueue spotQueue,
            Executor infrastructureExecutor,
            Executor serialExecutor,
            ZLinkUserSpotExecutionMode executionMode,
            boolean instanceSpot) {
        this(
                spotQueue,
                new ZLinkSerialExecutionQueue(
                        infrastructureExecutor, ZLinkExecutionLanePolicy.spot()),
                serialExecutor,
                executionMode,
                instanceSpot);
    }

    public ZLinkSpotSerialExecutor(
            ZLinkSerialExecutionQueue spotQueue,
            ZLinkSerialExecutionQueue infrastructureQueue,
            Executor serialExecutor,
            ZLinkUserSpotExecutionMode executionMode,
            boolean instanceSpot) {
        this.spotQueue = Objects.requireNonNull(spotQueue, "spotQueue");
        this.infrastructureQueue =
                Objects.requireNonNull(infrastructureQueue, "infrastructureQueue");
        this.serialExecutor = Objects.requireNonNull(serialExecutor, "serialExecutor");
        this.sharedSpotGate = instanceSpot || executionMode == ZLinkUserSpotExecutionMode.SPOT_WIDE;
        this.sharedExecution =
                sharedSpotGate ? new ZLinkSerialExecutionQueue.SharedSpotGate(spotQueue) : null;
    }

    CompletionStage<Void> executeSpot(
            long payloadBytes, Supplier<CompletionStage<Void>> operation) {
        return spotQueue.enqueueWithPayloadBytes(payloadBytes, operation, null);
    }

    CompletionStage<Void> executeInfrastructure(Supplier<CompletionStage<Void>> operation) {
        return infrastructureQueue.enqueueWithPayloadBytes(0, operation, null);
    }

    @Override
    public CompletionStage<Void> executeActor(
            String actorId,
            Supplier<CompletionStage<Void>> operation,
            CompletableFuture<Void> admission) {
        return executeActor(actorId, 0, operation, admission);
    }

    @Override
    public CompletionStage<ZLinkActorSerialExecutor> claimActorQueue(String actorId) {
        return actorQueueAsync(actorId);
    }

    @Override
    public CompletionStage<Void> executeActor(
            String actorId,
            long payloadBytes,
            Supplier<CompletionStage<Void>> operation,
            CompletableFuture<Void> admission) {
        return submitActor(
                () ->
                        claimActorQueue(actorId)
                                .thenCompose(
                                        activation ->
                                                enqueueActor(
                                                        activation,
                                                        payloadBytes,
                                                        operation,
                                                        admission)),
                admission);
    }

    @Override
    public CompletionStage<Void> executeActor(
            ZLinkActorSerialExecutor activation,
            long payloadBytes,
            Supplier<CompletionStage<Void>> operation,
            CompletableFuture<Void> admission) {
        return submitActor(
                () -> enqueueActor(activation, payloadBytes, operation, admission), admission);
    }

    private CompletionStage<Void> enqueueActor(
            ZLinkActorSerialExecutor activation,
            long payloadBytes,
            Supplier<CompletionStage<Void>> operation,
            CompletableFuture<Void> admission) {
        return activation.executeActor(payloadBytes, operation, admission);
    }

    @Override
    public CompletionStage<Void> executeActor(
            String actorId,
            byte[] acceptedJournalRecord,
            Supplier<CompletionStage<Void>> operation,
            Runnable relocationRelease,
            CompletableFuture<Void> admission) {
        return submitActor(
                () ->
                        claimActorQueue(actorId)
                                .thenCompose(
                                        activation ->
                                                enqueueActor(
                                                        activation,
                                                        acceptedJournalRecord,
                                                        operation,
                                                        relocationRelease,
                                                        admission)),
                admission);
    }

    @Override
    public CompletionStage<Void> executeActor(
            ZLinkActorSerialExecutor activation,
            byte[] acceptedJournalRecord,
            Supplier<CompletionStage<Void>> operation,
            Runnable relocationRelease,
            CompletableFuture<Void> admission) {
        return submitActor(
                () ->
                        enqueueActor(
                                activation,
                                acceptedJournalRecord,
                                operation,
                                relocationRelease,
                                admission),
                admission);
    }

    private CompletionStage<Void> enqueueActor(
            ZLinkActorSerialExecutor activation,
            byte[] acceptedJournalRecord,
            Supplier<CompletionStage<Void>> operation,
            Runnable relocationRelease,
            CompletableFuture<Void> admission) {
        return activation.executeActor(
                acceptedJournalRecord, operation, relocationRelease, admission);
    }

    @Override
    public CompletionStage<Void> executeActorLazyRecord(
            String actorId,
            Supplier<byte[]> acceptedJournalRecord,
            long acceptedJournalRecordSizeHint,
            Supplier<CompletionStage<Void>> operation,
            Runnable relocationRelease,
            CompletableFuture<Void> admission) {
        return submitActor(
                () ->
                        claimActorQueue(actorId)
                                .thenCompose(
                                        activation ->
                                                enqueueActorLazyRecord(
                                                        activation,
                                                        acceptedJournalRecord,
                                                        acceptedJournalRecordSizeHint,
                                                        operation,
                                                        relocationRelease,
                                                        admission)),
                admission);
    }

    @Override
    public CompletionStage<Void> executeActorLazyRecord(
            ZLinkActorSerialExecutor activation,
            Supplier<byte[]> acceptedJournalRecord,
            long acceptedJournalRecordSizeHint,
            Supplier<CompletionStage<Void>> operation,
            Runnable relocationRelease,
            CompletableFuture<Void> admission) {
        return submitActor(
                () ->
                        enqueueActorLazyRecord(
                                activation,
                                acceptedJournalRecord,
                                acceptedJournalRecordSizeHint,
                                operation,
                                relocationRelease,
                                admission),
                admission);
    }

    private CompletionStage<Void> enqueueActorLazyRecord(
            ZLinkActorSerialExecutor activation,
            Supplier<byte[]> acceptedJournalRecord,
            long acceptedJournalRecordSizeHint,
            Supplier<CompletionStage<Void>> operation,
            Runnable relocationRelease,
            CompletableFuture<Void> admission) {
        return activation.executeActorLazyRecord(
                acceptedJournalRecord,
                acceptedJournalRecordSizeHint,
                operation,
                relocationRelease,
                admission);
    }

    private CompletionStage<Void> submitActor(
            Supplier<CompletionStage<Void>> submission, CompletableFuture<Void> admission) {
        CompletionStage<Void> queued;
        try {
            queued = submission.get();
        } catch (RuntimeException failure) {
            queued = CompletableFuture.failedFuture(failure);
        }
        completeAdmissionOnFailure(queued, admission);
        return sharedSpotGate && spotQueue.isCurrent()
                ? ZLinkSerialExecutionQueue.yieldCurrent(queued)
                : queued;
    }

    @Override
    public CompletionStage<Void> executeActorLifecycle(
            String actorId, Supplier<CompletionStage<Void>> operation) {
        return actorQueueAsync(actorId)
                .thenCompose(actorQueue -> actorQueue.executeLifecycle(operation));
    }

    @Override
    public CompletionStage<Void> executeActorLifecycleNext(
            String actorId,
            Supplier<CompletionStage<Void>> operation,
            CompletableFuture<Void> admission) {
        CompletionStage<Void> queued =
                actorQueueAsync(actorId)
                        .thenCompose(
                                actorQueue ->
                                        actorQueue.executeLifecycleNext(operation, admission));
        completeAdmissionOnFailure(queued, admission);
        return queued;
    }

    @Override
    public boolean isActorQueueCurrent(String actorId) {
        return actorQueueIfPresent(actorId).map(ZLinkActorSerialExecutor::isCurrent).orElse(false);
    }

    @Override
    public Optional<ZLinkSerialExecutionQueue.RelocationSeal> trySealActorRelocation(
            String actorId) {
        return actorQueue(actorId).trySealRelocation();
    }

    @Override
    public boolean abortActorRelocation(
            String actorId, ZLinkSerialExecutionQueue.RelocationSeal seal) {
        return actorQueueIfPresent(actorId).map(queue -> queue.abortRelocation(seal)).orElse(false);
    }

    @Override
    public CompletionStage<Boolean> abortActorRelocationAsync(
            String actorId, ZLinkSerialExecutionQueue.RelocationSeal seal) {
        Objects.requireNonNull(actorId, "actorId");
        return stateLane.runNowOrQueue(
                () ->
                        Optional.ofNullable(actorQueues.get(actorId))
                                .map(queue -> queue.abortRelocation(seal))
                                .orElse(false));
    }

    @Override
    public Optional<List<ZLinkSerialExecutionQueue.QueuedRecord>> commitActorRelocation(
            String actorId, ZLinkSerialExecutionQueue.RelocationSeal seal) {
        return actorQueueIfPresent(actorId).flatMap(queue -> queue.commitRelocation(seal));
    }

    @Override
    public Optional<ZLinkRetainedSerialQueueCommit.Commit> retainActorRelocationCommit(
            String actorId, ZLinkSerialExecutionQueue.RelocationSeal seal) {
        return actorQueueIfPresent(actorId).flatMap(queue -> queue.retainRelocationCommit(seal));
    }

    @Override
    public CompletionStage<Optional<ZLinkRetainedSerialQueueCommit.Commit>>
            retainActorRelocationCommitAsync(
                    String actorId, ZLinkSerialExecutionQueue.RelocationSeal seal) {
        Objects.requireNonNull(actorId, "actorId");
        return stateLane.runNowOrQueue(
                () ->
                        Optional.ofNullable(actorQueues.get(actorId))
                                .flatMap(queue -> queue.retainRelocationCommit(seal)));
    }

    @Override
    public Optional<List<ZLinkSerialExecutionQueue.QueuedRecord>> freezeActorRelocationIngress(
            String actorId, ZLinkSerialExecutionQueue.RelocationSeal seal) {
        return actorQueueIfPresent(actorId).flatMap(queue -> queue.freezeRelocationIngress(seal));
    }

    @Override
    public CompletionStage<Void> awaitActorQuiescence(String actorId) {
        Objects.requireNonNull(actorId, "actorId");
        return stateLane
                .runNowOrQueue(() -> Optional.ofNullable(actorQueues.get(actorId)))
                .thenCompose(
                        actorQueue ->
                                actorQueue
                                        .map(ZLinkActorSerialExecutor::awaitQuiescence)
                                        .orElseGet(() -> CompletableFuture.completedFuture(null)));
    }

    @Override
    public void removeActorQueue(String actorId) {
        inStateLane(
                () -> {
                    actorQueues.remove(actorId);
                    return null;
                });
    }

    @Override
    public CompletionStage<Void> removeActorQueueAsync(String actorId) {
        return stateLane.runNowOrQueue(
                () -> {
                    actorQueues.remove(actorId);
                    return null;
                });
    }

    @Override
    public CompletionStage<Void> removeActorQueueAsync(
            String actorId, ZLinkActorSerialExecutor activation) {
        activation.relocationLane().sealClosingAdmission();
        return stateLane.runNowOrQueue(
                () -> {
                    actorQueues.remove(actorId, activation);
                    return null;
                });
    }

    @Override
    public ZLinkSerialExecutionQueue actorRelocationLane(String actorId) {
        return actorQueue(actorId).relocationLane();
    }

    @Override
    public CompletionStage<ZLinkSerialExecutionQueue> actorRelocationLaneAsync(String actorId) {
        return actorQueueAsync(actorId).thenApply(ZLinkActorSerialExecutor::relocationLane);
    }

    CompletionStage<Void> executeTimer(
            String timerName, Function<Boolean, CompletionStage<Void>> operation) {
        if (sharedSpotGate) {
            return executeSpot(0, () -> operation.apply(true));
        }
        return timerQueue(timerName).enqueue(() -> operation.apply(false), null);
    }

    CompletionStage<Void> executeLifecycle(Supplier<CompletionStage<Void>> operation) {
        boolean ownsDependencyTurn =
                spotQueue.isCurrent()
                        || (!sharedSpotGate
                                && ZLinkSerialExecutionQueue.isCurrentTimerOf(spotQueue));
        CompletionStage<Void> lifecycle = requestLifecycle(operation);
        return ownsDependencyTurn ? ZLinkSerialExecutionQueue.yieldCurrent(lifecycle) : lifecycle;
    }

    CompletionStage<Void> requestLifecycle(Supplier<CompletionStage<Void>> operation) {
        CompletionStage<List<ZLinkSerialExecutionQueue>> timers =
                sharedSpotGate
                        ? CompletableFuture.completedFuture(List.of())
                        : stateLane.runNowOrQueue(() -> List.copyOf(timerQueues.values()));
        return timers.thenCompose(
                        snapshot ->
                                CompletableFuture.allOf(
                                        snapshot.stream()
                                                .map(
                                                        queue ->
                                                                queue.enqueue(
                                                                                () ->
                                                                                        CompletableFuture
                                                                                                .completedFuture(
                                                                                                        null),
                                                                                null)
                                                                        .toCompletableFuture())
                                                .toArray(CompletableFuture[]::new)))
                .thenCompose(ignored -> spotQueue.enqueueLifecycleBarrier(operation));
    }

    CompletionStage<Void> executeAcceptedSpot(
            byte[] acceptedJournalRecord,
            Function<Boolean, CompletionStage<Void>> operation,
            Runnable relocationRelease) {
        return spotQueue.enqueueRelocatable(
                acceptedJournalRecord,
                () -> operation.apply(sharedSpotGate),
                relocationRelease,
                null);
    }

    CompletionStage<Void> executeAcceptedSpotLazyRecord(
            Supplier<byte[]> acceptedJournalRecord,
            long acceptedJournalRecordSizeHint,
            Function<Boolean, CompletionStage<Void>> operation,
            Runnable relocationRelease) {
        return executeAcceptedSpotLazyRecord(
                acceptedJournalRecord,
                acceptedJournalRecordSizeHint,
                operation,
                relocationRelease,
                null);
    }

    CompletionStage<Void> executeAcceptedSpotLazyRecord(
            Supplier<byte[]> acceptedJournalRecord,
            long acceptedJournalRecordSizeHint,
            Function<Boolean, CompletionStage<Void>> operation,
            Runnable relocationRelease,
            CompletableFuture<Void> admission) {
        return spotQueue.enqueueRelocatableLazyRecord(
                acceptedJournalRecord,
                acceptedJournalRecordSizeHint,
                () -> operation.apply(sharedSpotGate),
                relocationRelease,
                admission);
    }

    boolean usesSharedExecutionGate() {
        return sharedSpotGate;
    }

    CompletionStage<Void> enqueueSpotBarrierNext(Supplier<CompletionStage<Void>> operation) {
        return spotQueue.enqueueBarrierNext(operation, null);
    }

    CompletionStage<Void> enqueueSpotLifecycleAdmission(Supplier<CompletionStage<Void>> operation) {
        return spotQueue.enqueueLifecycleAdmission(operation);
    }

    CompletionStage<Void> enqueueClose(
            Supplier<CompletionStage<Void>> operation,
            java.util.function.BooleanSupplier committed) {
        CompletionStage<Void> close =
                spotQueue.enqueueLifecycleTransition(
                        operation,
                        ignored -> CompletableFuture.completedFuture(null),
                        committed,
                        ignored -> false);
        return spotQueue.isCurrent() ? ZLinkSerialExecutionQueue.yieldCurrent(close) : close;
    }

    void commitClose() {
        spotQueue.commitLifecycleTransition();
    }

    Optional<ZLinkSerialExecutionQueue.RelocationSeal> trySealRelocation() {
        return spotQueue.trySealRelocation();
    }

    void sealClosingAdmission() {
        spotQueue.sealClosingAdmission();
    }

    CompletionStage<Void> admitIngress(Supplier<CompletionStage<Void>> admission) {
        return spotQueue.admitIngress(admission);
    }

    boolean tryEnqueueSpot(Supplier<CompletionStage<Void>> operation) {
        return spotQueue.tryEnqueue(operation);
    }

    boolean abortRelocation(ZLinkSerialExecutionQueue.RelocationSeal seal) {
        return spotQueue.abortRelocation(seal);
    }

    Optional<List<ZLinkSerialExecutionQueue.QueuedRecord>> commitRelocation(
            ZLinkSerialExecutionQueue.RelocationSeal seal) {
        return spotQueue.commitRelocation(seal);
    }

    CompletionStage<Void> awaitAllLanes(ZLinkSerialExecutionQueue.Quiescence spotScope) {
        List<CompletionStage<Void>> queues = new ArrayList<>();
        queues.add(spotQueue.awaitQuiescence(spotScope));
        queues.add(infrastructureQueue.awaitQuiescence());
        return stateLane
                .runAsync(
                        () -> {
                            timerQueues
                                    .values()
                                    .forEach(queue -> queues.add(queue.awaitQuiescence()));
                            actorQueues
                                    .values()
                                    .forEach(queue -> queues.add(queue.awaitQuiescence()));
                            return queues;
                        })
                .thenCompose(
                        pending ->
                                CompletableFuture.allOf(
                                        pending.stream()
                                                .map(CompletionStage::toCompletableFuture)
                                                .toArray(CompletableFuture[]::new)));
    }

    boolean isCurrentSpotTurn() {
        return spotQueue.isCurrent();
    }

    void close() {
        CompletableFuture<Void> closing = closeAsync().toCompletableFuture();
        assert closing.isDone() || ZLinkStateLane.assertMayBlock();
        closing.join();
    }

    CompletionStage<Void> closeAsync() {
        if (!closed.compareAndSet(false, true)) {
            return CompletableFuture.completedFuture(null);
        }
        CompletionStage<Runnable> prepared =
                stateLane.runAsync(
                        () -> {
                            List<ZLinkSerialExecutionQueue> timers =
                                    List.copyOf(timerQueues.values());
                            List<ZLinkActorSerialExecutor> actors =
                                    List.copyOf(actorQueues.values());
                            timerQueues.clear();
                            actorQueues.clear();
                            return (Runnable)
                                    () -> {
                                        timers.forEach(ZLinkSerialExecutionQueue::close);
                                        actors.forEach(ZLinkActorSerialExecutor::close);
                                        spotQueue.close();
                                        infrastructureQueue.close();
                                    };
                        });
        CompletionStage<Void> laneClosed = stateLane.closeAsync();
        return prepared.thenComposeAsync(
                closeQueues -> {
                    closeQueues.run();
                    return laneClosed;
                });
    }

    Optional<ZLinkSerialExecutionQueue.ActiveTurnSealHandle> captureSpotActiveTurnSealHandle() {
        return spotQueue.captureActiveTurnSealHandle();
    }

    CompletionStage<Map<String, ZLinkSerialExecutionQueue>> relocationLanesAsync(
            List<String> participantActorIds) {
        return stateLane.runNowOrQueue(
                () -> {
                    LinkedHashMap<String, ZLinkSerialExecutionQueue> lanes = new LinkedHashMap<>();
                    lanes.put("spot", spotQueue);
                    timerQueues.entrySet().stream()
                            .sorted(Map.Entry.comparingByKey())
                            .forEach(
                                    entry ->
                                            lanes.put("timer:" + entry.getKey(), entry.getValue()));
                    for (String actorId : participantActorIds) {
                        if (lanes.putIfAbsent(
                                        "actor:" + actorId,
                                        actorQueueOnLane(actorId).relocationLane())
                                != null) {
                            throw new IllegalStateException(
                                    "duplicate User Spot relocation lane: actor:" + actorId);
                        }
                    }
                    return Collections.unmodifiableMap(lanes);
                });
    }

    private ZLinkSerialExecutionQueue timerQueue(String timerName) {
        Objects.requireNonNull(timerName, "timerName");
        return inStateLane(
                () ->
                        timerQueues.computeIfAbsent(
                                timerName,
                                ignored ->
                                        ZLinkSerialExecutionQueue.spotTimer(
                                                serialExecutor, spotQueue)));
    }

    private List<ZLinkActorSerialExecutor> actorSnapshot() {
        return inStateLane(() -> List.copyOf(actorQueues.values()));
    }

    private ZLinkActorSerialExecutor actorQueue(String actorId) {
        Objects.requireNonNull(actorId, "actorId");
        return inStateLane(() -> actorQueueOnLane(actorId));
    }

    private CompletionStage<ZLinkActorSerialExecutor> actorQueueAsync(String actorId) {
        Objects.requireNonNull(actorId, "actorId");
        return stateLane.runNowOrQueue(() -> actorQueueOnLane(actorId));
    }

    private static void completeAdmissionOnFailure(
            CompletionStage<Void> queued, CompletableFuture<Void> admission) {
        if (admission != null) {
            queued.whenComplete(
                    (ignored, failure) -> {
                        if (failure != null) admission.completeExceptionally(failure);
                    });
        }
    }

    private ZLinkActorSerialExecutor actorQueueOnLane(String actorId) {
        return actorQueues.computeIfAbsent(
                actorId,
                ignored ->
                        sharedSpotGate
                                ? new ZLinkActorSerialExecutor(serialExecutor, sharedExecution)
                                : new ZLinkActorSerialExecutor(serialExecutor));
    }

    private Optional<ZLinkActorSerialExecutor> actorQueueIfPresent(String actorId) {
        Objects.requireNonNull(actorId, "actorId");
        return inStateLane(() -> Optional.ofNullable(actorQueues.get(actorId)));
    }

    private <T> T inStateLane(Supplier<T> operation) {
        try {
            CompletableFuture<T> turn = stateLane.runAsync(operation).toCompletableFuture();
            assert turn.isDone() || ZLinkStateLane.assertMayBlock();
            return turn.join();
        } catch (CompletionException failure) {
            Throwable cause = failure.getCause();
            if (cause instanceof RuntimeException runtimeFailure) throw runtimeFailure;
            if (cause instanceof Error error) throw error;
            throw failure;
        }
    }
}
