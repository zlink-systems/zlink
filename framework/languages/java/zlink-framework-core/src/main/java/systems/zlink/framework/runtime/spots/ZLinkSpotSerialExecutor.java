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
    private final AtomicBoolean closed = new AtomicBoolean();
    // Named child queues are C2 state: clearing this map and completing its
    // queues must happen as one state-lane turn when Spot shutdown is wired.
    private final ZLinkStateLane stateLane = new ZLinkStateLane();
    private volatile Map<String, ZLinkSerialExecutionQueue> timerQueues = Map.of();
    private volatile Map<String, ZLinkActorSerialExecutor> actorQueues = Map.of();

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
    }

    CompletionStage<Void> executeSpot(
            long payloadBytes, Supplier<CompletionStage<Void>> operation) {
        return spotQueue.enqueueWithPayloadBytes(payloadBytes, operation);
    }

    CompletionStage<Void> executeInfrastructure(Supplier<CompletionStage<Void>> operation) {
        return infrastructureQueue.enqueueWithPayloadBytes(0, operation);
    }

    @Override
    public CompletionStage<Void> executeActor(
            String actorId, Supplier<CompletionStage<Void>> operation) {
        return executeActor(actorId, 0, operation);
    }

    @Override
    public CompletionStage<Void> executeActor(
            String actorId, long payloadBytes, Supplier<CompletionStage<Void>> operation) {
        Objects.requireNonNull(actorId, "actorId");
        Objects.requireNonNull(operation, "operation");
        CompletionStage<Void> queued =
                onActorQueue(actorId, actorQueue ->
                                        actorQueue.executeActor(
                                                payloadBytes,
                                                () ->
                                                        sharedSpotGate
                                                                ? spotQueue.enqueue(operation)
                                                                : operation.get()));
        return sharedSpotGate && spotQueue.isCurrent()
                ? ZLinkSerialExecutionQueue.yieldCurrent(queued)
                : queued;
    }

    @Override
    public CompletionStage<Void> executeActor(
            String actorId,
            byte[] acceptedJournalRecord,
            Supplier<CompletionStage<Void>> operation,
            Runnable relocationRelease) {
        Objects.requireNonNull(actorId, "actorId");
        Objects.requireNonNull(operation, "operation");
        Objects.requireNonNull(relocationRelease, "relocationRelease");
        CompletionStage<Void> queued =
                onActorQueue(actorId, actorQueue ->
                                        actorQueue.executeActor(
                                                acceptedJournalRecord,
                                                () ->
                                                        sharedSpotGate
                                                                ? spotQueue.enqueue(operation)
                                                                : operation.get(),
                                                relocationRelease));
        return sharedSpotGate && spotQueue.isCurrent()
                ? ZLinkSerialExecutionQueue.yieldCurrent(queued)
                : queued;
    }

    @Override
    public CompletionStage<Void> executeActorLazyRecord(
            String actorId,
            Supplier<byte[]> acceptedJournalRecord,
            long acceptedJournalRecordSizeHint,
            Supplier<CompletionStage<Void>> operation,
            Runnable relocationRelease) {
        Objects.requireNonNull(actorId, "actorId");
        Objects.requireNonNull(acceptedJournalRecord, "acceptedJournalRecord");
        Objects.requireNonNull(operation, "operation");
        Objects.requireNonNull(relocationRelease, "relocationRelease");
        CompletionStage<Void> queued =
                onActorQueue(actorId, actorQueue ->
                                        actorQueue.executeActorLazyRecord(
                                                acceptedJournalRecord,
                                                acceptedJournalRecordSizeHint,
                                                () ->
                                                        sharedSpotGate
                                                                ? spotQueue.enqueue(operation)
                                                                : operation.get(),
                                                relocationRelease));
        return sharedSpotGate && spotQueue.isCurrent()
                ? ZLinkSerialExecutionQueue.yieldCurrent(queued)
                : queued;
    }

    @Override
    public CompletionStage<Void> executeActorLifecycle(
            String actorId, Supplier<CompletionStage<Void>> operation) {
        return onActorQueue(actorId, queue -> queue.executeLifecycle(operation));
    }

    @Override
    public CompletionStage<Void> executeActorLifecycleNext(
            String actorId, Supplier<CompletionStage<Void>> operation) {
        return onActorQueue(actorId, queue -> queue.executeLifecycleNext(operation));
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
    public Optional<List<ZLinkSerialExecutionQueue.QueuedRecord>> freezeActorRelocationIngress(
            String actorId, ZLinkSerialExecutionQueue.RelocationSeal seal) {
        return actorQueueIfPresent(actorId).flatMap(queue -> queue.freezeRelocationIngress(seal));
    }

    @Override
    public CompletionStage<Void> awaitActorQuiescence(String actorId) {
        Objects.requireNonNull(actorId, "actorId");
        // A lane turn orders this after every admission submitted before it.
        return stateLane
                .runAsync(() -> Optional.ofNullable(actorQueues.get(actorId)))
                .thenCompose(
                        queue ->
                                queue.map(ZLinkActorSerialExecutor::awaitQuiescence)
                                        .orElseGet(() -> CompletableFuture.completedFuture(null)));
    }

    @Override
    public void removeActorQueue(String actorId) {
        inStateLane(
                () -> {
                    if (actorQueues.containsKey(actorId)) {
                        Map<String, ZLinkActorSerialExecutor> next =
                                new LinkedHashMap<>(actorQueues);
                        next.remove(actorId);
                        actorQueues = java.util.Collections.unmodifiableMap(next);
                    }
                    return null;
                });
    }

    @Override
    public ZLinkSerialExecutionQueue actorRelocationLane(String actorId) {
        return actorQueue(actorId).relocationLane();
    }

    CompletionStage<Void> executeTimer(
            String timerName, Function<Boolean, CompletionStage<Void>> operation) {
        if (sharedSpotGate) {
            return executeSpot(0, () -> operation.apply(true));
        }
        return timerQueue(timerName).enqueue(() -> operation.apply(false));
    }

    CompletionStage<Void> executeLifecycle(Supplier<CompletionStage<Void>> operation) {
        CompletionStage<Void> lifecycle = requestLifecycle(operation);
        List<ZLinkSerialExecutionQueue> timerQueues = sharedSpotGate ? List.of() : timerSnapshot();
        boolean ownsDependencyTurn =
                spotQueue.isCurrent()
                        || timerQueues.stream().anyMatch(ZLinkSerialExecutionQueue::isCurrent);
        return ownsDependencyTurn ? ZLinkSerialExecutionQueue.yieldCurrent(lifecycle) : lifecycle;
    }

    CompletionStage<Void> requestLifecycle(Supplier<CompletionStage<Void>> operation) {
        List<ZLinkSerialExecutionQueue> timerQueues = sharedSpotGate ? List.of() : timerSnapshot();
        CompletionStage<Void> timers =
                sharedSpotGate
                        ? CompletableFuture.completedFuture(null)
                        : CompletableFuture.allOf(
                                timerQueues.stream()
                                        .map(
                                                queue ->
                                                        queue.enqueue(
                                                                        () ->
                                                                                CompletableFuture
                                                                                        .completedFuture(
                                                                                                null))
                                                                .toCompletableFuture())
                                        .toArray(CompletableFuture[]::new));
        return timers.thenCompose(ignored -> spotQueue.enqueueLifecycleBarrier(operation));
    }

    CompletionStage<Void> executeAcceptedSpot(
            byte[] acceptedJournalRecord,
            Function<Boolean, CompletionStage<Void>> operation,
            Runnable relocationRelease) {
        return spotQueue.enqueueRelocatable(
                acceptedJournalRecord, () -> operation.apply(sharedSpotGate), relocationRelease);
    }

    CompletionStage<Void> executeAcceptedSpotLazyRecord(
            Supplier<byte[]> acceptedJournalRecord,
            long acceptedJournalRecordSizeHint,
            Function<Boolean, CompletionStage<Void>> operation,
            Runnable relocationRelease) {
        return spotQueue.enqueueRelocatableLazyRecord(
                acceptedJournalRecord,
                acceptedJournalRecordSizeHint,
                () -> operation.apply(sharedSpotGate),
                relocationRelease);
    }

    boolean usesSharedExecutionGate() {
        return sharedSpotGate;
    }

    CompletionStage<Void> enqueueSpotBarrierNext(Supplier<CompletionStage<Void>> operation) {
        return spotQueue.enqueueBarrierNext(operation);
    }

    Optional<ZLinkSerialExecutionQueue.RelocationSeal> trySealRelocation() {
        return spotQueue.trySealRelocation();
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
        timerSnapshot().forEach(queue -> queues.add(queue.awaitQuiescence()));
        // A lane turn orders the Actor snapshot after every admission submitted before it.
        queues.add(
                stateLane
                        .runAsync(() -> List.copyOf(actorQueues.values()))
                        .thenCompose(
                                actors ->
                                        allOf(
                                                actors.stream()
                                                        .map(
                                                                ZLinkActorSerialExecutor
                                                                        ::awaitQuiescence)
                                                        .toList())));
        return allOf(queues);
    }

    private static CompletionStage<Void> allOf(List<CompletionStage<Void>> stages) {
        return CompletableFuture.allOf(
                stages.stream()
                        .map(CompletionStage::toCompletableFuture)
                        .toArray(CompletableFuture[]::new));
    }

    boolean isCurrentSpotTurn() {
        return spotQueue.isCurrent();
    }

    void close() {
        assert ZLinkStateLane.assertMayBlock();
        closeAsync().toCompletableFuture().join();
    }

    CompletionStage<Void> closeAsync() {
        if (!closed.compareAndSet(false, true)) {
            return CompletableFuture.completedFuture(null);
        }
        return stateLane
                .runAsync(
                        () -> {
                            List<ZLinkSerialExecutionQueue> timers =
                                    List.copyOf(timerQueues.values());
                            List<ZLinkActorSerialExecutor> actors =
                                    List.copyOf(actorQueues.values());
                            timerQueues = Map.of();
                            actorQueues = Map.of();
                            return (Runnable)
                                    () -> {
                                        timers.forEach(ZLinkSerialExecutionQueue::close);
                                        actors.forEach(ZLinkActorSerialExecutor::close);
                                        spotQueue.close();
                                        infrastructureQueue.close();
                                    };
                        })
                .thenComposeAsync(
                        closeQueues -> {
                            closeQueues.run();
                            return stateLane.closeAsync();
                        });
    }

    Map<String, ZLinkSerialExecutionQueue> relocationLanes() {
        LinkedHashMap<String, ZLinkSerialExecutionQueue> lanes = new LinkedHashMap<>();
        lanes.put("spot", spotQueue);
        timerQueues.entrySet().stream()
                .sorted(Map.Entry.comparingByKey())
                .forEach(entry -> lanes.put("timer:" + entry.getKey(), entry.getValue()));
        return Collections.unmodifiableMap(lanes);
    }

    private ZLinkSerialExecutionQueue timerQueue(String timerName) {
        Objects.requireNonNull(timerName, "timerName");
        return inStateLane(
                () -> {
                    ZLinkSerialExecutionQueue current = timerQueues.get(timerName);
                    if (current != null) {
                        return current;
                    }
                    ZLinkSerialExecutionQueue created =
                            new ZLinkSerialExecutionQueue(
                                    serialExecutor, ZLinkExecutionLanePolicy.spot());
                    Map<String, ZLinkSerialExecutionQueue> next = new LinkedHashMap<>(timerQueues);
                    next.put(timerName, created);
                    timerQueues = Collections.unmodifiableMap(next);
                    return created;
                });
    }

    private List<ZLinkSerialExecutionQueue> timerSnapshot() {
        return List.copyOf(timerQueues.values());
    }

    private ZLinkActorSerialExecutor actorQueue(String actorId) {
        Objects.requireNonNull(actorId, "actorId");
        return inStateLane(() -> actorQueueOnLane(actorId));
    }

    /**
     * Runs one Actor queue operation in one state-lane turn: {@link #actorQueueOnLane} is the only
     * place that finds or creates the Actor's queue, and the lane's FIFO keeps submission order.
     */
    private <T> CompletionStage<T> onActorQueue(
            String actorId, Function<ZLinkActorSerialExecutor, CompletionStage<T>> operation) {
        Objects.requireNonNull(actorId, "actorId");
        return stateLane.admitAsync(() -> actorQueueOnLane(actorId), operation);
    }

    private ZLinkActorSerialExecutor actorQueueOnLane(String actorId) {
        ZLinkActorSerialExecutor current = actorQueues.get(actorId);
        if (current != null) {
            return current;
        }
        ZLinkActorSerialExecutor created = new ZLinkActorSerialExecutor(serialExecutor);
        Map<String, ZLinkActorSerialExecutor> next = new LinkedHashMap<>(actorQueues);
        next.put(actorId, created);
        actorQueues = java.util.Collections.unmodifiableMap(next);
        return created;
    }

    private Optional<ZLinkActorSerialExecutor> actorQueueIfPresent(String actorId) {
        Objects.requireNonNull(actorId, "actorId");
        return Optional.ofNullable(actorQueues.get(actorId));
    }

    private <T> T inStateLane(Supplier<T> operation) {
        try {
            var turn = stateLane.runNowOrQueue(operation).toCompletableFuture();
            // An idle lane ran the turn on this thread; only a pending turn is a wait.
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
