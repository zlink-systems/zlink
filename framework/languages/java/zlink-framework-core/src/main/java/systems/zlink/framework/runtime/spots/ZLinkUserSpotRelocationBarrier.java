package systems.zlink.framework.runtime.spots;
import java.util.Collections;
import java.util.Objects;

import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.function.BooleanSupplier;
import java.util.function.Predicate;
import java.util.function.Supplier;
import java.util.logging.Level;
import java.util.logging.Logger;
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.relocation
    .ZLinkCompositeRelocationBarrier;

/**
 * Owns the source-side all-lane barrier for one User Spot aggregate.
 */
final class ZLinkUserSpotRelocationBarrier {
    private static final Logger LOGGER =
        Logger.getLogger(ZLinkUserSpotRelocationBarrier.class.getName());
    private final DefaultSpotContext context;
    private final ZLinkActorSessionCoordinator actors;
    private final ZLinkCompositeRelocationBarrier barrier =
        new ZLinkCompositeRelocationBarrier();
    private final ZLinkStateLane stateLane = new ZLinkStateLane();
    private Seal active;
    private boolean sealing;
    private boolean committing;

    ZLinkUserSpotRelocationBarrier(
        DefaultSpotContext context,
        ZLinkActorSessionCoordinator actors) {
        this.context = Objects.requireNonNull(
            context, "context");
        this.actors = Objects.requireNonNull(
            actors, "actors");
    }

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

    Optional<Seal> trySeal() {
        return trySeal(ignored -> true);
    }

    Optional<Seal> trySeal(Predicate<Preview> admission) {
        Objects.requireNonNull(admission, "admission");
        byte[] timerEnvelope;
        List<String> participantActorIds;
        Optional<ZLinkCompositeRelocationBarrier.Seal> localSeal =
            Optional.empty();
        Map<String, List<ZLinkAsyncSerialQueue.QueuedRecord>> captured;
        boolean begin = inStateLane(() -> {
            if (active != null || sealing || committing) {
                return false;
            }
            sealing = true;
            return true;
        });
        if (!begin) {
            return Optional.empty();
        }
        boolean timerFrozen = false;
        try {
            timerEnvelope = context.freezeTimerRelocationEnvelope();
            timerFrozen = true;
            participantActorIds = actors.actorIdsInSpot(context.spotId());
            LinkedHashMap<String, ZLinkAsyncSerialQueue> lanes =
                relocationLanes(participantActorIds);
            localSeal = barrier.trySeal(lanes);
            if (localSeal.isEmpty()) {
                clearSealing();
                context.resumeTimersAfterRelocationAbort();
                return Optional.empty();
            }
            List<String> currentActorIds =
                actors.actorIdsInSpot(context.spotId());
            if (!participantActorIds.equals(currentActorIds)) {
                rollback(localSeal.get());
                clearSealing();
                context.resumeTimersAfterRelocationAbort();
                return Optional.empty();
            }
            captured = captureRecords(localSeal.get());
        } catch (RuntimeException failure) {
            if (localSeal.isPresent()) {
                rollback(localSeal.get());
            }
            clearSealing();
            if (timerFrozen) {
                context.resumeTimersAfterRelocationAbort();
            }
            throw failure;
        }

        boolean admitted;
        try {
            admitted = admission.test(new Preview(
                timerEnvelope,
                participantActorIds,
                captured));
        } catch (RuntimeException failure) {
            rollback(localSeal.get());
            clearSealing();
            context.resumeTimersAfterRelocationAbort();
            throw failure;
        }

        ZLinkCompositeRelocationBarrier.Seal sealedComposite =
            localSeal.orElseThrow();
        Seal result = inStateLane(() -> {
            if (active == null && sealing && admitted) {
                Seal sealed = new Seal(
                    sealedComposite,
                    timerEnvelope.clone(),
                    participantActorIds,
                    captured);
                active = sealed;
                sealing = false;
                return sealed;
            } else {
                sealing = false;
                return null;
            }
        });
        if (result != null) {
            return Optional.of(result);
        }
        rollback(localSeal.get());
        context.resumeTimersAfterRelocationAbort();
        return Optional.empty();
    }

    CompletionStage<Optional<Seal>> sealAtTurnBoundary(
        Predicate<Preview> admission,
        BooleanSupplier cancelled) {
        Objects.requireNonNull(admission, "admission");
        Objects.requireNonNull(cancelled, "cancelled");
        boolean available = inStateLane(() -> {
            if (active != null || sealing || committing) {
                return false;
            }
            return true;
        });
        if (!available) {
            return CompletableFuture.completedFuture(Optional.empty());
        }
        List<String> participantActorIds =
            actors.actorIdsInSpot(context.spotId());
        LinkedHashMap<String, ZLinkAsyncSerialQueue> lanes =
            relocationLanes(participantActorIds);
        return barrier.sealAtTurnBoundary(lanes, cancelled)
            .thenApply(sealed -> finishTurnBoundarySeal(
                sealed,
                participantActorIds,
                admission,
                cancelled));
    }

    CompletionStage<Optional<Seal>> sealForRelocation(
        Predicate<Preview> admission,
        BooleanSupplier cancelled) {
        if (context.relocationCoordinationMode()
            != systems.zlink.framework.configuration
                .ZLinkSpotRelocationCoordinationMode.APPLICATION_SIGNALED) {
            return sealAtTurnBoundary(admission, cancelled);
        }
        return context.awaitRelocationReadySignal(
            () -> trySeal(admission),
            cancelled)
            .thenApply(sealed -> {
                sealed.ifPresent(value -> inStateLane(() -> {
                    value.markApplicationSignaled();
                    return null;
                }));
                return sealed;
            });
    }

    private Optional<Seal> finishTurnBoundarySeal(
        Optional<ZLinkCompositeRelocationBarrier.Seal> sealed,
        List<String> participantActorIds,
        Predicate<Preview> admission,
        BooleanSupplier cancelled) {
        if (sealed.isEmpty()) {
            return Optional.empty();
        }
        ZLinkCompositeRelocationBarrier.Seal composite =
            sealed.orElseThrow();
        boolean begin;
        begin = inStateLane(() -> {
            boolean allowed = active == null && !sealing && !committing;
            if (allowed) {
                sealing = true;
            }
            return allowed;
        });
        if (!begin) {
            rollback(composite);
            return Optional.empty();
        }
        boolean timerFrozen = false;
        try {
            if (cancelled.getAsBoolean()
                || !participantActorIds.equals(
                    actors.actorIdsInSpot(context.spotId()))) {
                rollback(composite);
                clearSealing();
                return Optional.empty();
            }
            byte[] timerEnvelope =
                context.freezeTimerRelocationEnvelope();
            timerFrozen = true;
            Map<String, List<ZLinkAsyncSerialQueue.QueuedRecord>> captured =
                captureRecords(composite);
            boolean admitted;
            admitted = admission.test(new Preview(
                timerEnvelope,
                participantActorIds,
                captured));

            Seal result = inStateLane(() -> {
                if (active == null && sealing && admitted) {
                    Seal value = new Seal(
                        composite,
                        timerEnvelope,
                        participantActorIds,
                        captured);
                    active = value;
                    sealing = false;
                    return value;
                } else {
                    sealing = false;
                    return null;
                }
            });
            if (result != null) {
                return Optional.of(result);
            }
            rollback(composite);
            if (timerFrozen) {
                context.resumeTimersAfterRelocationAbort();
            }
            return Optional.empty();
        } catch (RuntimeException failure) {
            rollback(composite);
            clearSealing();
            if (timerFrozen) {
                context.resumeTimersAfterRelocationAbort();
            }
            throw failure;
        }
    }

    <T> CompletionStage<T> runCapture(
        Seal seal,
        Supplier<CompletionStage<T>> capture) {
        inStateLane(() -> {
            requireActive(seal);
            return null;
        });
        return barrier.runCapture(seal.composite, capture);
    }

    CompletionStage<Boolean> abortAsync(Seal seal) {
        return abortAsync(seal, null);
    }

    /**
     * Aborts the sealed relocation. The optional {@code beforeLaneResume}
     * step runs after the abort is guaranteed to proceed and before the
     * captured queues are restored and the lanes resume, so terminal
     * bookkeeping can finish while every lane is still paused.
     */
    CompletionStage<Boolean> abortAsync(Seal seal, Runnable beforeLaneResume) {
        Boolean completionRequired = inStateLane(() -> {
            if (committing || seal == null || seal != active
                || !seal.markAbortInProgress()) {
                return null;
            }
            return seal.applicationSignaled()
                && seal.markCompletionScheduled();
        });
        if (completionRequired == null) {
            return CompletableFuture.completedFuture(false);
        }

        CompletionStage<Void> completion;
        if (!completionRequired) {
            completion = CompletableFuture.completedFuture(null);
        } else {
            try {
                completion = Objects.requireNonNull(
                    context.runRelocationReadyCompletion(
                        systems.zlink.framework.spots
                            .ZLinkSpotRelocationReadyOutcome.CONTINUED),
                    "relocation completion result");
            } catch (RuntimeException failure) {
                completion = CompletableFuture.failedFuture(failure);
            }
        }
        return completion.handle((ignored, completionFailure) -> {
            boolean shouldResume;
            boolean activeSeal = inStateLane(() -> {
                if (seal != active) {
                    return false;
                }
                return true;
            });
            if (!activeSeal) {
                if (completionFailure != null) {
                    throw new CompletionException(completionFailure);
                }
                return false;
            }
            shouldResume = true;
            if (beforeLaneResume != null) {
                beforeLaneResume.run();
            }
            if (!barrier.abort(seal.composite)) {
                throw new IllegalStateException(
                    "User Spot barrier abort lost local lane");
            }
            inStateLane(() -> {
                if (seal == active) {
                    active = null;
                }
                return null;
            });
            if (shouldResume) {
                context.resumeTimersAfterRelocationAbort();
            }
            if (completionFailure != null) {
                throw new CompletionException(completionFailure);
            }
            return true;
        });
    }

    boolean abort(Seal seal) {
        CompletionStage<Boolean> completion = abortAsync(seal);
        CompletableFuture<Boolean> future = completion.toCompletableFuture();
        if (future.isDone()) {
            return future.getNow(false);
        }
        completion.whenComplete((ignored, failure) -> {
            if (failure != null) {
                LOGGER.log(
                    Level.WARNING,
                    "User Spot relocation abort completion failed",
                    failure);
            }
        });
        return true;
    }

    Optional<Committed> commit(Seal seal) {
        Optional<RelocationCommit> retained = retainCommit(seal);
        retained.ifPresent(RelocationCommit::complete);
        return retained.map(RelocationCommit::committed);
    }

    Optional<RelocationCommit> retainCommit(Seal seal) {
        boolean begin = inStateLane(() -> {
            if (committing || seal == null || seal != active
                || seal.aborting()) {
                return false;
            }
            committing = true;
            return true;
        });
        if (!begin) {
            return Optional.empty();
        }
        ZLinkCompositeRelocationBarrier.RelocationCommit committed =
            barrier.retainCommit(seal.composite)
                .orElseThrow(() -> new IllegalStateException(
                    "User Spot barrier commit lost a lane"));
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
        LinkedHashMap<String, List<ZLinkAsyncSerialQueue.QueuedRecord>>
            heldIngress = new LinkedHashMap<>(committed.records());
        return Optional.of(new RelocationCommit(
            new Committed(
                seal.generation(),
                seal.timerEnvelope(),
                seal.participantActorIds(),
                heldIngress),
            committed));
    }

    Optional<Map<String, List<
        ZLinkAsyncSerialQueue.QueuedRecord>>> freezeIngress(Seal seal) {
        ZLinkCompositeRelocationBarrier.Seal composite = inStateLane(() -> {
            if (committing || seal == null || seal != active) {
                return null;
            }
            if (seal.aborting()) {
                return null;
            }
            return seal.composite;
        });
        if (composite == null) {
            return Optional.empty();
        }
        LinkedHashMap<String, List<ZLinkAsyncSerialQueue.QueuedRecord>> held =
            new LinkedHashMap<>();
        held.putAll(barrier.freezeIngress(composite)
            .orElseThrow(() -> new IllegalStateException(
                "User Spot barrier freeze lost a lane")));
        return Optional.of(Collections.unmodifiableMap(held));
    }

    private void rollback(ZLinkCompositeRelocationBarrier.Seal seal) {
        if (!barrier.abort(seal)) {
            throw new IllegalStateException(
                "partial User Spot barrier rollback lost a lane");
        }
    }

    private void clearSealing() {
        inStateLane(() -> {
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

    private Map<String, List<ZLinkAsyncSerialQueue.QueuedRecord>>
        captureRecords(ZLinkCompositeRelocationBarrier.Seal seal) {
        return new LinkedHashMap<>(barrier.captured(seal)
                .orElseThrow(() -> new IllegalStateException(
                    "User Spot relocation seal is not active")));
    }

    private LinkedHashMap<String, ZLinkAsyncSerialQueue> relocationLanes(
        List<String> participantActorIds) {
        LinkedHashMap<String, ZLinkAsyncSerialQueue> lanes =
            new LinkedHashMap<>(context.relocationLanes());
        for (String actorId : participantActorIds) {
            if (lanes.putIfAbsent(
                    "actor:" + actorId,
                    actors.actorRelocationLane(actorId)) != null) {
                throw new IllegalStateException(
                    "duplicate User Spot relocation lane: actor:"
                        + actorId);
            }
        }
        return lanes;
    }

    static final class Seal {
        private final ZLinkCompositeRelocationBarrier.Seal composite;
        private final byte[] timerEnvelope;
        private final List<String> participantActorIds;
        private final Map<String, List<ZLinkAsyncSerialQueue.QueuedRecord>>
            capturedRecords;
        private boolean applicationSignaled;
        private boolean completionScheduled;
        private boolean aborting;

        private Seal(
            ZLinkCompositeRelocationBarrier.Seal composite,
            byte[] timerEnvelope,
            List<String> participantActorIds,
            Map<String, List<ZLinkAsyncSerialQueue.QueuedRecord>>
                capturedRecords) {
            this.composite = composite;
            this.timerEnvelope = timerEnvelope;
            this.participantActorIds = List.copyOf(
                participantActorIds);
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

        Map<String, List<ZLinkAsyncSerialQueue.QueuedRecord>>
            capturedRecords() {
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
        Map<String, List<ZLinkAsyncSerialQueue.QueuedRecord>> capturedRecords) {
        Preview {
            timerEnvelope = timerEnvelope.clone();
            participantActorIds = List.copyOf(participantActorIds);
            capturedRecords = Map.copyOf(capturedRecords);
        }

        @Override public byte[] timerEnvelope() {
            return timerEnvelope.clone();
        }
    }

    record Committed(
        long generation,
        byte[] timerEnvelope,
        List<String> participantActorIds,
        Map<String, List<ZLinkAsyncSerialQueue.QueuedRecord>> heldIngress) {
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
            Committed committed,
            ZLinkCompositeRelocationBarrier.RelocationCommit retained) {
            this.committed = committed;
            this.retained = retained;
        }

        Committed committed() {
            return committed;
        }

        Cut cut() {
            ZLinkCompositeRelocationBarrier.RelocationCommit.Cut cut =
                retained.cut();
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
            private final ZLinkCompositeRelocationBarrier.RelocationCommit.Cut
                retained;

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
