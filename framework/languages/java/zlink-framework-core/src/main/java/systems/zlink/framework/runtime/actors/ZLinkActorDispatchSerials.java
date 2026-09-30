package systems.zlink.framework.runtime.actors;

import systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode;
import systems.zlink.framework.execution.ZLinkExecutionLanePolicy;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.relocation.ZLinkRetainedSerialQueueCommit;
import systems.zlink.framework.runtime.spots.ZLinkSpotSerialExecutor;

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
import java.util.stream.Stream;

final class ZLinkActorDispatchSerials {
    private static final CompletionStage<Void> COMPLETED = CompletableFuture.completedFuture(null);
    private final Object runtimeScope;
    private final Function<String, Object> incarnationResolver;
    private final ZLinkActorDispatchTarget legacyTarget;
    private final Function<String, ZLinkActorDispatchTarget> targetResolver;
    private final Set<String> activeActorIds = new HashSet<>();
    // Source cleanup can outlive its Actor registry entry. Keep the accepted
    // turn's Spot target so queue retirement never falls back to a different
    // coordinator after that registry entry disappears.
    private final Map<String, ZLinkActorDispatchTarget> actorTargets = new HashMap<>();
    private final Map<String, CompletionStage<Void>> teardowns = new HashMap<>();
    private final Map<String, Object> admissionGates = new HashMap<>();
    // An accepted packet claims admission while its queue entry is installed
    // outside the admission monitor.  Teardown waits for those claims before
    // it puts its lifecycle barrier on the queue, preserving the monitor-era
    // admission ordering without invoking the queue under that monitor.
    private final Map<String, Set<CompletableFuture<Void>>> pendingAdmissions = new HashMap<>();
    // A deferred Join barrier is queued on the Actor's dispatch target at
    // registration time, but a same-node Join re-targets the Actor to the
    // target Spot as soon as that Spot admits it (spec 05-spot-actor-membership
    // §4.2 step 2), before OnJoinedActor and the completion callback ran.
    // Spec 05 §4 lets no Actor payload run before the completion callback
    // ended, so the barrier must stay in front of the Actor across that
    // re-target: every later dispatch target starts behind it.
    private final Map<String, CompletableFuture<Void>> lifecycleBarriers = new HashMap<>();
    private final ZLinkStateLane stateLane = new ZLinkStateLane();

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
        this.runtimeScope = Objects.requireNonNull(runtimeScope, "runtimeScope");
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

    private Object admissionGate(String actorId) {
        return inStateLane(() -> admissionGates.computeIfAbsent(actorId, ignored -> new Object()));
    }

    boolean isCurrent(String actorId) {
        return actorId.equals(
                systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext
                        .currentActorDispatch());
    }

    boolean isActive(String actorId) {
        return inStateLane(() -> activeActorIds.contains(actorId));
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
                    CompletionStage<Void> barrierAdmission = COMPLETED;
                    if (barrier != null && previous != null && previous != target) {
                        //  Admit the barrier before this prepared turn reaches
                        //  the target Actor queue. Its terminal remains pending.
                        CompletableFuture<Void> accepted = new CompletableFuture<>();
                        target.executeActorLifecycleNext(actorId, () -> barrier, accepted);
                        barrierAdmission = accepted;
                    }
                    return new QueuedTurn(actorId, target, barrierAdmission);
                });
    }

    ZLinkSerialExecutionQueue relocationLane(String actorId) {
        return trackedTarget(actorId).actorRelocationLane(actorId);
    }

    CompletionStage<ZLinkSerialExecutionQueue> relocationLaneAsync(String actorId) {
        return trackedTargetAsync(actorId)
                .thenCompose(target -> target.actorRelocationLaneAsync(actorId));
    }

    CompletionStage<Void> removeAsync(String actorId) {
        CompletableFuture<Void> terminal = new CompletableFuture<>();
        return stateLane
                .runNowOrQueue(
                        () -> {
                            CompletionStage<Void> existing = teardowns.get(actorId);
                            if (existing != null) {
                                return new RemovalClaim(null, existing);
                            }
                            teardowns.put(actorId, terminal);
                            releaseLifecycleBarrierHold(actorId);
                            return new RemovalClaim(actorTargets.get(actorId), null);
                        })
                .thenCompose(
                        claim -> {
                            if (claim.existing() != null) {
                                return claim.existing();
                            }
                            CompletionStage<Void> removed;
                            try {
                                removed =
                                        claim.target() == null
                                                ? CompletableFuture.completedFuture(null)
                                                : claim.target().removeActorQueueAsync(actorId);
                            } catch (RuntimeException | Error failure) {
                                removed = CompletableFuture.failedFuture(failure);
                            }
                            removed.whenComplete(
                                    (ignored, removalFailure) ->
                                            stateLane
                                                    .runNowOrQueue(
                                                            () -> {
                                                                if (removalFailure == null) {
                                                                    actorTargets.remove(
                                                                            actorId,
                                                                            claim.target());
                                                                    activeActorIds.remove(actorId);
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
        Object admissionGate = admissionGate(turn.actorId);
        Supplier<CompletionStage<Void>> turnOperation =
                () -> {
                    inStateLane(
                            () -> {
                                activeActorIds.add(turn.actorId);
                                return null;
                            });
                    return runTurn(turn.actorId, operation)
                            .whenComplete(
                                    (ignored, error) -> {
                                        inStateLane(
                                                () -> {
                                                    activeActorIds.remove(turn.actorId);
                                                    return null;
                                                });
                                    });
                };
        CompletableFuture<Void> admission = new CompletableFuture<>();
        synchronized (admissionGate) {
            boolean closed = inStateLane(() -> teardowns.containsKey(turn.actorId));
            if (closed) {
                return CompletableFuture.failedFuture(
                        new IllegalStateException(
                                "actor dispatch admission is closed: " + turn.actorId));
            }
            inStateLane(
                    () -> {
                        pendingAdmissions
                                .computeIfAbsent(turn.actorId, ignored -> new HashSet<>())
                                .add(admission);
                        return null;
                    });
        }
        return enqueueAfterAdmission(
                turn,
                admission,
                accepted ->
                        turn.target.executeActor(
                                turn.actorId, payloadBytes, turnOperation, accepted));
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
        Object admissionGate = admissionGate(turn.actorId);
        Supplier<CompletionStage<Void>> turnOperation =
                () -> {
                    inStateLane(
                            () -> {
                                activeActorIds.add(turn.actorId);
                                return null;
                            });
                    return runTurn(turn.actorId, operation)
                            .whenComplete(
                                    (ignored, error) -> {
                                        inStateLane(
                                                () -> {
                                                    activeActorIds.remove(turn.actorId);
                                                    return null;
                                                });
                                    });
                };
        CompletableFuture<Void> admission = new CompletableFuture<>();
        synchronized (admissionGate) {
            boolean closed = inStateLane(() -> teardowns.containsKey(turn.actorId));
            if (closed) {
                return CompletableFuture.failedFuture(
                        new IllegalStateException(
                                "actor dispatch admission is closed: " + turn.actorId));
            }
            //  Only the first part of a multi-part message carries an
            //  accepted-journal record; the remaining parts are handed an
            //  empty array (ZLinkJavaRawSpotNode). An empty record has nothing
            //  to replay, and enqueuing it as relocatable writes a zero-length
            //  entry into the relocation envelope - the reader takes the first
            //  byte of every journal record as its `kind`, so the empty entry
            //  makes it read the following field and reject the envelope.
            inStateLane(
                    () -> {
                        pendingAdmissions
                                .computeIfAbsent(turn.actorId, ignored -> new HashSet<>())
                                .add(admission);
                        return null;
                    });
        }
        return enqueueAfterAdmission(
                turn,
                admission,
                accepted ->
                        acceptedJournalRecord == null || acceptedJournalRecord.length == 0
                                ? turn.target.executeActor(turn.actorId, turnOperation, accepted)
                                : turn.target.executeActor(
                                        turn.actorId,
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
        Object admissionGate = admissionGate(turn.actorId);
        Supplier<CompletionStage<Void>> turnOperation =
                () -> {
                    inStateLane(
                            () -> {
                                activeActorIds.add(turn.actorId);
                                return null;
                            });
                    return runTurn(turn.actorId, operation)
                            .whenComplete(
                                    (ignored, error) -> {
                                        inStateLane(
                                                () -> {
                                                    activeActorIds.remove(turn.actorId);
                                                    return null;
                                                });
                                    });
                };
        CompletableFuture<Void> admission = new CompletableFuture<>();
        synchronized (admissionGate) {
            boolean closed = inStateLane(() -> teardowns.containsKey(turn.actorId));
            if (closed) {
                return CompletableFuture.failedFuture(
                        new IllegalStateException(
                                "actor dispatch admission is closed: " + turn.actorId));
            }
            inStateLane(
                    () -> {
                        pendingAdmissions
                                .computeIfAbsent(turn.actorId, ignored -> new HashSet<>())
                                .add(admission);
                        return null;
                    });
        }
        return enqueueAfterAdmission(
                turn,
                admission,
                accepted ->
                        turn.target.executeActorLazyRecord(
                                turn.actorId,
                                acceptedJournalRecord,
                                acceptedJournalRecordSizeHint,
                                turnOperation,
                                relocationRelease,
                                accepted));
    }

    CompletionStage<Void> beginTeardown(String actorId, Supplier<CompletionStage<Void>> cleanup) {
        TeardownSetup setup;
        Object admissionGate = admissionGate(actorId);
        synchronized (admissionGate) {
            setup =
                    inStateLane(
                            () -> {
                                CompletionStage<Void> existing = teardowns.get(actorId);
                                if (existing == null) {
                                    //  Teardown ends the Actor's dispatch registration, so a
                                    //  hold that a re-target installed must not keep the
                                    //  cleanup turn behind a barrier that no longer matters.
                                    releaseLifecycleBarrierHold(actorId);
                                    CompletableFuture<Void> createdTerminal =
                                            new CompletableFuture<>();
                                    teardowns.put(actorId, createdTerminal);
                                    ZLinkActorDispatchTarget target = actorTargets.get(actorId);
                                    return new TeardownSetup(
                                            createdTerminal,
                                            target == null ? target(actorId) : target,
                                            createdTerminal,
                                            List.copyOf(
                                                    pendingAdmissions.getOrDefault(
                                                            actorId, Set.of())),
                                            true);
                                }
                                return new TeardownSetup(existing, null, null, List.of(), false);
                            });
        }
        if (setup.created()) {
            startTeardown(actorId, setup, cleanup);
        }
        // Waiting for a barrier queued behind the current turn would make the
        // current Actor handler wait for itself. Cleanup still runs next, but
        // the initiating turn may complete normally.
        return isCurrent(actorId) ? CompletableFuture.completedFuture(null) : setup.teardown();
    }

    CompletionStage<Void> enqueueBarrier(
            String actorId, Supplier<CompletionStage<Void>> operation) {
        ZLinkActorDispatchTarget target = trackedTarget(actorId);
        //  The hold that a re-target installs in front of the new queue
        //  releases on the barrier's terminal, success or failure alike.
        CompletableFuture<Void> released = new CompletableFuture<>();
        inStateLane(() -> lifecycleBarriers.put(actorId, released));
        CompletableFuture<Void> admitted = new CompletableFuture<>();
        CompletionStage<Void> barrier =
                target.executeActorLifecycleNext(
                        actorId, () -> runTurn(actorId, operation), admitted);
        CompletionStage<Void> completed = admitted.thenCompose(ignored -> barrier);
        completed.whenComplete(
                (ignored, error) -> {
                    stateLane.runAsync(() -> lifecycleBarriers.remove(actorId, released));
                    released.complete(null);
                });
        return completed;
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

    CompletionStage<Boolean> abortAsync(
            String actorId, ZLinkSerialExecutionQueue.RelocationSeal seal) {
        return trackedTargetAsync(actorId)
                .thenCompose(owner -> owner.abortActorRelocationAsync(actorId, seal));
    }

    Optional<List<ZLinkSerialExecutionQueue.QueuedRecord>> commit(
            String actorId, ZLinkSerialExecutionQueue.RelocationSeal seal) {
        return trackedTarget(actorId).commitActorRelocation(actorId, seal);
    }

    Optional<ZLinkRetainedSerialQueueCommit.Commit> retainCommit(
            String actorId, ZLinkSerialExecutionQueue.RelocationSeal seal) {
        return trackedTarget(actorId).retainActorRelocationCommit(actorId, seal);
    }

    CompletionStage<Optional<ZLinkRetainedSerialQueueCommit.Commit>> retainCommitAsync(
            String actorId, ZLinkSerialExecutionQueue.RelocationSeal seal) {
        return trackedTargetAsync(actorId)
                .thenCompose(owner -> owner.retainActorRelocationCommitAsync(actorId, seal));
    }

    private CompletionStage<ZLinkActorDispatchTarget> trackedTargetAsync(String actorId) {
        return stateLane
                .runNowOrQueue(() -> actorTargets.get(actorId))
                .thenCompose(
                        tracked -> {
                            if (tracked != null) {
                                return CompletableFuture.completedFuture(tracked);
                            }
                            ZLinkActorDispatchTarget resolved = target(actorId);
                            return stateLane.runNowOrQueue(
                                    () ->
                                            actorTargets.computeIfAbsent(
                                                    actorId, ignored -> resolved));
                        });
    }

    Optional<List<ZLinkSerialExecutionQueue.QueuedRecord>> freezeIngress(
            String actorId, ZLinkSerialExecutionQueue.RelocationSeal seal) {
        return trackedTarget(actorId).freezeActorRelocationIngress(actorId, seal);
    }

    CompletionStage<Void> awaitQuiescence() {
        Map.Entry<Map<String, ZLinkActorDispatchTarget>, List<CompletionStage<Void>>> snapshot =
                inStateLane(
                        () -> Map.entry(Map.copyOf(actorTargets), List.copyOf(teardowns.values())));
        CompletableFuture<?>[] barriers =
                Stream.concat(
                                snapshot.getKey().entrySet().stream()
                                        .map(
                                                entry ->
                                                        entry.getValue()
                                                                .awaitActorQuiescence(
                                                                        entry.getKey())),
                                snapshot.getValue().stream())
                        .map(CompletionStage::toCompletableFuture)
                        .toArray(CompletableFuture[]::new);
        return CompletableFuture.allOf(barriers);
    }

    <T> CompletionStage<T> runTurn(String actorId, Supplier<CompletionStage<T>> operation) {
        try (systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext.Scope
                        ignored =
                                systems.zlink.framework.runtime.internal.handlers
                                        .ZLinkSuspendInvocationContext.enterActorDispatch(actorId);
                ZLinkDeferredActorJoinScope.Scope joins =
                        ZLinkDeferredActorJoinScope.enter(
                                runtimeScope,
                                Objects.requireNonNull(
                                        incarnationResolver.apply(actorId), "actor incarnation"),
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
        admission.whenComplete((ignored, error) -> removeAdmission(turn.actorId, admission));
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

    private void removeAdmission(String actorId, CompletableFuture<Void> admission) {
        stateLane.runNowOrQueue(
                () -> {
                    Set<CompletableFuture<Void>> admissions = pendingAdmissions.get(actorId);
                    if (admissions != null) {
                        admissions.remove(admission);
                        if (admissions.isEmpty()) {
                            pendingAdmissions.remove(actorId);
                        }
                    }
                    return null;
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
                            } catch (RuntimeException failure) {
                                completeTeardown(actorId, setup, failure);
                            }
                        });
    }

    private void completeTeardown(String actorId, TeardownSetup setup, Throwable error) {
        CompletionStage<Void> removed;
        try {
            removed =
                    error == null
                            ? setup.target().removeActorQueueAsync(actorId)
                            : CompletableFuture.completedFuture(null);
        } catch (RuntimeException | Error failure) {
            removed = CompletableFuture.failedFuture(failure);
        }
        removed.whenComplete(
                (ignored, removalFailure) -> {
                    CompletionStage<Void> registration;
                    try {
                        registration =
                                stateLane.runNowOrQueue(
                                        () -> {
                                            teardowns.remove(actorId, setup.terminal());
                                            if (error == null && removalFailure == null) {
                                                actorTargets.remove(actorId, setup.target());
                                                activeActorIds.remove(actorId);
                                            }
                                            return null;
                                        });
                    } catch (RuntimeException | Error failure) {
                        registration = CompletableFuture.failedFuture(failure);
                    }
                    registration.whenComplete(
                            (nothing, registrationFailure) -> {
                                Throwable terminalFailure = error == null ? removalFailure : error;
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
            CompletionStage<Void> barrierAdmission) {}

    private record TeardownSetup(
            CompletionStage<Void> teardown,
            ZLinkActorDispatchTarget target,
            CompletableFuture<Void> terminal,
            List<CompletableFuture<Void>> pendingAdmissions,
            boolean created) {}

    private record RemovalClaim(ZLinkActorDispatchTarget target, CompletionStage<Void> existing) {}
}
