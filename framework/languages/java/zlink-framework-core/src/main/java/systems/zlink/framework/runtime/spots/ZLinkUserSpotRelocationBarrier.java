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
import java.util.concurrent.CompletionStage;
import java.util.function.BooleanSupplier;
import java.util.function.Predicate;
import java.util.function.Supplier;
import java.util.logging.Level;
import java.util.logging.Logger;

/** Owns the source-side all-lane barrier for one User Spot aggregate. */
final class ZLinkUserSpotRelocationBarrier {
    private static final Logger LOGGER =
            Logger.getLogger(ZLinkUserSpotRelocationBarrier.class.getName());
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

    private <T> CompletionStage<T> inStateLane(Supplier<T> work) {
        return stateLane.runNowOrQueue(work);
    }

    CompletionStage<Optional<Seal>> trySeal() {
        return trySeal(ignored -> true);
    }

    CompletionStage<Optional<Seal>> trySeal(Predicate<Preview> admission) {
        Objects.requireNonNull(admission, "admission");
        Map<String, ZLinkSerialExecutionQueue.ActiveTurnSealHandle> activeTurns =
                context.captureSpotActiveTurnSealHandle()
                        .<Map<String, ZLinkSerialExecutionQueue.ActiveTurnSealHandle>>map(
                                handle -> Map.of("spot", handle))
                        .orElseGet(Map::of);
        return inStateLane(
                        () -> {
                            if (active != null || sealing || committing) {
                                return false;
                            }
                            sealing = true;
                            return true;
                        })
                .thenCompose(
                        begin -> {
                            if (!begin) {
                                return CompletableFuture.completedFuture(Optional.empty());
                            }
                            byte[] timerEnvelope;
                            List<String> participantActorIds;
                            try {
                                timerEnvelope = context.freezeTimerRelocationEnvelope();
                                participantActorIds = actors.actorIdsInSpot(context.spotId());
                            } catch (RuntimeException failure) {
                                return clearSealing()
                                        .thenCompose(
                                                ignored -> {
                                                    context.resumeTimersAfterRelocationAbort();
                                                    return CompletableFuture.failedFuture(failure);
                                                });
                            }
                            return context.relocationLanesAsync(participantActorIds)
                                    .thenCompose(lanes -> barrier.trySeal(lanes, activeTurns))
                                    .exceptionallyCompose(
                                            failure -> {
                                                return clearSealing()
                                                        .thenCompose(
                                                                ignored -> {
                                                                    context
                                                                            .resumeTimersAfterRelocationAbort();
                                                                    return CompletableFuture
                                                                            .failedFuture(failure);
                                                                });
                                            })
                                    .thenCompose(
                                            localSeal -> {
                                                if (localSeal.isEmpty()) {
                                                    return clearSealing()
                                                            .thenApply(
                                                                    ignored -> {
                                                                        context
                                                                                .resumeTimersAfterRelocationAbort();
                                                                        return Optional
                                                                                .<Seal>empty();
                                                                    });
                                                }
                                                ZLinkCompositeRelocationBarrier.Seal composite =
                                                        localSeal.orElseThrow();
                                                if (!participantActorIds.equals(
                                                        actors.actorIdsInSpot(context.spotId()))) {
                                                    return rejectSeal(composite, true, null);
                                                }
                                                return captureRecords(composite)
                                                        .thenCompose(
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
                                                                                if (active != null
                                                                                        || !admitted) {
                                                                                    return null;
                                                                                }
                                                                                Seal result =
                                                                                        new Seal(
                                                                                                composite,
                                                                                                timerEnvelope
                                                                                                        .clone(),
                                                                                                participantActorIds,
                                                                                                captured);
                                                                                active = result;
                                                                                return result;
                                                                            });
                                                                })
                                                        .handle(
                                                                (result, failure) ->
                                                                        failure == null
                                                                                        && result
                                                                                                != null
                                                                                ? CompletableFuture
                                                                                        .completedFuture(
                                                                                                Optional
                                                                                                        .of(
                                                                                                                result))
                                                                                : rejectSeal(
                                                                                        composite,
                                                                                        true,
                                                                                        failure))
                                                        .thenCompose(result -> result);
                                            });
                        });
    }

    CompletionStage<Optional<Seal>> sealAtTurnBoundary(
            Predicate<Preview> admission, BooleanSupplier cancelled) {
        Objects.requireNonNull(admission, "admission");
        Objects.requireNonNull(cancelled, "cancelled");
        return inStateLane(
                        () -> {
                            if (active != null || sealing || committing) {
                                return false;
                            }
                            return true;
                        })
                .thenCompose(
                        available -> {
                            if (!available) {
                                return CompletableFuture.completedFuture(Optional.empty());
                            }
                            List<String> participantActorIds =
                                    actors.actorIdsInSpot(context.spotId());
                            return context.relocationLanesAsync(participantActorIds)
                                    .thenCompose(
                                            lanes -> barrier.sealAtTurnBoundary(lanes, cancelled))
                                    .thenCompose(
                                            sealedResult ->
                                                    finishTurnBoundarySeal(
                                                            sealedResult,
                                                            participantActorIds,
                                                            admission,
                                                            cancelled));
                        });
    }

    CompletionStage<Optional<Seal>> sealForRelocation(
            Predicate<Preview> admission,
            BooleanSupplier cancelled,
            CompletionStage<?> cancellationSignal) {
        if (context.relocationCoordinationMode()
                != systems.zlink.framework.configuration.ZLinkSpotRelocationCoordinationMode
                        .APPLICATION_SIGNALED) {
            return sealAtTurnBoundary(admission, cancelled);
        }
        return context.awaitRelocationReadySignal(() -> trySeal(admission), cancellationSignal)
                .thenCompose(
                        sealedResult ->
                                sealedResult.isEmpty()
                                        ? CompletableFuture.completedFuture(sealedResult)
                                        : inStateLane(
                                                () -> {
                                                    sealedResult
                                                            .orElseThrow()
                                                            .markApplicationSignaled();
                                                    return sealedResult;
                                                }));
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
        return inStateLane(
                        () -> {
                            boolean allowed = active == null && !sealing && !committing;
                            if (allowed) {
                                sealing = true;
                            }
                            return allowed;
                        })
                .thenCompose(
                        begin -> {
                            if (!begin) {
                                return rollback(composite).thenApply(ignored -> Optional.empty());
                            }
                            if (cancelled.getAsBoolean()
                                    || !participantActorIds.equals(
                                            actors.actorIdsInSpot(context.spotId()))) {
                                return rejectSeal(composite, false, null);
                            }
                            byte[] timerEnvelope;
                            try {
                                timerEnvelope = context.freezeTimerRelocationEnvelope();
                            } catch (RuntimeException failure) {
                                return rejectSeal(composite, false, failure);
                            }
                            return captureRecords(composite)
                                    .thenCompose(
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
                                                            ? CompletableFuture.completedFuture(
                                                                    Optional.of(result))
                                                            : rejectSeal(composite, true, failure))
                                    .thenCompose(result -> result);
                        });
    }

    <T> CompletionStage<T> runCapture(Seal seal, Supplier<CompletionStage<T>> capture) {
        return inStateLane(
                        () -> {
                            requireActive(seal);
                            return null;
                        })
                .thenCompose(ignored -> barrier.runCapture(seal.composite, capture));
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
        return inStateLane(
                        () -> {
                            if (committing
                                    || seal == null
                                    || seal != active
                                    || !seal.markAbortInProgress()) {
                                return null;
                            }
                            return seal.applicationSignaled() && seal.markCompletionScheduled();
                        })
                .thenCompose(
                        completionRequired -> {
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
                                                                    .ZLinkSpotRelocationReadyOutcome
                                                                    .CONTINUED),
                                                    "relocation completion result");
                                } catch (RuntimeException failure) {
                                    completion = CompletableFuture.failedFuture(failure);
                                }
                            }
                            return completion
                                    .handle((ignored, completionFailure) -> completionFailure)
                                    .thenCompose(
                                            completionFailure -> {
                                                return inStateLane(
                                                                () -> {
                                                                    if (seal != active) {
                                                                        return false;
                                                                    }
                                                                    return true;
                                                                })
                                                        .thenCompose(
                                                                activeSeal -> {
                                                                    if (!activeSeal) {
                                                                        if (completionFailure
                                                                                != null) {
                                                                            return CompletableFuture
                                                                                    .failedFuture(
                                                                                            completionFailure);
                                                                        }
                                                                        return CompletableFuture
                                                                                .completedFuture(
                                                                                        false);
                                                                    }
                                                                    if (beforeLaneResume != null) {
                                                                        beforeLaneResume.run();
                                                                    }
                                                                    return barrier.abort(
                                                                                    seal.composite)
                                                                            .thenCompose(
                                                                                    restored -> {
                                                                                        if (!restored) {
                                                                                            throw new IllegalStateException(
                                                                                                    "User Spot barrier abort lost local lane");
                                                                                        }
                                                                                        return inStateLane(
                                                                                                        () -> {
                                                                                                            if (seal
                                                                                                                    == active) {
                                                                                                                active =
                                                                                                                        null;
                                                                                                            }
                                                                                                            return true;
                                                                                                        })
                                                                                                .thenApply(
                                                                                                        ignored -> {
                                                                                                            context
                                                                                                                    .resumeTimersAfterRelocationAbort();
                                                                                                            if (completionFailure
                                                                                                                    != null) {
                                                                                                                throw new java
                                                                                                                        .util
                                                                                                                        .concurrent
                                                                                                                        .CompletionException(
                                                                                                                        completionFailure);
                                                                                                            }
                                                                                                            return true;
                                                                                                        });
                                                                                    });
                                                                });
                                            });
                        });
    }

    CompletionStage<Boolean> abort(Seal seal) {
        return abortAsync(seal)
                .whenComplete(
                        (ignored, failure) -> {
                            if (failure != null) {
                                LOGGER.log(
                                        Level.WARNING,
                                        "User Spot relocation abort completion failed",
                                        failure);
                            }
                        });
    }

    CompletionStage<Optional<Committed>> commit(Seal seal) {
        return retainCommit(seal)
                .thenCompose(
                        retained ->
                                retained.isEmpty()
                                        ? CompletableFuture.completedFuture(Optional.empty())
                                        : retained.orElseThrow()
                                                .complete()
                                                .thenApply(
                                                        ignored ->
                                                                retained.map(
                                                                        RelocationCommit
                                                                                ::committed)));
    }

    CompletionStage<Optional<RelocationCommit>> retainCommit(Seal seal) {
        return inStateLane(
                        () -> {
                            if (committing || seal == null || seal != active || seal.aborting()) {
                                return false;
                            }
                            committing = true;
                            return true;
                        })
                .thenCompose(
                        begin -> {
                            if (!begin) {
                                return CompletableFuture.completedFuture(Optional.empty());
                            }
                            return barrier.retainCommit(seal.composite)
                                    .thenCompose(
                                            retained -> {
                                                ZLinkCompositeRelocationBarrier.RelocationCommit
                                                        committed =
                                                                retained.orElseThrow(
                                                                        () ->
                                                                                new IllegalStateException(
                                                                                        "User Spot barrier commit lost a"
                                                                                                + " lane"));
                                                return inStateLane(
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
                                                                retainedActive -> {
                                                                    if (!retainedActive) {
                                                                        return Optional
                                                                                .<RelocationCommit>
                                                                                        empty();
                                                                    }
                                                                    LinkedHashMap<
                                                                                    String,
                                                                                    List<
                                                                                            ZLinkSerialExecutionQueue
                                                                                                    .QueuedRecord>>
                                                                            heldIngress =
                                                                                    new LinkedHashMap<>(
                                                                                            committed
                                                                                                    .records());
                                                                    return Optional.of(
                                                                            new RelocationCommit(
                                                                                    new Committed(
                                                                                            seal
                                                                                                    .generation(),
                                                                                            seal
                                                                                                    .timerEnvelope(),
                                                                                            seal
                                                                                                    .participantActorIds(),
                                                                                            heldIngress),
                                                                                    committed));
                                                                });
                                            })
                                    .exceptionallyCompose(
                                            failure ->
                                                    inStateLane(
                                                                    () -> {
                                                                        committing = false;
                                                                        return null;
                                                                    })
                                                            .thenCompose(
                                                                    ignored ->
                                                                            CompletableFuture
                                                                                    .failedFuture(
                                                                                            failure)));
                        });
    }

    CompletionStage<Optional<Map<String, List<ZLinkSerialExecutionQueue.QueuedRecord>>>>
            freezeIngress(Seal seal) {
        return inStateLane(
                        () -> {
                            if (committing || seal == null || seal != active) {
                                return null;
                            }
                            if (seal.aborting()) {
                                return null;
                            }
                            return seal.composite;
                        })
                .thenCompose(
                        composite -> {
                            if (composite == null) {
                                return CompletableFuture.completedFuture(Optional.empty());
                            }
                            return barrier.freezeIngress(composite)
                                    .thenApply(
                                            frozen -> {
                                                LinkedHashMap<
                                                                String,
                                                                List<
                                                                        ZLinkSerialExecutionQueue
                                                                                .QueuedRecord>>
                                                        held =
                                                                new LinkedHashMap<>(
                                                                        frozen.orElseThrow(
                                                                                () ->
                                                                                        new IllegalStateException(
                                                                                                "User Spot barrier freeze"
                                                                                                        + " lost a lane")));
                                                return Optional.of(
                                                        Collections.unmodifiableMap(held));
                                            });
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
                .handle((ignored, rollbackFailure) -> rollbackFailure)
                .thenCompose(
                        rollbackFailure -> {
                            Throwable cause = failure == null ? rollbackFailure : failure;
                            if (failure != null && rollbackFailure != null) {
                                failure.addSuppressed(rollbackFailure);
                            }
                            return clearSealing()
                                    .handle(
                                            (ignored, clearFailure) -> {
                                                Throwable terminal = cause;
                                                if (clearFailure != null) {
                                                    if (terminal == null) {
                                                        terminal = clearFailure;
                                                    } else {
                                                        terminal.addSuppressed(clearFailure);
                                                    }
                                                }
                                                try {
                                                    if (timerFrozen) {
                                                        context.resumeTimersAfterRelocationAbort();
                                                    }
                                                } catch (RuntimeException resumeFailure) {
                                                    if (terminal == null) {
                                                        terminal = resumeFailure;
                                                    } else {
                                                        terminal.addSuppressed(resumeFailure);
                                                    }
                                                }
                                                if (terminal != null) {
                                                    throw new java.util.concurrent
                                                            .CompletionException(terminal);
                                                }
                                                return Optional.<Seal>empty();
                                            });
                        });
    }

    private CompletionStage<Void> clearSealing() {
        return inStateLane(
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

        CompletionStage<Cut> cut() {
            return retained.cut().thenApply(this::cutFromRetained);
        }

        CompletionStage<Cut> capture() {
            return retained.capture().thenApply(this::cutFromRetained);
        }

        private Cut cutFromRetained(ZLinkCompositeRelocationBarrier.RelocationCommit.Cut cut) {
            return new Cut(
                    new Committed(
                            committed.generation(),
                            committed.timerEnvelope(),
                            committed.participantActorIds(),
                            cut.records()),
                    cut);
        }

        CompletionStage<Boolean> tryFinishCapture(Cut cut) {
            Objects.requireNonNull(cut, "cut");
            return retained.tryFinishCapture(cut.retained);
        }

        CompletionStage<Boolean> tryEstablishDurableCut(Cut cut) {
            Objects.requireNonNull(cut, "cut");
            return retained.tryEstablishDurableCut(cut.retained);
        }

        CompletionStage<Boolean> tryEstablishAndFinishCapture(Cut cut) {
            Objects.requireNonNull(cut, "cut");
            return retained.tryEstablishAndFinishCapture(cut.retained);
        }

        CompletionStage<Boolean> abort() {
            return retained.abort();
        }

        CompletionStage<Void> complete() {
            return retained.complete();
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
