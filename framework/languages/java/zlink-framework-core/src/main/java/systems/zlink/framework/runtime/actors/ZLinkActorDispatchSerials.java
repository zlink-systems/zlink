package systems.zlink.framework.runtime.actors;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.execution.ZLinkExecutionLanePolicy;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.actors.ZLinkActorDispatchTarget.QueuedTurn;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerStages;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorReceived;
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
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ExecutionException;
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
    // Retain accepted Actor turns until retirement, including turns that still
    // belong to the source Spot after membership moves to another Spot.
    private final Map<String, QueuedTurn> actorTargets = new HashMap<>();
    private final ZLinkStateLane stateLane = new ZLinkStateLane();
    private final Map<String, TeardownSetup> teardowns = new HashMap<>();
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
                                                                            ignored ->
                                                                                    initializeOnRegistry(
                                                                                            resolved))));
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

    CompletionStage<QueuedTurn> prepareAsync(
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
                                                                                        resolved))));
    }

    private QueuedTurn prepareOnRegistry(
            String actorId,
            ZLinkActorDispatchTarget.ActivationSnapshot snapshot,
            QueuedTurn resolved) {
        QueuedTurn previous = actorTargets.get(actorId);
        if (previous != null
                && ((previous.target() == snapshot.target()
                                && previous.incarnation() == snapshot.incarnation())
                        || teardowns.containsKey(actorId))) {
            return previous;
        }
        if (resolved == null) return null;
        QueuedTurn turn = initializeOnRegistry(resolved);
        actorTargets.put(actorId, turn);
        return turn;
    }

    private QueuedTurn initializeOnRegistry(QueuedTurn turn) {
        CompletionStage<Void> barrier = lifecycleBarriers.get(turn.actorId());
        if (barrier == null
                && turn.incarnation() instanceof ZLinkActorRuntime.DefaultActorContext context) {
            CompletionStage<Void> moving = context.moveCompletion();
            if (!moving.toCompletableFuture().isDone()) {
                CompletableFuture<Void> released = new CompletableFuture<>();
                retainLifecycleBarrierOnRegistry(turn.actorId(), released);
                moving.whenComplete((ignored, failure) -> released.complete(null));
                barrier = released;
            }
        }
        if (barrier != null) {
            // Both a restored activation and a retargeted activation accept payloads behind
            // their existing lifecycle completion. Admission belongs to the queue commit.
            CompletionStage<Void> lifecycle = barrier;
            turn.activation()
                    .executeLifecycleNext(
                            () -> ZLinkSerialExecutionQueue.yieldCurrent(lifecycle), null);
        }
        return turn;
    }

    private void retainLifecycleBarrierOnRegistry(
            String actorId, CompletableFuture<Void> released) {
        lifecycleBarriers.put(actorId, released);
        released.whenComplete(
                (ignored, failure) -> {
                    if (stateLane.isOnLane()) lifecycleBarriers.remove(actorId, released);
                    else onRegistry(() -> lifecycleBarriers.remove(actorId, released));
                });
    }

    CompletionStage<ZLinkSerialExecutionQueue> relocationLaneAsync(String actorId) {
        return trackedTurnAsync(actorId).thenApply(turn -> turn.activation().relocationLane());
    }

    CompletionStage<Void> removeAsync(String actorId) {
        CompletableFuture<Void> terminal = new CompletableFuture<>();
        return onRegistry(
                        () -> {
                            TeardownSetup existing = teardowns.get(actorId);
                            if (existing != null) {
                                return new RemovalClaim(null, existing.teardown(), null, null);
                            }
                            QueuedTurn tracked = actorTargets.get(actorId);
                            teardowns.put(
                                    actorId,
                                    new TeardownSetup(
                                            terminal,
                                            null,
                                            terminal,
                                            tracked == null ? null : tracked.incarnation(),
                                            null,
                                            null));
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
                                                                teardowns.computeIfPresent(
                                                                        actorId,
                                                                        (id, setup) ->
                                                                                setup.terminal()
                                                                                                == terminal
                                                                                        ? null
                                                                                        : setup);
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
        return enqueue(turn, payloadBytes, operation, relocationRelease, null, null, null);
    }

    CompletionStage<Void> enqueue(
            QueuedTurn turn,
            long payloadBytes,
            Supplier<CompletionStage<Void>> operation,
            Runnable relocationRelease,
            CompletableFuture<Void> requestAdmission,
            ZLinkBackendActorReceived header,
            Message payload) {
        Supplier<CompletionStage<Void>> turnOperation = () -> runTurn(turn, operation);
        CompletableFuture<Void> admission =
                requestAdmission == null ? new CompletableFuture<>() : requestAdmission;
        return enqueueAfterAdmission(
                admission,
                header,
                payload,
                accepted ->
                        turn.target()
                                .executeActor(
                                        turn.activation(), payloadBytes, turnOperation, accepted));
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
                admission,
                accepted ->
                        acceptedJournalRecord == null || acceptedJournalRecord.length == 0
                                ? turn.target()
                                        .executeActor(turn.activation(), 0, turnOperation, accepted)
                                : turn.target()
                                        .executeActor(
                                                turn.activation(),
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
        return enqueueLazyRecord(
                turn,
                acceptedJournalRecord,
                acceptedJournalRecordSizeHint,
                operation,
                relocationRelease,
                null,
                null,
                null);
    }

    CompletionStage<Void> enqueueLazyRecord(
            QueuedTurn turn,
            Supplier<byte[]> acceptedJournalRecord,
            long acceptedJournalRecordSizeHint,
            Supplier<CompletionStage<Void>> operation,
            Runnable relocationRelease,
            CompletableFuture<Void> requestAdmission,
            ZLinkBackendActorReceived header,
            Message payload) {
        Supplier<CompletionStage<Void>> turnOperation = () -> runTurn(turn, operation);
        CompletableFuture<Void> admission =
                requestAdmission == null ? new CompletableFuture<>() : requestAdmission;
        return enqueueAfterAdmission(
                admission,
                header,
                payload,
                accepted ->
                        turn.target()
                                .executeActorLazyRecord(
                                        turn.activation(),
                                        acceptedJournalRecord,
                                        acceptedJournalRecordSizeHint,
                                        turnOperation,
                                        relocationRelease,
                                        accepted));
    }

    CompletionStage<Void> awaitTeardown(String actorId) {
        return onRegistry(
                        () -> {
                            TeardownSetup existing = teardowns.get(actorId);
                            return existing == null ? COMPLETED : existing.teardown();
                        })
                .thenCompose(terminal -> terminal);
    }

    CompletionStage<Void> beginTeardown(String actorId, Supplier<CompletionStage<Void>> cleanup) {
        return beginTeardown(actorId, null, cleanup);
    }

    CompletionStage<Void> beginTeardown(
            String actorId, Object incarnation, Supplier<CompletionStage<Void>> cleanup) {
        boolean current = isCurrent(actorId);
        return registerTeardown(actorId, incarnation, () -> COMPLETED, cleanup)
                .thenCompose(terminal -> current ? COMPLETED : terminal);
    }

    CompletionStage<CompletionStage<Void>> registerTeardown(
            String actorId,
            Supplier<CompletionStage<Void>> beforeStart,
            Supplier<CompletionStage<Void>> cleanup) {
        return registerTeardown(actorId, null, beforeStart, cleanup);
    }

    CompletionStage<CompletionStage<Void>> registerTeardown(
            String actorId,
            Object incarnation,
            Supplier<CompletionStage<Void>> beforeStart,
            Supplier<CompletionStage<Void>> cleanup) {
        return onRegistry(
                        () -> {
                            TeardownSetup existing = teardowns.get(actorId);
                            QueuedTurn tracked = actorTargets.get(actorId);
                            Object current =
                                    existing != null
                                            ? existing.incarnation()
                                            : tracked == null ? incarnation : tracked.incarnation();
                            requireSameIncarnation(incarnation, current);
                            if (existing != null) {
                                return new TeardownSetup(
                                        existing.teardown(), null, null, current, null, null);
                            }
                            CompletableFuture<Void> terminal = new CompletableFuture<>();
                            TeardownSetup setup =
                                    new TeardownSetup(
                                            terminal,
                                            tracked == null ? null : tracked.target(),
                                            terminal,
                                            current,
                                            lifecycleBarriers.remove(actorId),
                                            tracked == null ? null : tracked.activation());
                            // Reserve the operation before consulting another registry or claiming
                            // a queue.
                            // Concurrent lifecycle requests join this terminal even while
                            // resolution waits.
                            teardowns.put(actorId, setup);
                            return setup;
                        })
                .thenCompose(
                        setup -> {
                            if (setup.terminal() == null)
                                return CompletableFuture.completedFuture(setup.teardown());
                            if (setup.barrier() != null) setup.barrier().complete(null);
                            CompletionStage<ZLinkActorDispatchTarget.ActivationSnapshot>
                                    resolution =
                                            ZLinkHandlerStages.fromStageSupplier(
                                                    () -> targetResolver.apply(actorId));
                            return resolution
                                    .thenCompose(
                                            snapshot ->
                                                    onRegistry(
                                                            () -> {
                                                                Object current =
                                                                        snapshot == null
                                                                                ? null
                                                                                : snapshot
                                                                                        .incarnation();
                                                                requireSameIncarnation(
                                                                        setup.incarnation(),
                                                                        current);
                                                                return current == null
                                                                        ? null
                                                                        : new TeardownSetup(
                                                                                setup.teardown(),
                                                                                snapshot.target()
                                                                                                == null
                                                                                        ? legacyTarget
                                                                                        : snapshot
                                                                                                .target(),
                                                                                setup.terminal(),
                                                                                current,
                                                                                setup.barrier(),
                                                                                setup.activation());
                                                            }))
                                    .thenCompose(
                                            resolved -> {
                                                if (resolved == null) {
                                                    completeTeardown(actorId, setup, null, null);
                                                    return CompletableFuture.<Void>completedFuture(
                                                            null);
                                                }
                                                CompletionStage<ZLinkActorSerialExecutor>
                                                        activation =
                                                                ZLinkHandlerStages
                                                                        .fromStageSupplier(
                                                                                () ->
                                                                                        resolved.target()
                                                                                                .claimActorQueue(
                                                                                                        actorId));
                                                return activation.thenCompose(
                                                        accepted -> {
                                                            TeardownSetup claimed =
                                                                    new TeardownSetup(
                                                                            resolved.teardown(),
                                                                            resolved.target(),
                                                                            resolved.terminal(),
                                                                            resolved.incarnation(),
                                                                            resolved.barrier(),
                                                                            accepted);
                                                            return onRegistry(
                                                                            () -> {
                                                                                actorTargets.put(
                                                                                        actorId,
                                                                                        new QueuedTurn(
                                                                                                actorId,
                                                                                                claimed
                                                                                                        .target(),
                                                                                                claimed
                                                                                                        .incarnation(),
                                                                                                accepted));
                                                                                return null;
                                                                            })
                                                                    .thenCompose(
                                                                            ignored ->
                                                                                    ZLinkHandlerStages
                                                                                            .fromStageSupplier(
                                                                                                    beforeStart))
                                                                    .handle(
                                                                            (ignored, failure) -> {
                                                                                startTeardown(
                                                                                        actorId,
                                                                                        claimed,
                                                                                        setup,
                                                                                        failure
                                                                                                        == null
                                                                                                ? cleanup
                                                                                                : () ->
                                                                                                        ZLinkHandlerStages
                                                                                                                .completeAll(
                                                                                                                        List
                                                                                                                                .of(
                                                                                                                                        () ->
                                                                                                                                                CompletableFuture
                                                                                                                                                        .failedFuture(
                                                                                                                                                                failure),
                                                                                                                                        cleanup)));
                                                                                return (Void) null;
                                                                            });
                                                        });
                                            })
                                    .handle(
                                            (ignored, failure) -> {
                                                if (failure != null)
                                                    completeTeardown(actorId, setup, null, failure);
                                                return setup.teardown();
                                            });
                        });
    }

    private static void requireSameIncarnation(Object expected, Object current) {
        if (expected != null && current != null && current != expected) {
            throw ZLinkFrameworkErrorOrigin.framework(
                    ZLinkFrameworkErrorKind.INVALID_OPERATION,
                    "Actor teardown does not match the current incarnation");
        }
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
                                            retainLifecycleBarrierOnRegistry(actorId, released);
                                            return tracked;
                                        }))
                .thenCompose(
                        turn ->
                                turn.activation()
                                        .executeLifecycleNext(() -> runTurn(turn, operation), null))
                .whenComplete(
                        (ignored, error) -> {
                            released.complete(null);
                        });
    }

    CompletionStage<Boolean> abortAsync(
            String actorId, ZLinkSerialExecutionQueue.RelocationSeal seal) {
        return trackedTurnAsync(actorId)
                .thenCompose(turn -> turn.activation().abortRelocation(seal));
    }

    CompletionStage<Optional<ZLinkRetainedSerialQueueCommit.Commit>> retainCommitAsync(
            String actorId, ZLinkSerialExecutionQueue.RelocationSeal seal) {
        return trackedTurnAsync(actorId)
                .thenCompose(turn -> turn.activation().retainRelocationCommit(seal));
    }

    CompletionStage<Void> awaitQuiescence() {
        return onRegistry(
                        () ->
                                Map.entry(
                                        Map.copyOf(actorTargets),
                                        teardowns.values().stream()
                                                .map(TeardownSetup::teardown)
                                                .toList()))
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
            CompletableFuture<Void> admission,
            Function<CompletableFuture<Void>, CompletionStage<Void>> enqueue) {
        return enqueueAfterAdmission(admission, null, null, enqueue);
    }

    private CompletionStage<Void> enqueueAfterAdmission(
            CompletableFuture<Void> admission,
            ZLinkBackendActorReceived header,
            Message payload,
            Function<CompletableFuture<Void>, CompletionStage<Void>> enqueue) {
        var ownership =
                systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext
                        .transferToQueuedJob();
        CompletionStage<Void> queued;
        try {
            try (var scope =
                    systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext
                            .enterQueued(ownership)) {
                queued = enqueue.apply(admission);
            }
        } catch (RuntimeException | Error failure) {
            queued = CompletableFuture.failedFuture(failure);
        }
        if (header != null && header.relocationRedirect() != null) {
            return queued.handle(
                            (ignored, error) -> {
                                if (ownership != null) ownership.close();
                                if (error == null) return COMPLETED;
                                Throwable cause = error;
                                while ((cause instanceof CompletionException
                                                || cause instanceof ExecutionException)
                                        && cause.getCause() != null) cause = cause.getCause();
                                if (cause
                                        instanceof
                                        ZLinkSerialExecutionQueue.RelocatedOwnerException) {
                                    // The local candidate never ran. Preserve its operation and
                                    // reply route through Message Follow (routing spec §2.5), and
                                    // complete admission only after the relay accepts the frames.
                                    try {
                                        if (header.relocationRedirect()
                                                .redirect(header.message(), payload)) {
                                            admission.complete(null);
                                            return COMPLETED;
                                        }
                                    } catch (RuntimeException redirectFailure) {
                                        cause.addSuppressed(redirectFailure);
                                    }
                                }
                                admission.completeExceptionally(cause);
                                return CompletableFuture.<Void>failedFuture(cause);
                            })
                    .thenCompose(stage -> stage);
        }
        return queued.whenComplete(
                (ignored, error) -> {
                    if (ownership != null) ownership.close();
                    if (error != null) admission.completeExceptionally(error);
                });
    }

    private void startTeardown(
            String actorId,
            TeardownSetup setup,
            TeardownSetup previous,
            Supplier<CompletionStage<Void>> cleanup) {
        try {
            var lanes =
                    Stream.of(previous.activation(), setup.activation())
                            .filter(Objects::nonNull)
                            .map(ZLinkActorSerialExecutor::relocationLane)
                            .distinct()
                            .toList();
            lanes.forEach(ZLinkSerialExecutionQueue::sealClosingAdmission);
            ZLinkHandlerStages.completeAll(
                            lanes.stream()
                                    .<Supplier<? extends CompletionStage<Void>>>map(
                                            lane ->
                                                    () ->
                                                            lane.awaitQuiescence(
                                                                    ZLinkSerialExecutionQueue
                                                                            .Quiescence
                                                                            .APPLICATION))
                                    .toList())
                    .thenCompose(ignored -> setup.activation().executeLifecycle(cleanup))
                    .whenComplete(
                            (ignored, error) -> completeTeardown(actorId, setup, previous, error));
        } catch (RuntimeException failure) {
            completeTeardown(actorId, setup, previous, failure);
        }
    }

    private void completeTeardown(
            String actorId, TeardownSetup setup, TeardownSetup previous, Throwable error) {
        var retirements =
                previous == null || previous.activation() == setup.activation()
                        ? List.of(setup)
                        : List.of(previous, setup);
        CompletionStage<Void> removed =
                ZLinkHandlerStages.completeAll(
                        retirements.stream()
                                .filter(retired -> retired.activation() != null)
                                .<Supplier<? extends CompletionStage<Void>>>map(
                                        retired ->
                                                () ->
                                                        retired.target()
                                                                .removeActorQueueAsync(
                                                                        actorId,
                                                                        retired.activation()))
                                .toList());
        removed.whenComplete(
                (ignored, removalFailure) -> {
                    CompletionStage<Void> registration;
                    try {
                        registration =
                                onRegistry(
                                        () -> {
                                            teardowns.computeIfPresent(
                                                    actorId,
                                                    (id, current) ->
                                                            current.terminal() == setup.terminal()
                                                                    ? null
                                                                    : current);
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

    private record TeardownSetup(
            CompletionStage<Void> teardown,
            ZLinkActorDispatchTarget target,
            CompletableFuture<Void> terminal,
            Object incarnation,
            CompletableFuture<Void> barrier,
            ZLinkActorSerialExecutor activation) {}

    private record RemovalClaim(
            ZLinkActorDispatchTarget target,
            CompletionStage<Void> existing,
            CompletableFuture<Void> barrier,
            ZLinkActorSerialExecutor activation) {}
}
