package systems.zlink.framework.runtime.actors;

import systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode;
import systems.zlink.framework.execution.ZLinkExecutionLanePolicy;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.relocation.ZLinkRetainedSerialQueueCommit;
import systems.zlink.framework.runtime.spots.ZLinkSpotSerialExecutor;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executor;
import java.util.function.Function;
import java.util.function.Supplier;

final class ZLinkActorDispatchSerials {
    private final Object runtimeScope;
    private final Function<String, Object> incarnationResolver;
    private final ZLinkActorDispatchTarget legacyTarget;
    private final Function<String, ZLinkActorDispatchTarget> targetResolver;
    // Source cleanup can outlive its Actor registry entry. Keep the accepted
    // turn's Spot target so queue retirement never falls back to a different
    // coordinator after that registry entry disappears.
    private final Map<String, ZLinkActorDispatchTarget> actorTargets = new HashMap<>();
    private final Map<String, CompletionStage<Void>> teardowns = new HashMap<>();
    // Prepare claims admission before the queue entry is installed outside the
    // state lane. Teardown waits for these accepted claims before its barrier.
    private final Map<String, Set<CompletableFuture<Void>>> pendingAdmissions = new HashMap<>();
    // A deferred Join barrier is queued on the Actor's dispatch target at
    // registration time, but a same-node Join re-targets the Actor to the
    // target Spot as soon as that Spot admits it (spec 05-spot-actor-membership
    // §4.2 step 2), before OnJoinedActor and the completion callback ran.
    // Spec 05 §4 lets no Actor payload run before the completion callback
    // ended, so the barrier must stay in front of the Actor across that
    // re-target: every later dispatch target starts behind it.
    private final Map<String, CompletableFuture<Void>> lifecycleBarriers = new HashMap<>();
    private final ZLinkStateLane stateLane;

    ZLinkActorDispatchSerials() {
        this(ZLinkDeferredActorJoinScope.legacyRuntimeScope(), actorId -> actorId, null, null);
    }

    ZLinkActorDispatchSerials(Object runtimeScope, Function<String, Object> incarnationResolver) {
        this(runtimeScope, incarnationResolver, null, null);
    }

    ZLinkActorDispatchSerials(
            Object runtimeScope, Function<String, Object> incarnationResolver, Executor executor) {
        this(runtimeScope, incarnationResolver, executor, null);
    }

    ZLinkActorDispatchSerials(
            Object runtimeScope,
            Function<String, Object> incarnationResolver,
            Executor executor,
            Function<String, ZLinkActorDispatchTarget> targetResolver) {
        this(runtimeScope, incarnationResolver, executor, targetResolver, new ZLinkStateLane());
    }

    ZLinkActorDispatchSerials(
            Object runtimeScope,
            Function<String, Object> incarnationResolver,
            Executor executor,
            Function<String, ZLinkActorDispatchTarget> targetResolver,
            ZLinkStateLane stateLane) {
        this.runtimeScope = Objects.requireNonNull(runtimeScope, "runtimeScope");
        this.stateLane = Objects.requireNonNull(stateLane, "stateLane");
        this.incarnationResolver =
                Objects.requireNonNull(incarnationResolver, "incarnationResolver");
        this.legacyTarget = createLegacyTarget(executor);
        this.targetResolver = targetResolver == null ? ignored -> legacyTarget : targetResolver;
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

    private ZLinkActorDispatchTarget target(String actorId) {
        ZLinkActorDispatchTarget target = targetResolver.apply(actorId);
        return target == null ? legacyTarget : target;
    }

    private ZLinkActorDispatchTarget trackedTarget(String actorId) {
        ZLinkActorDispatchTarget tracked = inStateLane(() -> actorTargets.get(actorId));
        if (tracked != null) {
            return tracked;
        }
        ZLinkActorDispatchTarget resolved = target(actorId);
        return inStateLane(() -> actorTargets.computeIfAbsent(actorId, ignored -> resolved));
    }

    private <T> T inStateLane(Supplier<T> work) {
        if (stateLane.isOnLane()) {
            return work.get();
        }
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

    boolean isCurrent(String actorId) {
        return actorId.equals(
                systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext
                        .currentActorDispatch());
    }

    QueuedTurn prepare(String actorId) {
        return prepare(actorId, target(actorId));
    }

    QueuedTurn prepare(String actorId, ZLinkActorDispatchTarget target) {
        Objects.requireNonNull(target, "target");
        return inStateLane(
                () -> {
                    if (teardowns.containsKey(actorId)) {
                        throw new IllegalStateException(
                                "actor dispatch admission is closed: " + actorId);
                    }
                    ZLinkActorDispatchTarget previous = actorTargets.put(actorId, target);
                    CompletionStage<Void> barrier = lifecycleBarriers.get(actorId);
                    CompletableFuture<Void> admission = new CompletableFuture<>();
                    pendingAdmissions
                            .computeIfAbsent(actorId, ignored -> new HashSet<>())
                            .add(admission);
                    return new QueuedTurn(
                            actorId,
                            target,
                            barrier != null && previous != null && previous != target
                                    ? barrier
                                    : null,
                            admission);
                });
    }

    private static void installBarrier(QueuedTurn turn) {
        if (turn.precedingBarrier() != null) {
            turn.target().executeActorLifecycleNext(turn.actorId(), turn::precedingBarrier);
        }
    }

    ZLinkSerialExecutionQueue relocationLane(String actorId) {
        return trackedTarget(actorId).actorRelocationLane(actorId);
    }

    void remove(String actorId) {
        ZLinkActorDispatchTarget target =
                inStateLane(
                        () -> {
                            releaseLifecycleBarrierHold(actorId);
                            teardowns.remove(actorId);
                            return actorTargets.remove(actorId);
                        });
        if (target != null) {
            target.removeActorQueue(actorId);
        }
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
        return enqueueAfterAdmission(
                turn.actorId,
                turn.admission(),
                () -> {
                    installBarrier(turn);
                    Object incarnation = incarnationResolver.apply(turn.actorId);
                    Supplier<CompletionStage<Void>> turnOperation =
                            () -> runTurn(turn.actorId, incarnation, operation);
                    return turn.target.executeActor(turn.actorId, payloadBytes, turnOperation);
                });
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
        // Only the first part of a multi-part message carries an accepted
        // journal record; an empty record has nothing to replay.
        return enqueueAfterAdmission(
                turn.actorId,
                turn.admission(),
                () -> {
                    installBarrier(turn);
                    Object incarnation = incarnationResolver.apply(turn.actorId);
                    Supplier<CompletionStage<Void>> turnOperation =
                            () -> runTurn(turn.actorId, incarnation, operation);
                    return acceptedJournalRecord == null || acceptedJournalRecord.length == 0
                            ? turn.target.executeActor(turn.actorId, turnOperation)
                            : turn.target.executeActor(
                                    turn.actorId,
                                    acceptedJournalRecord,
                                    turnOperation,
                                    relocationRelease);
                });
    }

    CompletionStage<Void> enqueueLazyRecord(
            QueuedTurn turn,
            Supplier<byte[]> acceptedJournalRecord,
            long acceptedJournalRecordSizeHint,
            Supplier<CompletionStage<Void>> operation,
            Runnable relocationRelease) {
        return enqueueAfterAdmission(
                turn.actorId,
                turn.admission(),
                () -> {
                    installBarrier(turn);
                    Object incarnation = incarnationResolver.apply(turn.actorId);
                    Supplier<CompletionStage<Void>> turnOperation =
                            () -> runTurn(turn.actorId, incarnation, operation);
                    return turn.target.executeActorLazyRecord(
                            turn.actorId,
                            acceptedJournalRecord,
                            acceptedJournalRecordSizeHint,
                            turnOperation,
                            relocationRelease);
                });
    }

    CompletionStage<Void> beginTeardown(String actorId, Supplier<CompletionStage<Void>> cleanup) {
        boolean current = isCurrent(actorId);
        CompletionStage<TeardownSetup> setup =
                stateLane.runAsync(() -> createTeardownSetup(actorId));
        return setup.thenComposeAsync(
                accepted -> {
                    if (accepted.created()) {
                        startTeardown(actorId, accepted, cleanup);
                    }
                    // Cleanup queued behind this turn must not hold this turn open.
                    return current ? CompletableFuture.completedFuture(null) : accepted.teardown();
                });
    }

    /** State-lane only: makes the sole admission and cleanup terminal decision. */
    private TeardownSetup createTeardownSetup(String actorId) {
        CompletionStage<Void> existing = teardowns.get(actorId);
        if (existing != null) {
            return new TeardownSetup(existing, null, null, List.of(), false);
        }
        releaseLifecycleBarrierHold(actorId);
        CompletableFuture<Void> terminal = new CompletableFuture<>();
        teardowns.put(actorId, terminal);
        ZLinkActorDispatchTarget target = actorTargets.get(actorId);
        return new TeardownSetup(
                terminal,
                target == null ? target(actorId) : target,
                terminal,
                List.copyOf(pendingAdmissions.getOrDefault(actorId, Set.of())),
                true);
    }

    CompletionStage<Void> enqueueBarrier(
            String actorId, Supplier<CompletionStage<Void>> operation) {
        return enqueueBarrier(actorId, incarnationResolver.apply(actorId), operation);
    }

    CompletionStage<Void> enqueueBarrier(
            String actorId, Object incarnation, Supplier<CompletionStage<Void>> operation) {
        ZLinkActorDispatchTarget current = trackedTarget(actorId);
        // The hold that a re-target installs in front of the new queue
        // releases on the barrier's terminal, success or failure alike.
        CompletableFuture<Void> released = new CompletableFuture<>();
        stateLane.runAsync(() -> lifecycleBarriers.put(actorId, released));
        CompletionStage<Void> barrier =
                current.executeActorLifecycleNext(
                        actorId, () -> runTurn(actorId, incarnation, operation));
        return barrier.handle((ignored, failure) -> failure)
                .thenCompose(
                        failure ->
                                stateLane.runAsync(
                                        () -> {
                                            lifecycleBarriers.remove(actorId, released);
                                            return failure;
                                        }))
                .thenComposeAsync(
                        failure -> {
                            released.complete(null);
                            return failure == null
                                    ? CompletableFuture.completedFuture(null)
                                    : CompletableFuture.failedFuture(failure);
                        });
    }

    /** State-lane only: releases the hold of a pending lifecycle barrier. */
    private void releaseLifecycleBarrierHold(String actorId) {
        CompletableFuture<Void> released = lifecycleBarriers.remove(actorId);
        if (released != null) {
            released.complete(null);
        }
    }

    Optional<ZLinkSerialExecutionQueue.RelocationSeal> trySeal(String actorId) {
        return trackedTarget(actorId).trySealActorRelocation(actorId);
    }

    boolean abort(String actorId, ZLinkSerialExecutionQueue.RelocationSeal seal) {
        return trackedTarget(actorId).abortActorRelocation(actorId, seal);
    }

    Optional<List<ZLinkSerialExecutionQueue.QueuedRecord>> commit(
            String actorId, ZLinkSerialExecutionQueue.RelocationSeal seal) {
        return trackedTarget(actorId).commitActorRelocation(actorId, seal);
    }

    Optional<ZLinkRetainedSerialQueueCommit.Commit> retainCommit(
            String actorId, ZLinkSerialExecutionQueue.RelocationSeal seal) {
        return trackedTarget(actorId).retainActorRelocationCommit(actorId, seal);
    }

    Optional<List<ZLinkSerialExecutionQueue.QueuedRecord>> freezeIngress(
            String actorId, ZLinkSerialExecutionQueue.RelocationSeal seal) {
        return trackedTarget(actorId).freezeActorRelocationIngress(actorId, seal);
    }

    CompletionStage<Void> awaitQuiescence() {
        Map.Entry<Map<String, ZLinkActorDispatchTarget>, List<CompletionStage<Void>>> snapshot =
                inStateLane(
                        () -> Map.entry(Map.copyOf(actorTargets), List.copyOf(teardowns.values())));
        List<CompletableFuture<?>> barriers = new ArrayList<>();
        snapshot.getKey()
                .forEach(
                        (actorId, target) ->
                                barriers.add(
                                        target.awaitActorQuiescence(actorId)
                                                .toCompletableFuture()));
        snapshot.getValue().forEach(terminal -> barriers.add(terminal.toCompletableFuture()));
        return CompletableFuture.allOf(barriers.toArray(CompletableFuture[]::new));
    }

    <T> CompletionStage<T> runTurn(String actorId, Supplier<CompletionStage<T>> operation) {
        return runTurn(actorId, incarnationResolver.apply(actorId), operation);
    }

    <T> CompletionStage<T> runTurn(
            String actorId, Object incarnation, Supplier<CompletionStage<T>> operation) {
        try (systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext.Scope
                        ignored =
                                systems.zlink.framework.runtime.internal.handlers
                                        .ZLinkSuspendInvocationContext.enterActorDispatch(actorId);
                ZLinkDeferredActorJoinScope.Scope joins =
                        ZLinkDeferredActorJoinScope.enter(
                                runtimeScope,
                                Objects.requireNonNull(incarnation, "actor incarnation"),
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
        } catch (RuntimeException | Error ex) {
            return CompletableFuture.failedFuture(ex);
        }
    }

    private CompletionStage<Void> enqueueAfterAdmission(
            String actorId,
            CompletableFuture<Void> admission,
            Supplier<CompletionStage<Void>> enqueue) {
        try {
            CompletionStage<Void> queued = enqueue.get();
            admission.complete(null);
            removeAdmission(actorId, admission);
            return queued;
        } catch (RuntimeException | Error failure) {
            admission.completeExceptionally(failure);
            removeAdmission(actorId, admission);
            return CompletableFuture.failedFuture(failure);
        }
    }

    private void removeAdmission(String actorId, CompletableFuture<Void> admission) {
        stateLane.runAsync(
                () -> {
                    Set<CompletableFuture<Void>> admissions = pendingAdmissions.get(actorId);
                    if (admissions != null) {
                        admissions.remove(admission);
                        if (admissions.isEmpty()) {
                            pendingAdmissions.remove(actorId);
                        }
                    }
                });
    }

    private void startTeardown(
            String actorId, TeardownSetup setup, Supplier<CompletionStage<Void>> cleanup) {
        CompletableFuture<?>[] admitted =
                setup.pendingAdmissions().stream()
                        .map(CompletableFuture::toCompletableFuture)
                        .toArray(CompletableFuture[]::new);
        CompletableFuture.allOf(admitted)
                .whenComplete(
                        (ignored, admissionError) -> {
                            if (admissionError != null) {
                                completeTeardown(actorId, setup, admissionError);
                                return;
                            }
                            try {
                                setup.target()
                                        .executeActorLifecycle(actorId, cleanup)
                                        .whenComplete(
                                                (nothing, error) ->
                                                        completeTeardown(actorId, setup, error));
                            } catch (RuntimeException | Error failure) {
                                completeTeardown(actorId, setup, failure);
                            }
                        });
    }

    private void completeTeardown(String actorId, TeardownSetup setup, Throwable error) {
        CompletionStage<Void> retired =
                error == null
                        ? CompletableFuture.runAsync(() -> setup.target().removeActorQueue(actorId))
                        : CompletableFuture.failedFuture(error);
        retired.thenCompose(
                        ignored ->
                                stateLane.runAsync(
                                        () -> {
                                            actorTargets.remove(actorId, setup.target());
                                            teardowns.remove(actorId, setup.terminal());
                                        }))
                .whenComplete(
                        (ignored, failure) -> {
                            if (failure == null) {
                                setup.terminal().complete(null);
                            } else {
                                setup.terminal().completeExceptionally(failure);
                            }
                        });
    }

    record QueuedTurn(
            String actorId,
            ZLinkActorDispatchTarget target,
            CompletionStage<Void> precedingBarrier,
            CompletableFuture<Void> admission) {}

    private record TeardownSetup(
            CompletionStage<Void> teardown,
            ZLinkActorDispatchTarget target,
            CompletableFuture<Void> terminal,
            List<CompletableFuture<Void>> pendingAdmissions,
            boolean created) {}
}
