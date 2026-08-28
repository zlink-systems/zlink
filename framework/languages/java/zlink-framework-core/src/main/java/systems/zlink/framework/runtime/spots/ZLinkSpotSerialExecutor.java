package systems.zlink.framework.runtime.spots;

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
import java.util.function.Function;
import java.util.function.Supplier;
import systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode;
import systems.zlink.framework.execution.ZLinkExecutionLanePolicy;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

/** Coordinates the Spot queue and its named timer execution queues. */
final class ZLinkSpotSerialExecutor {
    private final ZLinkSerialExecutionQueue spotQueue;
    private final ZLinkSerialExecutionQueue infrastructureQueue;
    private final Executor serialExecutor;
    private final boolean sharedSpotGate;
    // Named child queues are C2 state: clearing this map and completing its
    // queues must happen as one state-lane turn when Spot shutdown is wired.
    private final ZLinkStateLane stateLane = new ZLinkStateLane();
    private final Map<String, ZLinkSerialExecutionQueue> timerQueues =
        new LinkedHashMap<>();

    ZLinkSpotSerialExecutor(
        ZLinkSerialExecutionQueue spotQueue,
        Executor infrastructureExecutor,
        Executor serialExecutor,
        ZLinkUserSpotExecutionMode executionMode,
        boolean instanceSpot) {
        this.spotQueue = Objects.requireNonNull(spotQueue, "spotQueue");
        this.infrastructureQueue = new ZLinkSerialExecutionQueue(
            infrastructureExecutor, ZLinkExecutionLanePolicy.spot());
        this.serialExecutor = Objects.requireNonNull(serialExecutor, "serialExecutor");
        this.sharedSpotGate = instanceSpot
            || executionMode == ZLinkUserSpotExecutionMode.SPOT_WIDE;
    }

    CompletionStage<Void> executeSpot(
        long payloadBytes,
        Supplier<CompletionStage<Void>> operation) {
        return spotQueue.enqueueWithPayloadBytes(payloadBytes, operation);
    }

    CompletionStage<Void> executeInfrastructure(
        Supplier<CompletionStage<Void>> operation) {
        return infrastructureQueue.enqueueWithPayloadBytes(0, operation);
    }

    CompletionStage<Void> executeActor(
        Function<Supplier<CompletionStage<Void>>, CompletionStage<Void>> submit,
        Function<Boolean, CompletionStage<Void>> operation) {
        CompletionStage<Void> queued = submit.apply(() -> sharedSpotGate
            ? spotQueue.enqueue(() -> operation.apply(true))
            : operation.apply(false));
        return sharedSpotGate && spotQueue.isCurrent()
            ? ZLinkSerialExecutionQueue.yieldCurrent(queued)
            : queued;
    }

    CompletionStage<Void> executeTimer(
        String timerName,
        Function<Boolean, CompletionStage<Void>> operation) {
        if (sharedSpotGate) {
            return executeSpot(0, () -> operation.apply(true));
        }
        return timerQueue(timerName).enqueue(() -> operation.apply(false));
    }

    CompletionStage<Void> executeLifecycle(
        Supplier<CompletionStage<Void>> operation) {
        CompletionStage<Void> timers = sharedSpotGate
            ? CompletableFuture.completedFuture(null)
            : CompletableFuture.allOf(timerSnapshot().stream()
                .map(queue -> queue.enqueue(() -> CompletableFuture.completedFuture(null))
                    .toCompletableFuture())
                .toArray(CompletableFuture[]::new));
        return ZLinkSerialExecutionQueue.yieldCurrent(timers.thenCompose(
            ignored -> spotQueue.enqueueLifecycleBarrier(operation)));
    }

    CompletionStage<Void> executeAcceptedSpot(
        byte[] acceptedJournalRecord,
        Function<Boolean, CompletionStage<Void>> operation,
        Runnable relocationRelease) {
        return spotQueue.enqueueRelocatable(
            acceptedJournalRecord,
            () -> operation.apply(sharedSpotGate),
            relocationRelease);
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

    boolean usesSharedExecutionGate() { return sharedSpotGate; }
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
    CompletionStage<Void> awaitAllLanes() {
        List<CompletionStage<Void>> queues = new ArrayList<>();
        queues.add(spotQueue.awaitQuiescence());
        queues.add(infrastructureQueue.awaitQuiescence());
        timerSnapshot().forEach(queue -> queues.add(queue.awaitQuiescence()));
        return CompletableFuture.allOf(queues.stream()
            .map(CompletionStage::toCompletableFuture)
            .toArray(CompletableFuture[]::new));
    }
    Map<String, ZLinkSerialExecutionQueue> relocationLanes() {
        LinkedHashMap<String, ZLinkSerialExecutionQueue> lanes = new LinkedHashMap<>();
        lanes.put("spot", spotQueue);
        inStateLane(() -> {
            timerQueues.entrySet().stream()
                .sorted(Map.Entry.comparingByKey())
                .forEach(entry -> lanes.put(
                    "timer:" + entry.getKey(), entry.getValue()));
            return null;
        });
        return Collections.unmodifiableMap(lanes);
    }

    private ZLinkSerialExecutionQueue timerQueue(String timerName) {
        Objects.requireNonNull(timerName, "timerName");
        return inStateLane(() -> timerQueues.computeIfAbsent(timerName,
            ignored -> new ZLinkSerialExecutionQueue(
                serialExecutor, ZLinkExecutionLanePolicy.spot())));
    }

    private List<ZLinkSerialExecutionQueue> timerSnapshot() {
        return inStateLane(() -> List.copyOf(timerQueues.values()));
    }

    private <T> T inStateLane(Supplier<T> operation) {
        try {
            return stateLane.runAsync(operation).toCompletableFuture().join();
        } catch (CompletionException failure) {
            Throwable cause = failure.getCause();
            if (cause instanceof RuntimeException runtimeFailure) throw runtimeFailure;
            if (cause instanceof Error error) throw error;
            throw failure;
        }
    }
}
