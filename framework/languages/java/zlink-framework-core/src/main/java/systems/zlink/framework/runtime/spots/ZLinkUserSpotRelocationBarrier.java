package systems.zlink.framework.runtime.spots;

import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.relocation.ZLinkCompositeRelocationBarrier;

import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.function.BooleanSupplier;
import java.util.function.Predicate;
import java.util.function.Supplier;

/** Owns the source-side all-lane barrier for one User Spot aggregate. */
final class ZLinkUserSpotRelocationBarrier {
    private final DefaultSpotContext context;
    private final ZLinkActorSessionCoordinator actors;
    private final ZLinkCompositeRelocationBarrier barrier = new ZLinkCompositeRelocationBarrier();
    private final ZLinkStateLane stateLane = new ZLinkStateLane();
    private Seal active;
    private boolean sealing;
    private boolean committing;

    ZLinkUserSpotRelocationBarrier(
            DefaultSpotContext context, ZLinkActorSessionCoordinator actors) {
        this.context = Objects.requireNonNull(context, "context");
        this.actors = Objects.requireNonNull(actors, "actors");
    }

    private <T> T inStateLane(Supplier<T> work) {
        try {
            var turn = stateLane.runNowOrQueue(work).toCompletableFuture();
            // An idle lane ran the turn on this thread; only a pending turn is a wait.
            assert turn.isDone() || ZLinkStateLane.assertMayBlock();
            return turn.join();
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

    CompletionStage<Optional<Seal>> trySeal() {
        return trySeal(ignored -> true);
    }

    CompletionStage<Optional<Seal>> trySeal(Predicate<Preview> admission) {
        Objects.requireNonNull(admission, "admission");
        boolean begin =
                inStateLane(
                        () -> {
                            if (active != null || sealing || committing) {
                                return false;
                            }
                            sealing = true;
                            return true;
                        });
        if (!begin) {
            return CompletableFuture.completedFuture(Optional.empty());
        }
        boolean timerFrozen = false;
        byte[] timerEnvelope;
        List<String> participantActorIds;
        LinkedHashMap<String, ZLinkSerialExecutionQueue> lanes;
        try {
            timerEnvelope = context.freezeTimerRelocationEnvelope();
            timerFrozen = true;
            participantActorIds = actors.actorIdsInSpot(context.spotId());
            lanes = relocationLanes(participantActorIds);
        } catch (RuntimeException failure) {
            clearSealing();
            if (timerFrozen) {
                context.resumeTimersAfterRelocationAbort();
            }
            throw failure;
        }
        return barrier.trySeal(lanes)
                .exceptionallyCompose(
                        failure -> {
                            clearSealing();
                            context.resumeTimersAfterRelocationAbort();
                            return CompletableFuture.failedFuture(failure);
                        })
                .thenCompose(
                        localSeal -> {
                            if (localSeal.isEmpty()) {
                                clearSealing();
                                context.resumeTimersAfterRelocationAbort();
                                return CompletableFuture.completedFuture(Optional.empty());
                            }
                            ZLinkCompositeRelocationBarrier.Seal composite =
                                    localSeal.orElseThrow();
                            if (!participantActorIds.equals(
                                    actors.actorIdsInSpot(context.spotId()))) {
                                return rejectSeal(composite, true, null);
                            }
                            return captureRecords(composite)
                                    .thenApply(
                                            captured -> {
                                                boolean admitted =
                                                        admission.test(
                                                                new Preview(
                                                                        timerEnvelope,
                                                                        participantActorIds,
                                                                        captured));
                                                return inStateLane(
                                                        () -> {
                                                            sealing = false;
                                                            if (active != null || !admitted) {
                                                                return null;
                                                            }
                                                            Seal result =
                                                                    new Seal(
                                                                            composite,
                                                                            timerEnvelope.clone(),
                                                                            participantActorIds,
                                                                            captured);
                                                            active = result;
                                                            return result;
                                                        });
                                            })
                                    .handle(
                                            (result, failure) ->
                                                    failure == null && result != null
                                                            ? CompletableFuture.completedFuture(
                                                                    Optional.of(result))
                                                            : rejectSeal(composite, true, failure))
                                    .thenCompose(result -> result);
                        });
    }

    CompletionStage<Optional<Seal>> sealAtTurnBoundary(
            Predicate<Preview> admission, BooleanSupplier cancelled) {
        Objects.requireNonNull(admission, "admission");
        Objects.requireNonNull(cancelled, "cancelled");
        boolean available =
                inStateLane(
                        () -> {
                            if (active != null || sealing || committing) {
                                return false;
                            }
                            return true;
                        });
        if (!available) {
            return CompletableFuture.completedFuture(Optional.empty());
        }
        List<String> participantActorIds = actors.actorIdsInSpot(context.spotId());
        LinkedHashMap<String, ZLinkSerialExecutionQueue> lanes =
                relocationLanes(participantActorIds);
        return barrier.sealAtTurnBoundary(lanes, cancelled)
                .thenCompose(
                        sealedResult ->
                                finishTurnBoundarySeal(
                                        sealedResult, participantActorIds, admission, cancelled));
    }

    CompletionStage<Optional<Seal>> sealForRelocation(
            Predicate<Preview> admission, BooleanSupplier cancelled) {
        if (context.relocationCoordinationMode()
                != systems.zlink.framework.configuration.ZLinkSpotRelocationCoordinationMode
                        .APPLICATION_SIGNALED) {
            return sealAtTurnBoundary(admission, cancelled);
        }
        return context.awaitRelocationReadySignal(() -> trySeal(admission), cancelled)
                .thenApply(
                        sealedResult -> {
                            sealedResult.ifPresent(
                                    value ->
                                            inStateLane(
                                                    () -> {
                                                        value.markApplicationSignaled();
                                                        return null;
                                                    }));
                            return sealedResult;
                        });
    }

    private CompletionStage<Optional<Seal>> finishTurnBoundarySeal(
            Optional<ZLinkCompositeRelocationBarrier.Seal> sealedResult,
            List<String> participantActorIds,
            Predicate<Preview> admission,
            BooleanSupplier cancelled) {
        if (sealedResult.isEmpty()) {
            return CompletableFuture.completedFuture(Optional.empty());
        }
        ZLinkCompositeRelocationBarrier.Seal composite = sealedResult.orElseThrow();
        boolean begin;
        begin =
                inStateLane(
                        () -> {
                            boolean allowed = active == null && !sealing && !committing;
                            if (allowed) {
                                sealing = true;
                            }
                            return allowed;
                        });
        if (!begin) {
            return rollback(composite).thenApply(ignored -> Optional.empty());
        }
        if (cancelled.getAsBoolean()
                || !participantActorIds.equals(actors.actorIdsInSpot(context.spotId()))) {
            return rejectSeal(composite, false, null);
        }
        byte[] timerEnvelope;
        try {
            timerEnvelope = context.freezeTimerRelocationEnvelope();
        } catch (RuntimeException failure) {
            return rejectSeal(composite, false, failure);
        }
        return captureRecords(composite)
                .thenApply(
                        captured -> {
                            boolean admitted =
                                    admission.test(
                                            new Preview(
                                                    timerEnvelope, participantActorIds, captured));
                            return inStateLane(
                                    () -> {
                                        sealing = false;
                                        if (active != null || !admitted) {
                                            return null;
                                        }
                                        Seal result =
                                                new Seal(
                                                        composite,
                                                        timerEnvelope,
                                                        participantActorIds,
                                                        captured);
                                        active = result;
                                        return result;
                                    });
                        })
                .handle(
                        (result, failure) ->
                                failure == null && result != null
                                        ? CompletableFuture.completedFuture(Optional.of(result))
                                        : rejectSeal(composite, true, failure))
                .thenCompose(result -> result);
    }

    <T> CompletionStage<T> runCapture(Seal seal, Supplier<CompletionStage<T>> capture) {
        inStateLane(
                () -> {
                    requireActive(seal);
                    return null;
                });
        return barrier.runCapture(seal.composite, capture);
    }

    CompletionStage<Boolean> abortAsync(Seal seal) {
        return abortAsync(seal, null);
    }

    /**
     * Aborts the sealedResult relocation. The optional {@code beforeLaneResume} step runs after the
     * abort is guaranteed to proceed and before the captured queues are restored and the lanes
     * resume, so terminal bookkeeping can finish while every lane is still paused.
     */
    CompletionStage<Boolean> abortAsync(Seal seal, Runnable beforeLaneResume) {
        Boolean completionRequired =
                inStateLane(
                        () -> {
                            if (committing
                                    || seal == null
                                    || seal != active
                                    || !seal.markAbortInProgress()) {
                                return null;
                            }
                            return seal.applicationSignaled() && seal.markCompletionScheduled();
                        });
        if (completionRequired == null) {
            return CompletableFuture.completedFuture(false);
        }

        CompletionStage<Void> completion;
        if (!completionRequired) {
            completion = CompletableFuture.completedFuture(null);
        } else {
            try {
                completion =
                        Objects.requireNonNull(
                                context.runRelocationReadyCompletion(
                                        systems.zlink.framework.spots
                                                .ZLinkSpotRelocationReadyOutcome.CONTINUED),
                                "relocation completion result");
            } catch (RuntimeException failure) {
                completion = CompletableFuture.failedFuture(failure);
            }
        }
        return completion
                .handle((ignored, completionFailure) -> completionFailure)
                .thenCompose(
                        completionFailure -> {
                            boolean activeSeal =
                                    inStateLane(
                                            () -> {
                                                if (seal != active) {
                                                    return false;
                                                }
                                                return true;
                                            });
                            if (!activeSeal) {
                                if (completionFailure != null) {
                                    return CompletableFuture.failedFuture(completionFailure);
                                }
                                return CompletableFuture.completedFuture(false);
                            }
                            if (beforeLaneResume != null) {
                                beforeLaneResume.run();
                            }
                            return barrier.abort(seal.composite)
                                    .thenApply(
                                            restored -> {
                                                if (!restored) {
                                                    throw new IllegalStateException(
                                                            "User Spot barrier abort lost local lane");
                                                }
                                                inStateLane(
                                                        () -> {
                                                            if (seal == active) {
                                                                active = null;
                                                            }
                                                            return null;
                                                        });
                                                context.resumeTimersAfterRelocationAbort();
                                                if (completionFailure != null) {
                                                    throw new CompletionException(
                                                            completionFailure);
                                                }
                                                return true;
                                            });
                        });
    }

    CompletionStage<Boolean> abort(Seal seal) {
        return abortAsync(seal);
    }

    CompletionStage<Optional<Committed>> commit(Seal seal) {
        return retainCommit(seal)
                .thenApply(
                        retained -> {
                            retained.ifPresent(RelocationCommit::complete);
                            return retained.map(RelocationCommit::committed);
                        });
    }

    CompletionStage<Optional<RelocationCommit>> retainCommit(Seal seal) {
        boolean begin =
                inStateLane(
                        () -> {
                            if (committing || seal == null || seal != active || seal.aborting()) {
                                return false;
                            }
                            committing = true;
                            return true;
                        });
        if (!begin) {
            return CompletableFuture.completedFuture(Optional.empty());
        }
        return barrier.retainCommit(seal.composite)
                .thenApply(
                        retained -> {
                            ZLinkCompositeRelocationBarrier.RelocationCommit committed =
                                    retained.orElseThrow(
                                            () ->
                                                    new IllegalStateException(
                                                            "User Spot barrier commit lost a"
                                                                    + " lane"));
                            boolean retainedActive =
                                    inStateLane(
                                            () -> {
                                                if (active != seal) {
                                                    committing = false;
                                                    return false;
                                                }
                                                active = null;
                                                committing = false;
                                                return true;
                                            });
                            if (!retainedActive) {
                                return Optional.<RelocationCommit>empty();
                            }
                            LinkedHashMap<String, List<ZLinkSerialExecutionQueue.QueuedRecord>>
                                    heldIngress = new LinkedHashMap<>(committed.records());
                            return Optional.of(
                                    new RelocationCommit(
                                            new Committed(
                                                    seal.generation(),
                                                    seal.timerEnvelope(),
                                                    seal.participantActorIds(),
                                                    heldIngress),
                                            committed));
                        })
                .whenComplete(
                        (retained, failure) -> {
                            if (failure != null) {
                                inStateLane(
                                        () -> {
                                            committing = false;
                                            return null;
                                        });
                            }
                        });
    }

    CompletionStage<Optional<Map<String, List<ZLinkSerialExecutionQueue.QueuedRecord>>>>
            freezeIngress(Seal seal) {
        ZLinkCompositeRelocationBarrier.Seal composite =
                inStateLane(
                        () -> {
                            if (committing || seal == null || seal != active) {
                                return null;
                            }
                            if (seal.aborting()) {
                                return null;
                            }
                            return seal.composite;
                        });
        if (composite == null) {
            return CompletableFuture.completedFuture(Optional.empty());
        }
        return barrier.freezeIngress(composite)
                .thenApply(
                        frozen -> {
                            LinkedHashMap<String, List<ZLinkSerialExecutionQueue.QueuedRecord>>
                                    held =
                                            new LinkedHashMap<>(
                                                    frozen.orElseThrow(
                                                            () ->
                                                                    new IllegalStateException(
                                                                            "User Spot barrier freeze"
                                                                                    + " lost a lane")));
                            return Optional.of(Collections.unmodifiableMap(held));
                        });
    }

    private CompletionStage<Void> rollback(ZLinkCompositeRelocationBarrier.Seal seal) {
        return barrier.abort(seal)
                .thenApply(
                        restored -> {
                            if (!restored) {
                                throw new IllegalStateException(
                                        "partial User Spot barrier rollback lost a lane");
                            }
                            return null;
                        });
    }

    private CompletionStage<Optional<Seal>> rejectSeal(
            ZLinkCompositeRelocationBarrier.Seal seal, boolean timerFrozen, Throwable failure) {
        return rollback(seal)
                .whenComplete(
                        (ignored, rollbackFailure) -> {
                            clearSealing();
                            if (timerFrozen) {
                                context.resumeTimersAfterRelocationAbort();
                            }
                        })
                .thenCompose(
                        ignored ->
                                failure == null
                                        ? CompletableFuture.completedFuture(Optional.empty())
                                        : CompletableFuture.<Optional<Seal>>failedFuture(failure));
    }

    private void clearSealing() {
        inStateLane(
                () -> {
                    sealing = false;
                    return null;
                });
    }

    private void requireActive(Seal seal) {
        if (committing || seal == null || seal != active || seal.aborting()) {
            throw new IllegalStateException(
                    "capture requires the active User Spot barrier generation");
        }
    }

    private CompletionStage<Map<String, List<ZLinkSerialExecutionQueue.QueuedRecord>>>
            captureRecords(ZLinkCompositeRelocationBarrier.Seal seal) {
        return barrier.captured(seal)
                .thenApply(
                        captured ->
                                new LinkedHashMap<>(
                                        captured.orElseThrow(
                                                () ->
                                                        new IllegalStateException(
                                                                "User Spot relocation seal is not"
                                                                        + " active"))));
    }

    private LinkedHashMap<String, ZLinkSerialExecutionQueue> relocationLanes(
            List<String> participantActorIds) {
        LinkedHashMap<String, ZLinkSerialExecutionQueue> lanes =
                new LinkedHashMap<>(context.relocationLanes());
        for (String actorId : participantActorIds) {
            if (lanes.putIfAbsent("actor:" + actorId, context.actorRelocationLane(actorId))
                    != null) {
                throw new IllegalStateException(
                        "duplicate User Spot relocation lane: actor:" + actorId);
            }
        }
        return lanes;
    }

    static final class Seal {
        private final ZLinkCompositeRelocationBarrier.Seal composite;
        private final byte[] timerEnvelope;
        private final List<String> participantActorIds;
        private final Map<String, List<ZLinkSerialExecutionQueue.QueuedRecord>> capturedRecords;
        private boolean applicationSignaled;
        private boolean completionScheduled;
        private boolean aborting;

        private Seal(
                ZLinkCompositeRelocationBarrier.Seal composite,
                byte[] timerEnvelope,
                List<String> participantActorIds,
                Map<String, List<ZLinkSerialExecutionQueue.QueuedRecord>> capturedRecords) {
            this.composite = composite;
            this.timerEnvelope = timerEnvelope;
            this.participantActorIds = List.copyOf(participantActorIds);
            this.capturedRecords = capturedRecords;
        }

        long generation() {
            return composite.generation();
        }

        byte[] timerEnvelope() {
            return timerEnvelope.clone();
        }

        List<String> participantActorIds() {
            return participantActorIds;
        }

        Map<String, List<ZLinkSerialExecutionQueue.QueuedRecord>> capturedRecords() {
            return capturedRecords;
        }

        void markApplicationSignaled() {
            applicationSignaled = true;
        }

        boolean applicationSignaled() {
            return applicationSignaled;
        }

        boolean markCompletionScheduled() {
            if (completionScheduled) {
                return false;
            }
            completionScheduled = true;
            return true;
        }

        boolean markAbortInProgress() {
            if (aborting) {
                return false;
            }
            aborting = true;
            return true;
        }

        boolean aborting() {
            return aborting;
        }
    }

    record Preview(
            byte[] timerEnvelope,
            List<String> participantActorIds,
            Map<String, List<ZLinkSerialExecutionQueue.QueuedRecord>> capturedRecords) {
        Preview {
            timerEnvelope = timerEnvelope.clone();
            participantActorIds = List.copyOf(participantActorIds);
            capturedRecords = Map.copyOf(capturedRecords);
        }

        @Override
        public byte[] timerEnvelope() {
            return timerEnvelope.clone();
        }
    }

    record Committed(
            long generation,
            byte[] timerEnvelope,
            List<String> participantActorIds,
            Map<String, List<ZLinkSerialExecutionQueue.QueuedRecord>> heldIngress) {
        Committed {
            timerEnvelope = timerEnvelope.clone();
            participantActorIds = List.copyOf(participantActorIds);
            heldIngress = Map.copyOf(heldIngress);
        }

        @Override
        public byte[] timerEnvelope() {
            return timerEnvelope.clone();
        }
    }

    static final class RelocationCommit {
        private final Committed committed;
        private final ZLinkCompositeRelocationBarrier.RelocationCommit retained;

        private RelocationCommit(
                Committed committed, ZLinkCompositeRelocationBarrier.RelocationCommit retained) {
            this.committed = committed;
            this.retained = retained;
        }

        Committed committed() {
            return committed;
        }

        Cut cut() {
            ZLinkCompositeRelocationBarrier.RelocationCommit.Cut cut = retained.cut();
            return new Cut(
                    new Committed(
                            committed.generation(),
                            committed.timerEnvelope(),
                            committed.participantActorIds(),
                            cut.records()),
                    cut);
        }

        boolean tryFinishCapture(Cut cut) {
            Objects.requireNonNull(cut, "cut");
            return retained.tryFinishCapture(cut.retained);
        }

        boolean tryEstablishDurableCut(Cut cut) {
            Objects.requireNonNull(cut, "cut");
            return retained.tryEstablishDurableCut(cut.retained);
        }

        boolean tryEstablishAndFinishCapture(Cut cut) {
            Objects.requireNonNull(cut, "cut");
            return retained.tryEstablishAndFinishCapture(cut.retained);
        }

        boolean abort() {
            return retained.abort();
        }

        void complete() {
            retained.complete();
        }

        static final class Cut {
            private final Committed committed;
            private final ZLinkCompositeRelocationBarrier.RelocationCommit.Cut retained;

            private Cut(
                    Committed committed,
                    ZLinkCompositeRelocationBarrier.RelocationCommit.Cut retained) {
                this.committed = committed;
                this.retained = retained;
            }

            Committed committed() {
                return committed;
            }
        }
    }
}
