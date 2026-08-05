package systems.zlink.framework.runtime.internal.relocation;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.BooleanSupplier;
import java.util.function.Supplier;
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue;

/**
 * Seals a fixed inventory of serial lanes as one generation.
 *
 * <p>The exact {@link Seal} instance is the fence. A failed partial seal is
 * rolled back before this method returns, so a caller never observes a
 * partially sealed participant inventory.
 */
public final class ZLinkCompositeRelocationBarrier {
    private long nextGeneration = 1L;
    private Seal active;

    public synchronized Optional<Seal> trySeal(
        Map<String, ZLinkAsyncSerialQueue> lanes) {
        if (active != null) {
            return Optional.empty();
        }
        if (lanes == null || lanes.isEmpty()) {
            throw new IllegalArgumentException(
                "at least one relocation lane is required");
        }
        if (nextGeneration == Long.MAX_VALUE) {
            throw new IllegalStateException(
                "composite relocation generation exhausted");
        }
        LinkedHashMap<String, ZLinkAsyncSerialQueue> laneSnapshot =
            new LinkedHashMap<>();
        LinkedHashMap<String, ZLinkAsyncSerialQueue.RelocationSeal> seals =
            new LinkedHashMap<>();
        try {
            for (Map.Entry<String, ZLinkAsyncSerialQueue> lane
                : lanes.entrySet()) {
                String laneId = requireLaneId(lane.getKey());
                ZLinkAsyncSerialQueue queue =
                    java.util.Objects.requireNonNull(
                        lane.getValue(), "relocation lane");
                if (laneSnapshot.putIfAbsent(laneId, queue) != null) {
                    throw new IllegalArgumentException(
                        "duplicate relocation lane: " + laneId);
                }
                Optional<ZLinkAsyncSerialQueue.RelocationSeal> sealed =
                    queue.trySealRelocation();
                if (sealed.isEmpty()) {
                    rollback(laneSnapshot, seals);
                    return Optional.empty();
                }
                seals.put(laneId, sealed.get());
            }
        } catch (RuntimeException failure) {
            rollback(laneSnapshot, seals);
            throw failure;
        }
        Seal result = new Seal(
            nextGeneration++,
            java.util.Collections.unmodifiableMap(
                new LinkedHashMap<>(laneSnapshot)),
            java.util.Collections.unmodifiableMap(
                new LinkedHashMap<>(seals)));
        active = result;
        return Optional.of(result);
    }

    /**
     * Waits until every lane reaches the next turn boundary and seals the
     * fixed lane inventory as one generation. Queued application records do
     * not run while the boundary is being acquired.
     */
    public CompletionStage<Optional<Seal>> sealAtTurnBoundary(
        Map<String, ZLinkAsyncSerialQueue> lanes,
        BooleanSupplier cancelled) {
        java.util.Objects.requireNonNull(cancelled, "cancelled");
        LinkedHashMap<String, ZLinkAsyncSerialQueue> snapshot =
            validateLanes(lanes);
        return attemptTurnBoundary(snapshot, cancelled);
    }

    private CompletionStage<Optional<Seal>> attemptTurnBoundary(
        LinkedHashMap<String, ZLinkAsyncSerialQueue> lanes,
        BooleanSupplier cancelled) {
        if (cancelled.getAsBoolean()) {
            return CompletableFuture.completedFuture(Optional.empty());
        }
        LinkedHashMap<String, ZLinkAsyncSerialQueue.RelocationBoundary>
            boundaries = new LinkedHashMap<>();
        for (Map.Entry<String, ZLinkAsyncSerialQueue> lane
            : lanes.entrySet()) {
            Optional<ZLinkAsyncSerialQueue.RelocationBoundary> boundary =
                lane.getValue().reserveRelocationTurnBoundary();
            if (boundary.isEmpty()) {
                release(boundaries);
                return awaitFinished(boundaries).thenApply(
                    ignored -> Optional.empty());
            }
            boundaries.put(lane.getKey(), boundary.orElseThrow());
        }
        CompletableFuture<?>[] reached = boundaries.values().stream()
            .map(boundary -> boundary.reached().toCompletableFuture())
            .toArray(CompletableFuture[]::new);
        return CompletableFuture.allOf(reached).thenCompose(ignored -> {
            if (cancelled.getAsBoolean()) {
                release(boundaries);
                return awaitFinished(boundaries).thenApply(
                    finished -> Optional.empty());
            }
            Optional<Seal> sealed =
                trySealAtReservedBoundaries(lanes, boundaries);
            release(boundaries);
            return awaitFinished(boundaries).thenCompose(finished -> {
                if (sealed.isPresent() || cancelled.getAsBoolean()) {
                    return CompletableFuture.completedFuture(sealed);
                }
                return attemptTurnBoundary(lanes, cancelled);
            });
        });
    }

    private synchronized Optional<Seal> trySealAtReservedBoundaries(
        Map<String, ZLinkAsyncSerialQueue> lanes,
        Map<String, ZLinkAsyncSerialQueue.RelocationBoundary> boundaries) {
        if (active != null) {
            return Optional.empty();
        }
        if (nextGeneration == Long.MAX_VALUE) {
            throw new IllegalStateException(
                "composite relocation generation exhausted");
        }
        LinkedHashMap<String, ZLinkAsyncSerialQueue.RelocationSeal> seals =
            new LinkedHashMap<>();
        try {
            for (Map.Entry<String, ZLinkAsyncSerialQueue> lane
                : lanes.entrySet()) {
                Optional<ZLinkAsyncSerialQueue.RelocationSeal> sealed =
                    lane.getValue().trySealRelocation(
                        boundaries.get(lane.getKey()));
                if (sealed.isEmpty()) {
                    rollback(lanes, seals);
                    return Optional.empty();
                }
                seals.put(lane.getKey(), sealed.orElseThrow());
            }
        } catch (RuntimeException failure) {
            rollback(lanes, seals);
            throw failure;
        }
        Seal result = new Seal(
            nextGeneration++,
            java.util.Collections.unmodifiableMap(
                new LinkedHashMap<>(lanes)),
            java.util.Collections.unmodifiableMap(
                new LinkedHashMap<>(seals)));
        active = result;
        return Optional.of(result);
    }

    public synchronized boolean abort(Seal seal) {
        if (seal == null || seal != active) {
            return false;
        }
        List<String> laneIds =
            new ArrayList<>(seal.lanes.keySet());
        java.util.Collections.reverse(laneIds);
        boolean restored = true;
        for (String laneId : laneIds) {
            restored &= seal.lanes.get(laneId).abortRelocation(
                seal.seals.get(laneId));
        }
        if (!restored) {
            throw new IllegalStateException(
                "composite relocation abort lost a lane fence");
        }
        active = null;
        return true;
    }

    public synchronized Optional<Map<String, List<
        ZLinkAsyncSerialQueue.QueuedRecord>>> commit(Seal seal) {
        if (seal == null || seal != active) {
            return Optional.empty();
        }
        LinkedHashMap<String, List<ZLinkAsyncSerialQueue.QueuedRecord>>
            held = new LinkedHashMap<>();
        for (Map.Entry<String, ZLinkAsyncSerialQueue> lane
            : seal.lanes.entrySet()) {
            List<ZLinkAsyncSerialQueue.QueuedRecord> records =
                lane.getValue().commitRelocation(
                    seal.seals.get(lane.getKey()))
                    .orElseThrow(() -> new IllegalStateException(
                        "composite relocation commit lost a lane fence"));
            held.put(lane.getKey(), records);
        }
        active = null;
        return Optional.of(Map.copyOf(held));
    }

    public synchronized Optional<Map<String, List<
        ZLinkAsyncSerialQueue.QueuedRecord>>> freezeIngress(Seal seal) {
        if (seal == null || seal != active) {
            return Optional.empty();
        }
        LinkedHashMap<String, List<ZLinkAsyncSerialQueue.QueuedRecord>> held =
            new LinkedHashMap<>();
        for (Map.Entry<String, ZLinkAsyncSerialQueue> lane
            : seal.lanes.entrySet()) {
            held.put(
                lane.getKey(),
                lane.getValue().freezeRelocationIngress(
                    seal.seals.get(lane.getKey()))
                    .orElseThrow(() -> new IllegalStateException(
                        "composite relocation freeze lost a lane fence")));
        }
        return Optional.of(java.util.Collections.unmodifiableMap(held));
    }

    public synchronized <T> CompletionStage<T> runCapture(
        Seal seal,
        Supplier<CompletionStage<T>> capture) {
        if (seal == null || seal != active) {
            throw new IllegalStateException(
                "capture requires the active relocation barrier generation");
        }
        return java.util.Objects.requireNonNull(
            capture.get(), "capture result");
    }

    /** Returns the immutable accepted journal fixed by this active seal. */
    public synchronized Optional<Map<String, List<
        ZLinkAsyncSerialQueue.QueuedRecord>>> captured(Seal seal) {
        if (seal == null || seal != active) {
            return Optional.empty();
        }
        LinkedHashMap<String, List<ZLinkAsyncSerialQueue.QueuedRecord>>
            captured = new LinkedHashMap<>();
        for (String laneId : seal.lanes.keySet()) {
            captured.put(
                laneId,
                seal.seals.get(laneId).captured());
        }
        return Optional.of(java.util.Collections.unmodifiableMap(captured));
    }

    private static void rollback(
        Map<String, ZLinkAsyncSerialQueue> lanes,
        Map<String, ZLinkAsyncSerialQueue.RelocationSeal> seals) {
        List<String> laneIds = new ArrayList<>(seals.keySet());
        java.util.Collections.reverse(laneIds);
        for (String laneId : laneIds) {
            if (!lanes.get(laneId).abortRelocation(seals.get(laneId))) {
                throw new IllegalStateException(
                    "partial relocation seal rollback lost a lane fence");
            }
        }
    }

    private static LinkedHashMap<String, ZLinkAsyncSerialQueue>
        validateLanes(Map<String, ZLinkAsyncSerialQueue> lanes) {
        if (lanes == null || lanes.isEmpty()) {
            throw new IllegalArgumentException(
                "at least one relocation lane is required");
        }
        LinkedHashMap<String, ZLinkAsyncSerialQueue> snapshot =
            new LinkedHashMap<>();
        lanes.forEach((laneId, queue) -> {
            String required = requireLaneId(laneId);
            if (snapshot.putIfAbsent(
                    required,
                    java.util.Objects.requireNonNull(
                        queue, "relocation lane")) != null) {
                throw new IllegalArgumentException(
                    "duplicate relocation lane: " + required);
            }
        });
        return snapshot;
    }

    private static void release(
        Map<String, ZLinkAsyncSerialQueue.RelocationBoundary> boundaries) {
        boundaries.values().forEach(
            ZLinkAsyncSerialQueue.RelocationBoundary::release);
    }

    private static CompletionStage<Void> awaitFinished(
        Map<String, ZLinkAsyncSerialQueue.RelocationBoundary> boundaries) {
        CompletableFuture<?>[] finished = boundaries.values().stream()
            .map(boundary -> boundary.finished().toCompletableFuture())
            .toArray(CompletableFuture[]::new);
        return CompletableFuture.allOf(finished);
    }

    private static String requireLaneId(String laneId) {
        if (laneId == null || laneId.isBlank()) {
            throw new IllegalArgumentException(
                "relocation lane id is required");
        }
        return laneId;
    }

    public static final class Seal {
        private final long generation;
        private final Map<String, ZLinkAsyncSerialQueue> lanes;
        private final Map<String, ZLinkAsyncSerialQueue.RelocationSeal> seals;

        private Seal(
            long generation,
            Map<String, ZLinkAsyncSerialQueue> lanes,
            Map<String, ZLinkAsyncSerialQueue.RelocationSeal> seals) {
            this.generation = generation;
            this.lanes = lanes;
            this.seals = seals;
        }

        public long generation() {
            return generation;
        }

        public List<String> laneIds() {
            return List.copyOf(lanes.keySet());
        }
    }
}
