package systems.zlink.framework.runtime.internal.relocation;
import java.util.Collections;
import java.util.Objects;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CompletionException;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.BooleanSupplier;
import java.util.function.Supplier;
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

/**
 * Seals a fixed inventory of serial lanes as one generation.
 *
 * <p>The exact {@link Seal} instance is the fence. A failed partial seal is
 * rolled back before this method returns, so a caller never observes a
 * partially sealed participant inventory.
 */
public final class ZLinkCompositeRelocationBarrier {
    // Active/generation state is lane-owned. A transition placeholder preserves
    // monitor-era admission while queue fence calls and rollback run outside it.
    private final ZLinkStateLane stateLane = new ZLinkStateLane();
    private long nextGeneration = 1L;
    private Seal active;
    private boolean committing;
    private CompletableFuture<Void> transition;

    private <T> T inStateLane(Supplier<T> work) {
        try {
            return stateLane.runAsync(work).toCompletableFuture().join();
        } catch (CompletionException failure) {
            Throwable cause = failure.getCause();
            if (cause instanceof RuntimeException runtimeFailure) {
                throw runtimeFailure;
            }
            if (cause instanceof Error error) {
                throw error;
            }
            throw failure;
        }
    }

    private void awaitTransition(CompletableFuture<Void> pending) {
        pending.handle((ignored, failure) -> null).join();
    }

    private void finishTransition(CompletableFuture<Void> finished) {
        inStateLane(() -> {
            if (transition == finished) {
                transition = null;
            }
            return null;
        });
        // Non-async CompletableFuture dependents would otherwise inherit lane
        // ownership from the state-finalization turn.
        finished.completeAsync(() -> null);
    }

    public Optional<Seal> trySeal(
        Map<String, ZLinkAsyncSerialQueue> lanes) {
        while (true) {
            SealStart start = inStateLane(() -> {
                if (transition != null) {
                    return new SealStart(null, transition);
                }
                if (active != null || committing) {
                    return new SealStart(null, null);
                }
                if (lanes == null || lanes.isEmpty()) {
                    throw new IllegalArgumentException(
                        "at least one relocation lane is required");
                }
                if (nextGeneration == Long.MAX_VALUE) {
                    throw new IllegalStateException(
                        "composite relocation generation exhausted");
                }
                LinkedHashMap<String, ZLinkAsyncSerialQueue> snapshot =
                    validateLanes(lanes);
                CompletableFuture<Void> pending = new CompletableFuture<>();
                transition = pending;
                return new SealStart(snapshot, pending);
            });
            if (start.lanes() == null) {
                if (start.pending() == null) {
                    return Optional.empty();
                }
                awaitTransition(start.pending());
                continue;
            }
            return sealOutsideTurn(start.lanes(), null, start.pending());
        }
    }

    private Optional<Seal> sealOutsideTurn(
        LinkedHashMap<String, ZLinkAsyncSerialQueue> laneSnapshot,
        Map<String, ZLinkAsyncSerialQueue.RelocationBoundary> boundaries,
        CompletableFuture<Void> pending) {
        LinkedHashMap<String, ZLinkAsyncSerialQueue.RelocationSeal> seals =
            new LinkedHashMap<>();
        try {
            for (Map.Entry<String, ZLinkAsyncSerialQueue> lane
                : laneSnapshot.entrySet()) {
                Optional<ZLinkAsyncSerialQueue.RelocationSeal> sealed =
                    boundaries == null
                        ? lane.getValue().trySealRelocation()
                        : lane.getValue().trySealRelocation(
                            boundaries.get(lane.getKey()));
                if (sealed.isEmpty()) {
                    try {
                        rollback(laneSnapshot, seals);
                    } finally {
                        finishTransition(pending);
                    }
                    return Optional.empty();
                }
                seals.put(lane.getKey(), sealed.get());
            }
        } catch (RuntimeException failure) {
            try {
                rollback(laneSnapshot, seals);
            } finally {
                finishTransition(pending);
            }
            throw failure;
        }
        try {
            Seal result = inStateLane(() -> {
                Seal established = new Seal(
                    nextGeneration++,
                    Collections.unmodifiableMap(new LinkedHashMap<>(laneSnapshot)),
                    Collections.unmodifiableMap(new LinkedHashMap<>(seals)));
                active = established;
                return established;
            });
            return Optional.of(result);
        } finally {
            finishTransition(pending);
        }
    }

    /**
     * Waits until every lane reaches the next turn boundary and seals the
     * fixed lane inventory as one generation. Queued application records do
     * not run while the boundary is being acquired.
     */
    public CompletionStage<Optional<Seal>> sealAtTurnBoundary(
        Map<String, ZLinkAsyncSerialQueue> lanes,
        BooleanSupplier cancelled) {
        Objects.requireNonNull(cancelled, "cancelled");
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

    private Optional<Seal> trySealAtReservedBoundaries(
        Map<String, ZLinkAsyncSerialQueue> lanes,
        Map<String, ZLinkAsyncSerialQueue.RelocationBoundary> boundaries) {
        while (true) {
            SealStart start = inStateLane(() -> {
                if (transition != null) {
                    return new SealStart(null, transition);
                }
                if (active != null || committing) {
                    return new SealStart(null, null);
                }
                if (nextGeneration == Long.MAX_VALUE) {
                    throw new IllegalStateException(
                        "composite relocation generation exhausted");
                }
                CompletableFuture<Void> pending = new CompletableFuture<>();
                transition = pending;
                return new SealStart(new LinkedHashMap<>(lanes), pending);
            });
            if (start.lanes() == null) {
                if (start.pending() == null) {
                    return Optional.empty();
                }
                awaitTransition(start.pending());
                continue;
            }
            return sealOutsideTurn(start.lanes(), boundaries, start.pending());
        }
    }

    public boolean abort(Seal seal) {
        while (true) {
            AbortStart start = inStateLane(() -> {
                if (transition != null) {
                    return new AbortStart(null, transition);
                }
                if (committing || seal == null || seal != active) {
                    return new AbortStart(null, null);
                }
                CompletableFuture<Void> pending = new CompletableFuture<>();
                transition = pending;
                return new AbortStart(seal, pending);
            });
            if (start.seal() == null) {
                if (start.pending() == null) {
                    return false;
                }
                awaitTransition(start.pending());
                continue;
            }
            List<String> laneIds = new ArrayList<>(start.seal().lanes.keySet());
            Collections.reverse(laneIds);
            boolean restored = true;
            try {
                for (String laneId : laneIds) {
                    restored &= start.seal().lanes.get(laneId).abortRelocation(
                        start.seal().seals.get(laneId));
                }
                if (!restored) {
                    throw new IllegalStateException(
                        "composite relocation abort lost a lane fence");
                }
                inStateLane(() -> {
                    if (active == start.seal()) {
                        active = null;
                    }
                    return null;
                });
                return true;
            } finally {
                finishTransition(start.pending());
            }
        }
    }

    public Optional<Map<String, List<ZLinkAsyncSerialQueue.QueuedRecord>>>
        commit(Seal seal) {
        Optional<RelocationCommit> retained = retainCommit(seal);
        if (retained.isEmpty()) {
            return Optional.empty();
        }
        RelocationCommit commit = retained.orElseThrow();
        RelocationCommit.Cut cut;
        do {
            cut = commit.cut();
        } while (!commit.tryEstablishAndFinishCapture(cut));
        commit.complete();
        return Optional.of(cut.records());
    }

    public Optional<RelocationCommit> retainCommit(Seal seal) {
        LinkedHashMap<String, ZLinkAsyncSerialQueue> lanes;
        while (true) {
            RetainStart start = inStateLane(() -> {
                if (transition != null) {
                    return new RetainStart(null, transition);
                }
                if (committing || seal == null || seal != active) {
                    return new RetainStart(null, null);
                }
                committing = true;
                return new RetainStart(new LinkedHashMap<>(seal.lanes), null);
            });
            if (start.lanes() != null) {
                lanes = start.lanes();
                break;
            }
            if (start.pending() == null) {
                return Optional.empty();
            }
            awaitTransition(start.pending());
        }
        LinkedHashMap<String, List<ZLinkAsyncSerialQueue.QueuedRecord>>
            held = new LinkedHashMap<>();
        List<ZLinkRetainedSerialQueueCommit.Commit> retained =
            new ArrayList<>();
        try {
            for (Map.Entry<String, ZLinkAsyncSerialQueue> lane
                : lanes.entrySet()) {
                ZLinkRetainedSerialQueueCommit.Commit committed =
                    ZLinkRetainedSerialQueueCommit.retain(
                        lane.getValue(), seal.seals.get(lane.getKey()))
                        .orElseThrow(() -> new IllegalStateException(
                            "composite relocation commit lost a lane fence"));
                retained.add(committed);
                held.put(lane.getKey(), committed.records());
            }
        } catch (RuntimeException failure) {
            inStateLane(() -> {
                committing = false;
                return null;
            });
            throw failure;
        }
        boolean retainedActive = inStateLane(() -> {
            if (active != seal) {
                committing = false;
                return false;
            }
            active = null;
            committing = false;
            return true;
        });
        if (!retainedActive) {
            return Optional.empty();
        }
        return Optional.of(new RelocationCommit(held, retained));
    }

    public Optional<Map<String, List<
        ZLinkAsyncSerialQueue.QueuedRecord>>> freezeIngress(Seal seal) {
        while (true) {
            FreezeStart start = inStateLane(() -> {
                if (transition != null) {
                    return new FreezeStart(null, transition);
                }
                if (committing || seal == null || seal != active) {
                    return new FreezeStart(null, null);
                }
                CompletableFuture<Void> pending = new CompletableFuture<>();
                transition = pending;
                return new FreezeStart(seal, pending);
            });
            if (start.seal() == null) {
                if (start.pending() == null) {
                    return Optional.empty();
                }
                awaitTransition(start.pending());
                continue;
            }
            try {
                LinkedHashMap<String, List<ZLinkAsyncSerialQueue.QueuedRecord>> held =
                    new LinkedHashMap<>();
                for (Map.Entry<String, ZLinkAsyncSerialQueue> lane
                    : start.seal().lanes.entrySet()) {
                    held.put(lane.getKey(), lane.getValue().freezeRelocationIngress(
                        start.seal().seals.get(lane.getKey())).orElseThrow(() ->
                            new IllegalStateException(
                                "composite relocation freeze lost a lane fence")));
                }
                return Optional.of(Collections.unmodifiableMap(held));
            } finally {
                finishTransition(start.pending());
            }
        }
    }

    public <T> CompletionStage<T> runCapture(
        Seal seal,
        Supplier<CompletionStage<T>> capture) {
        while (true) {
            CaptureStart start = inStateLane(() -> {
                if (transition != null) {
                    return new CaptureStart(false, transition);
                }
                if (committing || seal == null || seal != active) {
                    throw new IllegalStateException(
                        "capture requires the active relocation barrier generation");
                }
                return new CaptureStart(true, null);
            });
            if (start.accepted()) {
                break;
            }
            awaitTransition(start.pending());
        }
        return Objects.requireNonNull(
            capture.get(), "capture result");
    }

    /** Returns the immutable accepted journal fixed by this active seal. */
    public Optional<Map<String, List<
        ZLinkAsyncSerialQueue.QueuedRecord>>> captured(Seal seal) {
        List<Map.Entry<String, ZLinkAsyncSerialQueue.RelocationSeal>> seals;
        while (true) {
            CapturedState state = inStateLane(() -> {
                if (transition != null) {
                    return new CapturedState(null, transition);
                }
                if (seal == null || seal != active) {
                    return new CapturedState(null, null);
                }
                return new CapturedState(seal.lanes.keySet().stream().map(laneId ->
                    Map.entry(laneId, seal.seals.get(laneId))).toList(), null);
            });
            if (state.seals() != null) {
                seals = state.seals();
                break;
            }
            if (state.pending() == null) {
                return Optional.empty();
            }
            awaitTransition(state.pending());
        }
        LinkedHashMap<String, List<ZLinkAsyncSerialQueue.QueuedRecord>>
            captured = new LinkedHashMap<>();
        for (Map.Entry<String, ZLinkAsyncSerialQueue.RelocationSeal> entry : seals) {
            captured.put(entry.getKey(), entry.getValue().captured());
        }
        return Optional.of(Collections.unmodifiableMap(captured));
    }

    private static void rollback(
        Map<String, ZLinkAsyncSerialQueue> lanes,
        Map<String, ZLinkAsyncSerialQueue.RelocationSeal> seals) {
        List<String> laneIds = new ArrayList<>(seals.keySet());
        Collections.reverse(laneIds);
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
                    Objects.requireNonNull(
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

    private record SealStart(
        LinkedHashMap<String, ZLinkAsyncSerialQueue> lanes,
        CompletableFuture<Void> pending) {
    }

    private record AbortStart(Seal seal, CompletableFuture<Void> pending) {
    }

    private record FreezeStart(Seal seal, CompletableFuture<Void> pending) {
    }

    private record RetainStart(
        LinkedHashMap<String, ZLinkAsyncSerialQueue> lanes,
        CompletableFuture<Void> pending) {
    }

    private record CapturedState(
        List<Map.Entry<String, ZLinkAsyncSerialQueue.RelocationSeal>> seals,
        CompletableFuture<Void> pending) {
    }

    private record CaptureStart(boolean accepted, CompletableFuture<Void> pending) {
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

    public static final class RelocationCommit {
        private final Map<String, List<ZLinkAsyncSerialQueue.QueuedRecord>> records;
        private final List<ZLinkRetainedSerialQueueCommit.Commit> lanes;
        private final List<String> laneIds;
        private final AtomicBoolean completed = new AtomicBoolean();

        private RelocationCommit(
            Map<String, List<ZLinkAsyncSerialQueue.QueuedRecord>> records,
            List<ZLinkRetainedSerialQueueCommit.Commit> lanes) {
            this.records = Collections.unmodifiableMap(
                new LinkedHashMap<>(records));
            this.lanes = List.copyOf(lanes);
            this.laneIds = List.copyOf(records.keySet());
        }

        public Map<String, List<ZLinkAsyncSerialQueue.QueuedRecord>> records() {
            return records;
        }

        /** Captures one sequence-stable suffix cut across every lane. */
        public Cut cut() {
            LinkedHashMap<String, List<ZLinkAsyncSerialQueue.QueuedRecord>>
                current = new LinkedHashMap<>();
            List<ZLinkRetainedSerialQueueCommit.Cut> cuts = new ArrayList<>();
            for (int index = 0; index < lanes.size(); index++) {
                ZLinkRetainedSerialQueueCommit.Cut cut = lanes.get(index).cut();
                cuts.add(cut);
                current.put(laneIds.get(index), cut.records());
            }
            return new Cut(
                Collections.unmodifiableMap(current),
                List.copyOf(cuts));
        }

        /**
         * Detaches all lanes only if no lane accepted ingress after this cut.
         */
        public boolean tryFinishCapture(Cut cut) {
            Objects.requireNonNull(cut, "cut");
            return ZLinkRetainedSerialQueueCommit.finishCapture(
                lanes, cut.lanes);
        }

        public boolean tryEstablishDurableCut(Cut cut) {
            Objects.requireNonNull(cut, "cut");
            return ZLinkRetainedSerialQueueCommit.establishDurableCut(
                lanes, cut.lanes);
        }

        public boolean tryEstablishAndFinishCapture(Cut cut) {
            Objects.requireNonNull(cut, "cut");
            return ZLinkRetainedSerialQueueCommit.establishAndFinishCapture(
                lanes, cut.lanes);
        }

        public boolean abort() {
            return ZLinkRetainedSerialQueueCommit.abortRetained(lanes);
        }

        public void complete() {
            if (completed.compareAndSet(false, true)) {
                lanes.forEach(ZLinkRetainedSerialQueueCommit.Commit::complete);
            }
        }

        public static final class Cut {
            private final Map<String, List<
                ZLinkAsyncSerialQueue.QueuedRecord>> records;
            private final List<ZLinkRetainedSerialQueueCommit.Cut> lanes;

            private Cut(
                Map<String, List<ZLinkAsyncSerialQueue.QueuedRecord>> records,
                List<ZLinkRetainedSerialQueueCommit.Cut> lanes) {
                this.records = records;
                this.lanes = lanes;
            }

            public Map<String, List<ZLinkAsyncSerialQueue.QueuedRecord>>
                records() {
                return records;
            }
        }
    }
}
