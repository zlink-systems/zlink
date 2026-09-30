package systems.zlink.framework.runtime.internal.relocation;

import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.BooleanSupplier;
import java.util.function.Supplier;

/**
 * Seals a fixed inventory of serial lanes as one generation.
 *
 * <p>The exact {@link Seal} instance is the fence. A failed partial seal is rolled back before this
 * method returns, so a caller never observes a partially sealed participant inventory.
 */
public final class ZLinkCompositeRelocationBarrier {
    // Active/generation state is lane-owned. A transition placeholder preserves
    // monitor-era admission while queue fence calls and rollback run outside it.
    private final ZLinkStateLane stateLane = new ZLinkStateLane();
    private long nextGeneration = 1L;
    private Seal active;
    private boolean committing;
    private CompletableFuture<Void> transition;

    private CompletionStage<Void> awaitTransition(CompletableFuture<Void> pending) {
        return pending.handle((ignored, failure) -> null);
    }

    private CompletionStage<Void> finishTransition(CompletableFuture<Void> finished) {
        return stateLane
                .runAsync(
                        () -> {
                            if (transition == finished) {
                                transition = null;
                            }
                        })
                // Dependents must not inherit state-lane ownership.
                .whenCompleteAsync(
                        (ignored, failure) -> {
                            if (failure == null) {
                                finished.complete(null);
                            } else {
                                finished.completeExceptionally(failure);
                            }
                        });
    }

    public CompletionStage<Optional<Seal>> trySeal(Map<String, ZLinkSerialExecutionQueue> lanes) {
        return trySeal(lanes, Map.of());
    }

    public CompletionStage<Optional<Seal>> trySeal(
            Map<String, ZLinkSerialExecutionQueue> lanes,
            Map<String, ZLinkSerialExecutionQueue.ActiveTurnSealHandle> activeTurns) {
        return stateLane
                .runNowOrQueue(
                        () -> {
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
                            LinkedHashMap<String, ZLinkSerialExecutionQueue> snapshot =
                                    validateLanes(lanes);
                            CompletableFuture<Void> pending = new CompletableFuture<>();
                            transition = pending;
                            return new SealStart(snapshot, pending);
                        })
                .thenCompose(
                        start -> {
                            if (start.lanes() == null) {
                                return start.pending() == null
                                        ? CompletableFuture.completedFuture(Optional.empty())
                                        : awaitTransition(start.pending())
                                                .thenCompose(
                                                        ignored -> trySeal(lanes, activeTurns));
                            }
                            return sealOutsideTurn(
                                    start.lanes(), null, activeTurns, start.pending());
                        });
    }

    private CompletionStage<Optional<Seal>> sealOutsideTurn(
            LinkedHashMap<String, ZLinkSerialExecutionQueue> laneSnapshot,
            Map<String, ZLinkSerialExecutionQueue.RelocationBoundary> boundaries,
            Map<String, ZLinkSerialExecutionQueue.ActiveTurnSealHandle> activeTurns,
            CompletableFuture<Void> pending) {
        LinkedHashMap<String, ZLinkSerialExecutionQueue.RelocationSeal> seals =
                new LinkedHashMap<>();
        try {
            for (Map.Entry<String, ZLinkSerialExecutionQueue> lane : laneSnapshot.entrySet()) {
                Optional<ZLinkSerialExecutionQueue.RelocationSeal> sealed =
                        boundaries != null
                                ? lane.getValue().trySealRelocation(boundaries.get(lane.getKey()))
                                : activeTurns.containsKey(lane.getKey())
                                        ? lane.getValue()
                                                .trySealRelocation(activeTurns.get(lane.getKey()))
                                        : lane.getValue().trySealRelocation();
                if (sealed.isEmpty()) {
                    rollback(laneSnapshot, seals);
                    return finishTransition(pending).thenApply(ignored -> Optional.empty());
                }
                seals.put(lane.getKey(), sealed.get());
            }
        } catch (RuntimeException failure) {
            try {
                rollback(laneSnapshot, seals);
            } catch (RuntimeException rollbackFailure) {
                failure.addSuppressed(rollbackFailure);
            }
            return finishTransition(pending)
                    .thenCompose(ignored -> CompletableFuture.failedFuture(failure));
        }
        try {
            return stateLane
                    .runAsync(
                            () -> {
                                Seal established =
                                        new Seal(
                                                nextGeneration++,
                                                Collections.unmodifiableMap(
                                                        new LinkedHashMap<>(laneSnapshot)),
                                                Collections.unmodifiableMap(
                                                        new LinkedHashMap<>(seals)));
                                active = established;
                                return established;
                            })
                    .handle(
                            (seal, failure) ->
                                    finishTransition(pending)
                                            .thenCompose(
                                                    ignored ->
                                                            failure == null
                                                                    ? CompletableFuture
                                                                            .completedFuture(
                                                                                    Optional.of(
                                                                                            seal))
                                                                    : CompletableFuture
                                                                            .<Optional<Seal>>
                                                                                    failedFuture(
                                                                                            failure)))
                    .thenCompose(result -> result);
        } catch (RuntimeException | Error failure) {
            return finishTransition(pending)
                    .thenCompose(ignored -> CompletableFuture.failedFuture(failure));
        }
    }

    /**
     * Waits until every lane reaches the next turn boundary and seals the fixed lane inventory as
     * one generation. Queued application records do not run while the boundary is being acquired.
     */
    public CompletionStage<Optional<Seal>> sealAtTurnBoundary(
            Map<String, ZLinkSerialExecutionQueue> lanes, BooleanSupplier cancelled) {
        Objects.requireNonNull(cancelled, "cancelled");
        LinkedHashMap<String, ZLinkSerialExecutionQueue> snapshot = validateLanes(lanes);
        return attemptTurnBoundary(snapshot, cancelled);
    }

    private CompletionStage<Optional<Seal>> attemptTurnBoundary(
            LinkedHashMap<String, ZLinkSerialExecutionQueue> lanes, BooleanSupplier cancelled) {
        if (cancelled.getAsBoolean()) {
            return CompletableFuture.completedFuture(Optional.empty());
        }
        LinkedHashMap<String, ZLinkSerialExecutionQueue.RelocationBoundary> boundaries =
                new LinkedHashMap<>();
        for (Map.Entry<String, ZLinkSerialExecutionQueue> lane : lanes.entrySet()) {
            Optional<ZLinkSerialExecutionQueue.RelocationBoundary> boundary =
                    lane.getValue().reserveRelocationTurnBoundary();
            if (boundary.isEmpty()) {
                release(boundaries);
                return awaitFinished(boundaries).thenApply(ignored -> Optional.empty());
            }
            boundaries.put(lane.getKey(), boundary.orElseThrow());
        }
        CompletableFuture<?>[] reached =
                boundaries.values().stream()
                        .map(boundary -> boundary.reached().toCompletableFuture())
                        .toArray(CompletableFuture[]::new);
        return CompletableFuture.allOf(reached)
                .thenCompose(
                        ignored -> {
                            if (cancelled.getAsBoolean()) {
                                release(boundaries);
                                return awaitFinished(boundaries)
                                        .thenApply(finished -> Optional.empty());
                            }
                            return trySealAtReservedBoundaries(lanes, boundaries)
                                    .whenComplete(
                                            (result, failure) -> {
                                                release(boundaries);
                                            })
                                    .thenCompose(
                                            sealResult -> {
                                                return awaitFinished(boundaries)
                                                        .thenCompose(
                                                                finished -> {
                                                                    if (sealResult.isPresent()
                                                                            || cancelled
                                                                                    .getAsBoolean()) {
                                                                        return CompletableFuture
                                                                                .completedFuture(
                                                                                        sealResult);
                                                                    }
                                                                    return attemptTurnBoundary(
                                                                            lanes, cancelled);
                                                                });
                                            });
                        });
    }

    private CompletionStage<Optional<Seal>> trySealAtReservedBoundaries(
            Map<String, ZLinkSerialExecutionQueue> lanes,
            Map<String, ZLinkSerialExecutionQueue.RelocationBoundary> boundaries) {
        return stateLane
                .runAsync(
                        () -> {
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
                        })
                .thenComposeAsync(
                        start -> {
                            if (start.lanes() == null) {
                                return start.pending() == null
                                        ? CompletableFuture.completedFuture(Optional.empty())
                                        : awaitTransition(start.pending())
                                                .thenCompose(
                                                        ignored ->
                                                                trySealAtReservedBoundaries(
                                                                        lanes, boundaries));
                            }
                            return sealOutsideTurn(
                                    start.lanes(), boundaries, Map.of(), start.pending());
                        });
    }

    public CompletionStage<Boolean> abort(Seal seal) {
        return stateLane
                .runAsync(
                        () -> {
                            if (transition != null) {
                                return new AbortStart(null, transition);
                            }
                            if (committing || seal == null || seal != active) {
                                return new AbortStart(null, null);
                            }
                            CompletableFuture<Void> pending = new CompletableFuture<>();
                            transition = pending;
                            return new AbortStart(seal, pending);
                        })
                .thenComposeAsync(start -> abortStarted(seal, start));
    }

    private CompletionStage<Boolean> abortStarted(Seal seal, AbortStart start) {
        if (start.seal() == null) {
            return start.pending() == null
                    ? CompletableFuture.completedFuture(false)
                    : awaitTransition(start.pending()).thenCompose(ignored -> abort(seal));
        }
        List<String> laneIds = new ArrayList<>(start.seal().lanes.keySet());
        Collections.reverse(laneIds);
        boolean restored = true;
        try {
            for (String laneId : laneIds) {
                restored &=
                        start.seal()
                                .lanes
                                .get(laneId)
                                .abortRelocation(start.seal().seals.get(laneId));
            }
            if (!restored) {
                throw new IllegalStateException("composite relocation abort lost a lane fence");
            }
            return stateLane
                    .runAsync(
                            () -> {
                                if (active == start.seal()) {
                                    active = null;
                                }
                            })
                    .handle(
                            (ignored, failure) ->
                                    finishTransition(start.pending())
                                            .thenCompose(
                                                    finished ->
                                                            failure == null
                                                                    ? CompletableFuture
                                                                            .completedFuture(true)
                                                                    : CompletableFuture
                                                                            .<Boolean>failedFuture(
                                                                                    failure)))
                    .thenCompose(result -> result);
        } catch (RuntimeException | Error failure) {
            return finishTransition(start.pending())
                    .thenCompose(ignored -> CompletableFuture.failedFuture(failure));
        }
    }

    public CompletionStage<Optional<Map<String, List<ZLinkSerialExecutionQueue.QueuedRecord>>>>
            commit(Seal seal) {
        return retainCommit(seal)
                .thenApplyAsync(
                        retained -> {
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
                        });
    }

    public CompletionStage<Optional<RelocationCommit>> retainCommit(Seal seal) {
        return stateLane
                .runAsync(
                        () -> {
                            if (transition != null) {
                                return new RetainStart(null, transition);
                            }
                            if (committing || seal == null || seal != active) {
                                return new RetainStart(null, null);
                            }
                            committing = true;
                            return new RetainStart(new LinkedHashMap<>(seal.lanes), null);
                        })
                .thenComposeAsync(start -> retainCommitStarted(seal, start));
    }

    private CompletionStage<Optional<RelocationCommit>> retainCommitStarted(
            Seal seal, RetainStart start) {
        if (start.lanes() == null) {
            return start.pending() == null
                    ? CompletableFuture.completedFuture(Optional.empty())
                    : awaitTransition(start.pending()).thenCompose(ignored -> retainCommit(seal));
        }
        LinkedHashMap<String, List<ZLinkSerialExecutionQueue.QueuedRecord>> held =
                new LinkedHashMap<>();
        List<ZLinkRetainedSerialQueueCommit.Commit> retained = new ArrayList<>();
        try {
            for (Map.Entry<String, ZLinkSerialExecutionQueue> lane : start.lanes().entrySet()) {
                ZLinkRetainedSerialQueueCommit.Commit committed =
                        ZLinkRetainedSerialQueueCommit.retain(
                                        lane.getValue(), seal.seals.get(lane.getKey()))
                                .orElseThrow(
                                        () ->
                                                new IllegalStateException(
                                                        "composite relocation commit lost a lane"
                                                                + " fence"));
                retained.add(committed);
                held.put(lane.getKey(), committed.records());
            }
        } catch (RuntimeException failure) {
            return stateLane
                    .runAsync(() -> committing = false)
                    .thenCompose(ignored -> CompletableFuture.failedFuture(failure));
        }
        return stateLane
                .runAsync(
                        () -> {
                            if (active != seal) {
                                committing = false;
                                return false;
                            }
                            active = null;
                            committing = false;
                            return true;
                        })
                .thenApply(
                        retainedActive ->
                                retainedActive
                                        ? Optional.of(new RelocationCommit(held, retained))
                                        : Optional.empty());
    }

    public CompletionStage<Optional<Map<String, List<ZLinkSerialExecutionQueue.QueuedRecord>>>>
            freezeIngress(Seal seal) {
        return stateLane
                .runAsync(
                        () -> {
                            if (transition != null) {
                                return new FreezeStart(null, transition);
                            }
                            if (committing || seal == null || seal != active) {
                                return new FreezeStart(null, null);
                            }
                            CompletableFuture<Void> pending = new CompletableFuture<>();
                            transition = pending;
                            return new FreezeStart(seal, pending);
                        })
                .thenComposeAsync(start -> freezeIngressStarted(seal, start));
    }

    private CompletionStage<Optional<Map<String, List<ZLinkSerialExecutionQueue.QueuedRecord>>>>
            freezeIngressStarted(Seal seal, FreezeStart start) {
        if (start.seal() == null) {
            return start.pending() == null
                    ? CompletableFuture.completedFuture(Optional.empty())
                    : awaitTransition(start.pending()).thenCompose(ignored -> freezeIngress(seal));
        }
        try {
            LinkedHashMap<String, List<ZLinkSerialExecutionQueue.QueuedRecord>> held =
                    new LinkedHashMap<>();
            for (Map.Entry<String, ZLinkSerialExecutionQueue> lane :
                    start.seal().lanes.entrySet()) {
                held.put(
                        lane.getKey(),
                        lane.getValue()
                                .freezeRelocationIngress(start.seal().seals.get(lane.getKey()))
                                .orElseThrow(
                                        () ->
                                                new IllegalStateException(
                                                        "composite relocation freeze lost a"
                                                                + " lane fence")));
            }
            return finishTransition(start.pending())
                    .thenApply(ignored -> Optional.of(Collections.unmodifiableMap(held)));
        } catch (RuntimeException | Error failure) {
            return finishTransition(start.pending())
                    .thenCompose(ignored -> CompletableFuture.failedFuture(failure));
        }
    }

    public <T> CompletionStage<T> runCapture(Seal seal, Supplier<CompletionStage<T>> capture) {
        if (seal == null) {
            throw new IllegalStateException(
                    "capture requires the active relocation barrier generation");
        }
        return stateLane
                .runAsync(
                        () -> {
                            if (transition != null) {
                                return new CaptureStart(false, transition);
                            }
                            if (committing || seal != active) {
                                throw new IllegalStateException(
                                        "capture requires the active relocation barrier"
                                                + " generation");
                            }
                            return new CaptureStart(true, null);
                        })
                .thenComposeAsync(
                        start ->
                                start.accepted()
                                        ? Objects.requireNonNull(capture.get(), "capture result")
                                        : awaitTransition(start.pending())
                                                .thenCompose(ignored -> runCapture(seal, capture)));
    }

    /** Returns the immutable accepted journal fixed by this active seal. */
    public CompletionStage<Optional<Map<String, List<ZLinkSerialExecutionQueue.QueuedRecord>>>>
            captured(Seal seal) {
        return stateLane
                .runAsync(
                        () -> {
                            if (transition != null) {
                                return new CapturedState(null, transition);
                            }
                            if (seal == null || seal != active) {
                                return new CapturedState(null, null);
                            }
                            return new CapturedState(
                                    seal.lanes.keySet().stream()
                                            .map(
                                                    laneId ->
                                                            Map.entry(
                                                                    laneId, seal.seals.get(laneId)))
                                            .toList(),
                                    null);
                        })
                .thenComposeAsync(state -> capturedAfterState(seal, state));
    }

    private CompletionStage<Optional<Map<String, List<ZLinkSerialExecutionQueue.QueuedRecord>>>>
            capturedAfterState(Seal seal, CapturedState state) {
        if (state.seals() == null) {
            return state.pending() == null
                    ? CompletableFuture.completedFuture(Optional.empty())
                    : awaitTransition(state.pending()).thenCompose(ignored -> captured(seal));
        }
        LinkedHashMap<String, List<ZLinkSerialExecutionQueue.QueuedRecord>> captured =
                new LinkedHashMap<>();
        for (Map.Entry<String, ZLinkSerialExecutionQueue.RelocationSeal> entry : state.seals()) {
            captured.put(entry.getKey(), entry.getValue().captured());
        }
        return CompletableFuture.completedFuture(
                Optional.of(Collections.unmodifiableMap(captured)));
    }

    private static void rollback(
            Map<String, ZLinkSerialExecutionQueue> lanes,
            Map<String, ZLinkSerialExecutionQueue.RelocationSeal> seals) {
        List<String> laneIds = new ArrayList<>(seals.keySet());
        Collections.reverse(laneIds);
        for (String laneId : laneIds) {
            if (!lanes.get(laneId).abortRelocation(seals.get(laneId))) {
                throw new IllegalStateException(
                        "partial relocation seal rollback lost a lane fence");
            }
        }
    }

    private static LinkedHashMap<String, ZLinkSerialExecutionQueue> validateLanes(
            Map<String, ZLinkSerialExecutionQueue> lanes) {
        if (lanes == null || lanes.isEmpty()) {
            throw new IllegalArgumentException("at least one relocation lane is required");
        }
        LinkedHashMap<String, ZLinkSerialExecutionQueue> snapshot = new LinkedHashMap<>();
        lanes.forEach(
                (laneId, queue) -> {
                    String required = requireLaneId(laneId);
                    if (snapshot.putIfAbsent(
                                    required, Objects.requireNonNull(queue, "relocation lane"))
                            != null) {
                        throw new IllegalArgumentException(
                                "duplicate relocation lane: " + required);
                    }
                });
        return snapshot;
    }

    private static void release(
            Map<String, ZLinkSerialExecutionQueue.RelocationBoundary> boundaries) {
        boundaries.values().forEach(ZLinkSerialExecutionQueue.RelocationBoundary::release);
    }

    private static CompletionStage<Void> awaitFinished(
            Map<String, ZLinkSerialExecutionQueue.RelocationBoundary> boundaries) {
        CompletableFuture<?>[] finished =
                boundaries.values().stream()
                        .map(boundary -> boundary.finished().toCompletableFuture())
                        .toArray(CompletableFuture[]::new);
        return CompletableFuture.allOf(finished);
    }

    private static String requireLaneId(String laneId) {
        if (laneId == null || laneId.isBlank()) {
            throw new IllegalArgumentException("relocation lane id is required");
        }
        return laneId;
    }

    private record SealStart(
            LinkedHashMap<String, ZLinkSerialExecutionQueue> lanes,
            CompletableFuture<Void> pending) {}

    private record AbortStart(Seal seal, CompletableFuture<Void> pending) {}

    private record FreezeStart(Seal seal, CompletableFuture<Void> pending) {}

    private record RetainStart(
            LinkedHashMap<String, ZLinkSerialExecutionQueue> lanes,
            CompletableFuture<Void> pending) {}

    private record CapturedState(
            List<Map.Entry<String, ZLinkSerialExecutionQueue.RelocationSeal>> seals,
            CompletableFuture<Void> pending) {}

    private record CaptureStart(boolean accepted, CompletableFuture<Void> pending) {}

    public static final class Seal {
        private final long generation;
        private final Map<String, ZLinkSerialExecutionQueue> lanes;
        private final Map<String, ZLinkSerialExecutionQueue.RelocationSeal> seals;

        private Seal(
                long generation,
                Map<String, ZLinkSerialExecutionQueue> lanes,
                Map<String, ZLinkSerialExecutionQueue.RelocationSeal> seals) {
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
        private final Map<String, List<ZLinkSerialExecutionQueue.QueuedRecord>> records;
        private final List<ZLinkRetainedSerialQueueCommit.Commit> lanes;
        private final List<String> laneIds;
        private final AtomicBoolean completed = new AtomicBoolean();

        private RelocationCommit(
                Map<String, List<ZLinkSerialExecutionQueue.QueuedRecord>> records,
                List<ZLinkRetainedSerialQueueCommit.Commit> lanes) {
            this.records = Collections.unmodifiableMap(new LinkedHashMap<>(records));
            this.lanes = List.copyOf(lanes);
            this.laneIds = List.copyOf(records.keySet());
        }

        public Map<String, List<ZLinkSerialExecutionQueue.QueuedRecord>> records() {
            return records;
        }

        /** Captures one sequence-stable suffix cut across every lane. */
        public Cut cut() {
            LinkedHashMap<String, List<ZLinkSerialExecutionQueue.QueuedRecord>> current =
                    new LinkedHashMap<>();
            List<ZLinkRetainedSerialQueueCommit.Cut> cuts = new ArrayList<>();
            for (int index = 0; index < lanes.size(); index++) {
                ZLinkRetainedSerialQueueCommit.Cut cut = lanes.get(index).cut();
                cuts.add(cut);
                current.put(laneIds.get(index), cut.records());
            }
            return new Cut(Collections.unmodifiableMap(current), List.copyOf(cuts));
        }

        /** Detaches all lanes only if no lane accepted ingress after this cut. */
        public boolean tryFinishCapture(Cut cut) {
            Objects.requireNonNull(cut, "cut");
            return ZLinkRetainedSerialQueueCommit.finishCapture(lanes, cut.lanes);
        }

        public boolean tryEstablishDurableCut(Cut cut) {
            Objects.requireNonNull(cut, "cut");
            return ZLinkRetainedSerialQueueCommit.establishDurableCut(lanes, cut.lanes);
        }

        public boolean tryEstablishAndFinishCapture(Cut cut) {
            Objects.requireNonNull(cut, "cut");
            return ZLinkRetainedSerialQueueCommit.establishAndFinishCapture(lanes, cut.lanes);
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
            private final Map<String, List<ZLinkSerialExecutionQueue.QueuedRecord>> records;
            private final List<ZLinkRetainedSerialQueueCommit.Cut> lanes;

            private Cut(
                    Map<String, List<ZLinkSerialExecutionQueue.QueuedRecord>> records,
                    List<ZLinkRetainedSerialQueueCommit.Cut> lanes) {
                this.records = records;
                this.lanes = lanes;
            }

            public Map<String, List<ZLinkSerialExecutionQueue.QueuedRecord>> records() {
                return records;
            }
        }
    }
}
