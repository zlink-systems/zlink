package systems.zlink.framework.runtime.actors;

import systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.execution.ZLinkExecutionLanePolicy;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.relocation.ZLinkRetainedSerialQueueCommit;
import systems.zlink.framework.runtime.messaging.ZLinkFrameworkErrorOrigin;
import systems.zlink.framework.runtime.spots.ZLinkSpotSerialExecutor;

import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executor;
import java.util.function.Function;
import java.util.function.Supplier;
import java.util.stream.Stream;

final class ZLinkActorDispatchSerials {
    private static final CompletionStage<Void> COMPLETED = CompletableFuture.completedFuture(null);
    private final Object runtimeScope;
    private final ZLinkActorDispatchTarget legacyTarget;
    private final Function<String, CompletionStage<ZLinkActorDispatchTarget.ActivationSnapshot>>
            targetResolver;
    // Source cleanup can outlive its Actor registry entry. Keep the accepted
    // turn's Spot target so queue retirement never falls back to a different
    // coordinator after that registry entry disappears.
    private final Map<String, QueuedTurn> actorTargets = new HashMap<>();
    private final ZLinkStateLane stateLane = new ZLinkStateLane();
    private final Map<String, CompletionStage<Void>> teardowns = new HashMap<>();
    // A deferred Join barrier is queued on the Actor's dispatch target at
    // registration time, but a same-node Join re-targets the Actor to the
    // target Spot as soon as that Spot admits it (spec 05-spot-actor-membership
    // §4.2 step 2), before OnJoinedActor and the completion callback ran.
    // Spec 05 §4 lets no Actor payload run before the completion callback
    // ended, so the barrier must stay in front of the Actor across that
    // re-target: every later dispatch target starts behind it.
    private final Map<String, CompletableFuture<Void>> lifecycleBarriers = new HashMap<>();

    ZLinkActorDispatchSerials() {
        this(ZLinkDeferredActorJoinScope.legacyRuntimeScope(), null, null);
    }

    ZLinkActorDispatchSerials(
            Object runtimeScope,
            Executor executor,
            Function<String, CompletionStage<ZLinkActorDispatchTarget.ActivationSnapshot>>
                    targetResolver) {
        this.runtimeScope = Objects.requireNonNull(runtimeScope, "runtimeScope");
        this.legacyTarget = createLegacyTarget(executor);
        this.targetResolver =
                targetResolver == null
                        ? actorId ->
                                CompletableFuture.completedFuture(
                                        new ZLinkActorDispatchTarget.ActivationSnapshot(
                                                legacyTarget, actorId))
                        : targetResolver;
    }

    private static ZLinkActorDispatchTarget createLegacyTarget(Executor executor) {
        ZLinkSerialExecutionQueue queue =
                new ZLinkSerialExecutionQueue(ZLinkExecutionLanePolicy.spot());
        return new ZLinkSpotSerialExecutor(
                queue,
                executor == null ? Runnable::run : executor,
                executor == null ? Runnable::run : executor,
                ZLinkUserSpotExecutionMode.PER_ACTOR,
                false);
    }

    private CompletionStage<ZLinkActorDispatchTarget.ActivationSnapshot> targetAsync(
            String actorId) {
        return targetResolver
                .apply(actorId)
                .thenCompose(
                        snapshot -> {
                            if (snapshot == null || snapshot.incarnation() == null) {
                                return CompletableFuture.failedFuture(
                                        ZLinkFrameworkErrorOrigin.framework(
                                                ZLinkFrameworkErrorKind.NOT_FOUND,
                                                "Actor is not registered"));
                            }
                            return CompletableFuture.completedFuture(
                                    snapshot.target() == null
                                            ? new ZLinkActorDispatchTarget.ActivationSnapshot(
                                                    legacyTarget, snapshot.incarnation())
                                            : snapshot);
                        });
    }

    private CompletionStage<QueuedTurn> resolveTurnAsync(
            String actorId, ZLinkActorDispatchTarget.ActivationSnapshot snapshot) {
        return snapshot.target()
                .claimActorQueue(actorId)
                .thenApply(
                        activation ->
                                new QueuedTurn(
                                        actorId,
                                        snapshot.target(),
                                        COMPLETED,
                                        snapshot.incarnation(),
                                        activation));
    }

    private CompletionStage<QueuedTurn> trackedTurnAsync(String actorId) {
        return onRegistry(() -> actorTargets.get(actorId))
                .thenCompose(
                        tracked -> {
                            if (tracked != null) return CompletableFuture.completedFuture(tracked);
                            return targetAsync(actorId)
                                    .thenCompose(snapshot -> resolveTurnAsync(actorId, snapshot))
                                    .thenCompose(
                                            resolved ->
                                                    onRegistry(
                                                            () ->
                                                                    actorTargets.computeIfAbsent(
                                                                            actorId,
                                                                            ignored -> resolved)));
                        });
    }

    private <T> CompletionStage<T> onRegistry(Supplier<T> work) {
        return stateLane.runNowOrQueue(work);
    }

    boolean isCurrent(String actorId) {
        return actorId.equals(
                systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext
                        .currentActorDispatch());
    }

    CompletionStage<QueuedTurn> prepareAsync(String actorId) {
        return targetAsync(actorId).thenCompose(snapshot -> prepareAsync(actorId, snapshot));
    }

    CompletionStage<QueuedTurn> prepareAsync(String actorId, ZLinkActorDispatchTarget target) {
        return targetAsync(actorId)
                .thenCompose(
                        snapshot ->
                                prepareAsync(
                                        actorId,
                                        new ZLinkActorDispatchTarget.ActivationSnapshot(
                                                Objects.requireNonNull(target, "target"),
                                                snapshot.incarnation())));
    }

    private CompletionStage<QueuedTurn> prepareAsync(
            String actorId, ZLinkActorDispatchTarget.ActivationSnapshot snapshot) {
        return onRegistry(() -> prepareOnRegistry(actorId, snapshot, null))
                .thenCompose(
                        previous ->
                                previous != null
                                        ? CompletableFuture.completedFuture(previous)
                                        : resolveTurnAsync(actorId, snapshot)
                                                .thenCompose(
                                                        resolved ->
                                                                onRegistry(
                                                                        () ->
                                                                                prepareOnRegistry(
                                                                                        actorId,
                                                                                        snapshot,
                                                                                        resolved))))
                .thenApply(
                        prepared -> {
                            if (prepared.admission() != null) {
                                try {
                                    prepared.turn()
                                            .activation()
                                            .executeLifecycleNext(
                                                    () -> prepared.barrier(), prepared.admission());
                                } catch (RuntimeException | Error failure) {
                                    prepared.admission().completeExceptionally(failure);
                                }
                            }
                            return prepared.turn();
                        });
    }

    private PreparedTurn prepareOnRegistry(
            String actorId,
            ZLinkActorDispatchTarget.ActivationSnapshot snapshot,
            QueuedTurn resolved) {
        QueuedTurn previous = actorTargets.get(actorId);
        if (previous != null
                && ((previous.target() == snapshot.target()
                                && previous.incarnation() == snapshot.incarnation())
                        || teardowns.containsKey(actorId))) {
            return new PreparedTurn(previous, null, null);
        }
        if (resolved == null) return null;
        CompletionStage<Void> barrier = lifecycleBarriers.get(actorId);
        CompletableFuture<Void> admission =
                barrier != null && previous != null ? new CompletableFuture<>() : null;
        QueuedTurn turn =
                new QueuedTurn(
                        actorId,
                        snapshot.target(),
                        admission == null ? COMPLETED : admission,
                        snapshot.incarnation(),
                        resolved.activation());
        actorTargets.put(actorId, turn);
        return new PreparedTurn(turn, barrier, admission);
    }

    private record PreparedTurn(
            QueuedTurn turn, CompletionStage<Void> barrier, CompletableFuture<Void> admission) {}

    CompletionStage<ZLinkSerialExecutionQueue> relocationLaneAsync(String actorId) {
        return trackedTurnAsync(actorId).thenApply(turn -> turn.activation().relocationLane());
    }

    CompletionStage<Void> removeAsync(String actorId) {
        CompletableFuture<Void> terminal = new CompletableFuture<>();
        return onRegistry(
                        () -> {
                            CompletionStage<Void> existing = teardowns.get(actorId);
                            if (existing != null) {
                                return new RemovalClaim(null, existing, null, null);
                            }
                            teardowns.put(actorId, terminal);
                            QueuedTurn tracked = actorTargets.get(actorId);
                            return new RemovalClaim(
                                    tracked == null ? null : tracked.target(),
                                    null,
                                    lifecycleBarriers.remove(actorId),
                                    tracked == null ? null : tracked.activation());
                        })
                .thenCompose(
                        claim -> {
                            if (claim.barrier() != null) claim.barrier().complete(null);
                            if (claim.existing() != null) {
                                return claim.existing();
                            }
                            CompletionStage<Void> removed;
                            try {
                                removed =
                                        claim.target() == null
                                                ? CompletableFuture.completedFuture(null)
                                                : claim.target()
                                                        .removeActorQueueAsync(
                                                                actorId, claim.activation());
                            } catch (RuntimeException | Error failure) {
                                removed = CompletableFuture.failedFuture(failure);
                            }
                            removed.whenComplete(
                                    (ignored, removalFailure) ->
                                            onRegistry(
                                                            () -> {
                                                                if (removalFailure == null) {
                                                                    actorTargets.computeIfPresent(
                                                                            actorId,
                                                                            (id, turn) ->
                                                                                    turn
                                                                                                            .activation()
                                                                                                    == claim
                                                                                                            .activation()
                                                                                            ? null
                                                                                            : turn);
                                                                }
                                                                teardowns.remove(actorId, terminal);
                                                                return null;
                                                            })
                                                    .whenComplete(
                                                            (nothing, registrationFailure) -> {
                                                                Throwable failure =
                                                                        removalFailure == null
                                                                                ? registrationFailure
                                                                                : removalFailure;
                                                                if (failure == null) {
                                                                    terminal.complete(null);
                                                                } else {
                                                                    terminal.completeExceptionally(
                                                                            failure);
                                                                }
                                                            }));
                            return terminal;
                        });
    }

    CompletionStage<Void> enqueue(QueuedTurn turn, Supplier<CompletionStage<Void>> operation) {
        return enqueue(turn, null, operation);
    }

    CompletionStage<Void> enqueue(
            QueuedTurn turn, long payloadBytes, Supplier<CompletionStage<Void>> operation) {
        return enqueue(turn, payloadBytes, operation, () -> {});
    }

    CompletionStage<Void> enqueue(
            QueuedTurn turn,
            long payloadBytes,
            Supplier<CompletionStage<Void>> operation,
            Runnable relocationRelease) {
        Supplier<CompletionStage<Void>> turnOperation = () -> runTurn(turn, operation);
        CompletableFuture<Void> admission = new CompletableFuture<>();
        return enqueueAfterAdmission(
                turn,
                admission,
                accepted ->
                        turn.target.executeActor(
                                turn.activation, payloadBytes, turnOperation, accepted));
    }

    CompletionStage<Void> enqueue(
            QueuedTurn turn,
            byte[] acceptedJournalRecord,
            Supplier<CompletionStage<Void>> operation) {
        return enqueue(turn, acceptedJournalRecord, operation, () -> {});
    }

    CompletionStage<Void> enqueue(
            QueuedTurn turn,
            byte[] acceptedJournalRecord,
            Supplier<CompletionStage<Void>> operation,
            Runnable relocationRelease) {
        Supplier<CompletionStage<Void>> turnOperation = () -> runTurn(turn, operation);
        CompletableFuture<Void> admission = new CompletableFuture<>();
        return enqueueAfterAdmission(
                turn,
                admission,
                accepted ->
                        acceptedJournalRecord == null || acceptedJournalRecord.length == 0
                                ? turn.target.executeActor(
                                        turn.activation, 0, turnOperation, accepted)
                                : turn.target.executeActor(
                                        turn.activation,
                                        acceptedJournalRecord,
                                        turnOperation,
                                        relocationRelease,
                                        accepted));
    }

    CompletionStage<Void> enqueueLazyRecord(
            QueuedTurn turn,
            Supplier<byte[]> acceptedJournalRecord,
            long acceptedJournalRecordSizeHint,
            Supplier<CompletionStage<Void>> operation,
            Runnable relocationRelease) {
        Supplier<CompletionStage<Void>> turnOperation = () -> runTurn(turn, operation);
        CompletableFuture<Void> admission = new CompletableFuture<>();
        return enqueueAfterAdmission(
                turn,
                admission,
                accepted ->
                        turn.target.executeActorLazyRecord(
                                turn.activation,
                                acceptedJournalRecord,
                                acceptedJournalRecordSizeHint,
                                turnOperation,
                                relocationRelease,
                                accepted));
    }

    CompletionStage<Void> awaitTeardown(String actorId) {
        return onRegistry(() -> teardowns.getOrDefault(actorId, COMPLETED))
                .thenCompose(terminal -> terminal);
    }

    CompletionStage<Void> beginTeardown(String actorId, Supplier<CompletionStage<Void>> cleanup) {
        boolean current = isCurrent(actorId);
        return trackedTurnAsync(actorId)
                .thenCompose(
                        resolved ->
                                onRegistry(
                                        () -> {
                                            CompletionStage<Void> existing = teardowns.get(actorId);
                                            if (existing != null)
                                                return new TeardownSetup(
                                                        existing, null, null, false, null, null);
                                            CompletableFuture<Void> terminal =
                                                    new CompletableFuture<>();
                                            teardowns.put(actorId, terminal);
                                            QueuedTurn tracked =
                                                    actorTargets.computeIfAbsent(
                                                            actorId, ignored -> resolved);
                                            return new TeardownSetup(
                                                    terminal,
                                                    tracked.target(),
                                                    terminal,
                                                    true,
                                                    lifecycleBarriers.remove(actorId),
                                                    tracked.activation());
                                        }))
                .thenCompose(
                        setup -> {
                            if (setup.barrier() != null) setup.barrier().complete(null);
                            if (setup.created()) startTeardown(actorId, setup, cleanup);
                            return current ? COMPLETED : setup.teardown();
                        });
    }

    CompletionStage<Void> enqueueBarrier(
            String actorId, Supplier<CompletionStage<Void>> operation) {
        CompletableFuture<Void> released = new CompletableFuture<>();
        return trackedTurnAsync(actorId)
                .thenCompose(
                        resolved ->
                                onRegistry(
                                        () -> {
                                            QueuedTurn tracked =
                                                    actorTargets.computeIfAbsent(
                                                            actorId, ignored -> resolved);
                                            lifecycleBarriers.put(actorId, released);
                                            return tracked;
                                        }))
                .thenCompose(
                        turn -> {
                            CompletableFuture<Void> admitted = new CompletableFuture<>();
                            CompletionStage<Void> barrier =
                                    turn.activation()
                                            .executeLifecycleNext(
                                                    () -> runTurn(turn, operation), admitted);
                            return admitted.thenCompose(ignored -> barrier);
                        })
                .whenComplete(
                        (ignored, error) -> {
                            released.complete(null);
                            onRegistry(() -> lifecycleBarriers.remove(actorId, released));
                        });
    }

    CompletionStage<Boolean> abortAsync(
            String actorId, ZLinkSerialExecutionQueue.RelocationSeal seal) {
        return trackedTurnAsync(actorId).thenApply(turn -> turn.activation().abortRelocation(seal));
    }

    CompletionStage<Optional<ZLinkRetainedSerialQueueCommit.Commit>> retainCommitAsync(
            String actorId, ZLinkSerialExecutionQueue.RelocationSeal seal) {
        return trackedTurnAsync(actorId)
                .thenApply(turn -> turn.activation().retainRelocationCommit(seal));
    }

    CompletionStage<Void> awaitQuiescence() {
        return onRegistry(
                        () -> Map.entry(Map.copyOf(actorTargets), List.copyOf(teardowns.values())))
                .thenCompose(
                        snapshot -> {
                            CompletableFuture<?>[] barriers =
                                    Stream.concat(
                                                    snapshot.getKey().entrySet().stream()
                                                            .map(
                                                                    entry ->
                                                                            entry.getValue()
                                                                                    .activation()
                                                                                    .awaitQuiescence()),
                                                    snapshot.getValue().stream())
                                            .map(CompletionStage::toCompletableFuture)
                                            .toArray(CompletableFuture[]::new);
                            return CompletableFuture.allOf(barriers);
                        });
    }

    <T> CompletionStage<T> runTurn(QueuedTurn turn, Supplier<CompletionStage<T>> operation) {
        String actorId = turn.actorId();
        try (systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext.Scope
                        ignored =
                                systems.zlink.framework.runtime.internal.handlers
                                        .ZLinkSuspendInvocationContext.enterActorDispatch(actorId);
                ZLinkDeferredActorJoinScope.Scope joins =
                        ZLinkDeferredActorJoinScope.enter(
                                runtimeScope,
                                Objects.requireNonNull(turn.incarnation(), "actor incarnation"),
                                actorId)) {
            CompletionStage<T> handler = operation.get();
            CompletableFuture<T> completed = new CompletableFuture<>();
            joins.finish(handler, null)
                    .whenComplete(
                            (nothing, error) -> {
                                if (error != null) {
                                    completed.completeExceptionally(error);
                                    return;
                                }
                                handler.whenComplete(
                                        (value, handlerError) -> {
                                            if (handlerError != null) {
                                                completed.completeExceptionally(handlerError);
                                            } else {
                                                completed.complete(value);
                                            }
                                        });
                            });
            return completed;
        } catch (RuntimeException ex) {
            return CompletableFuture.failedFuture(ex);
        }
    }

    private CompletionStage<Void> enqueueAfterAdmission(
            QueuedTurn turn,
            CompletableFuture<Void> admission,
            Function<CompletableFuture<Void>, CompletionStage<Void>> enqueue) {
        CompletionStage<Void> queued;
        try {
            queued =
                    turn.barrierAdmission() == COMPLETED
                            ? enqueue.apply(admission)
                            : turn.barrierAdmission()
                                    .thenCompose(ignored -> enqueue.apply(admission));
        } catch (RuntimeException | Error failure) {
            queued = CompletableFuture.failedFuture(failure);
        }
        queued.whenComplete(
                (ignored, error) -> {
                    if (error != null) {
                        admission.completeExceptionally(error);
                    }
                });
        return queued;
    }

    private void startTeardown(
            String actorId, TeardownSetup setup, Supplier<CompletionStage<Void>> cleanup) {
        try {
            setup.activation().relocationLane().sealClosingAdmission();
            setup.activation()
                    .executeLifecycle(cleanup)
                    .whenComplete((ignored, error) -> completeTeardown(actorId, setup, error));
        } catch (RuntimeException failure) {
            completeTeardown(actorId, setup, failure);
        }
    }

    private void completeTeardown(String actorId, TeardownSetup setup, Throwable error) {
        CompletionStage<Void> removed;
        try {
            removed = setup.target().removeActorQueueAsync(actorId, setup.activation());
        } catch (RuntimeException | Error failure) {
            removed = CompletableFuture.failedFuture(failure);
        }
        removed.whenComplete(
                (ignored, removalFailure) -> {
                    CompletionStage<Void> registration;
                    try {
                        registration =
                                onRegistry(
                                        () -> {
                                            teardowns.remove(actorId, setup.terminal());
                                            if (removalFailure == null) {
                                                actorTargets.computeIfPresent(
                                                        actorId,
                                                        (id, turn) ->
                                                                turn.activation()
                                                                                == setup
                                                                                        .activation()
                                                                        ? null
                                                                        : turn);
                                            }
                                            return null;
                                        });
                    } catch (RuntimeException | Error failure) {
                        registration = CompletableFuture.failedFuture(failure);
                    }
                    registration.whenComplete(
                            (nothing, registrationFailure) -> {
                                Throwable terminalFailure = error == null ? removalFailure : error;
                                if (error != null
                                        && removalFailure != null
                                        && error != removalFailure) {
                                    error.addSuppressed(removalFailure);
                                }
                                if (terminalFailure == null) {
                                    terminalFailure = registrationFailure;
                                } else if (registrationFailure != null) {
                                    terminalFailure.addSuppressed(registrationFailure);
                                }
                                if (terminalFailure == null) {
                                    setup.terminal().complete(null);
                                } else {
                                    setup.terminal().completeExceptionally(terminalFailure);
                                }
                            });
                });
    }

    record QueuedTurn(
            String actorId,
            ZLinkActorDispatchTarget target,
            CompletionStage<Void> barrierAdmission,
            Object incarnation,
            ZLinkActorSerialExecutor activation) {}

    private record TeardownSetup(
            CompletionStage<Void> teardown,
            ZLinkActorDispatchTarget target,
            CompletableFuture<Void> terminal,
            boolean created,
            CompletableFuture<Void> barrier,
            ZLinkActorSerialExecutor activation) {}

    private record RemovalClaim(
            ZLinkActorDispatchTarget target,
            CompletionStage<Void> existing,
            CompletableFuture<Void> barrier,
            ZLinkActorSerialExecutor activation) {}
}
