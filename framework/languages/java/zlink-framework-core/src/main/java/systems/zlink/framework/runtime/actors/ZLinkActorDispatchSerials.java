package systems.zlink.framework.runtime.actors;
import java.util.Objects;
import java.util.logging.Logger;

import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.List;
import java.util.Optional;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executor;
import java.util.function.Supplier;
import java.util.function.Function;
import systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode;
import systems.zlink.framework.execution.ZLinkExecutionLanePolicy;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.relocation
    .ZLinkRetainedSerialQueueCommit;
import systems.zlink.framework.runtime.spots.ZLinkSpotSerialExecutor;

final class ZLinkActorDispatchSerials {
    private static final boolean STREAM_TRACE =
        "1".equals(System.getenv("ZLINK_JAVA_STREAM_TRACE"));
    private static final Logger LOGGER =
        Logger.getLogger(ZLinkActorDispatchSerials.class.getName());
    private final Object runtimeScope;
    private final Function<String, Object> incarnationResolver;
    private final ZLinkActorDispatchTarget legacyTarget;
    private final Function<String, ZLinkActorDispatchTarget> targetResolver;
    private final Set<String> activeActorIds = new HashSet<>();
    private final Set<String> knownActorIds = new HashSet<>();
    private final Map<String, CompletionStage<Void>> teardowns = new HashMap<>();
    private final Map<String, Object> admissionGates = new HashMap<>();
    // An accepted packet claims admission while its queue entry is installed
    // outside the admission monitor.  Teardown waits for those claims before
    // it puts its lifecycle barrier on the queue, preserving the monitor-era
    // admission ordering without invoking the queue under that monitor.
    private final Map<String, Set<CompletableFuture<Void>>> pendingAdmissions =
        new HashMap<>();
    private final ZLinkStateLane stateLane = new ZLinkStateLane();

    ZLinkActorDispatchSerials() {
        this(
            ZLinkDeferredActorJoinScope.legacyRuntimeScope(),
            actorId -> actorId,
            null,
            null);
    }

    ZLinkActorDispatchSerials(
        Object runtimeScope,
        Function<String, Object> incarnationResolver) {
        this(runtimeScope, incarnationResolver, null, null);
    }

    ZLinkActorDispatchSerials(
        Object runtimeScope,
        Function<String, Object> incarnationResolver,
        Executor executor) {
        this(
            runtimeScope,
            incarnationResolver,
            executor,
            null);
    }

    ZLinkActorDispatchSerials(
        Object runtimeScope,
        Function<String, Object> incarnationResolver,
        Executor executor,
        Function<String, ZLinkActorDispatchTarget> targetResolver) {
        this.runtimeScope = Objects.requireNonNull(
            runtimeScope, "runtimeScope");
        this.incarnationResolver = Objects.requireNonNull(
            incarnationResolver, "incarnationResolver");
        this.legacyTarget = createLegacyTarget(executor);
        this.targetResolver = targetResolver == null
            ? ignored -> legacyTarget
            : targetResolver;
    }

    private static ZLinkActorDispatchTarget createLegacyTarget(Executor executor) {
        ZLinkSerialExecutionQueue queue = new ZLinkSerialExecutionQueue(
            ZLinkExecutionLanePolicy.spot());
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
        return inStateLane(() -> admissionGates.computeIfAbsent(
            actorId, ignored -> new Object()));
    }

    boolean isCurrent(String actorId) {
        return actorId.equals(systems.zlink.framework.runtime.internal.handlers
            .ZLinkSuspendInvocationContext.currentActorDispatch());
    }

    boolean isActive(String actorId) {
        return inStateLane(() -> activeActorIds.contains(actorId));
    }

    QueuedTurn prepare(String actorId) {
        return prepare(actorId, target(actorId));
    }

    QueuedTurn prepare(
        String actorId,
        ZLinkActorDispatchTarget target) {
        Objects.requireNonNull(target, "target");
        return inStateLane(() -> {
            if (teardowns.containsKey(actorId)) {
                throw new IllegalStateException(
                    "actor dispatch admission is closed: " + actorId);
            }
            knownActorIds.add(actorId);
            return new QueuedTurn(
                actorId,
                target);
        });
    }

    ZLinkSerialExecutionQueue relocationLane(String actorId) {
        return target(actorId).actorRelocationLane(actorId);
    }

    void remove(String actorId) {
        inStateLane(() -> {
            knownActorIds.remove(actorId);
            activeActorIds.remove(actorId);
            teardowns.remove(actorId);
            return null;
        });
    }

    CompletionStage<Void> enqueue(
        QueuedTurn turn,
        Supplier<CompletionStage<Void>> operation) {
        return enqueue(turn, null, operation);
    }

    CompletionStage<Void> enqueue(
        QueuedTurn turn,
        long payloadBytes,
        Supplier<CompletionStage<Void>> operation) {
        return enqueue(turn, payloadBytes, operation, () -> { });
    }

    CompletionStage<Void> enqueue(
        QueuedTurn turn,
        long payloadBytes,
        Supplier<CompletionStage<Void>> operation,
        Runnable relocationRelease) {
        Object admissionGate = admissionGate(turn.actorId);
        Supplier<CompletionStage<Void>> turnOperation = () -> {
            inStateLane(() -> {
                activeActorIds.add(turn.actorId);
                return null;
            });
            streamTrace(STREAM_TRACE ? "turn-start actor=" + turn.actorId : null);
            return runTurn(turn.actorId, operation)
                .whenComplete((ignored, error) -> {
                    streamTrace(STREAM_TRACE ? "turn-complete actor=" + turn.actorId
                        + " error=" + (error == null ? "none" : error) : null);
                    inStateLane(() -> {
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
                            "actor dispatch admission is closed: "
                                + turn.actorId));
            }
            inStateLane(() -> {
                pendingAdmissions.computeIfAbsent(turn.actorId,
                    ignored -> new HashSet<>()).add(admission);
                return null;
            });
        }
        return enqueueAfterAdmission(
            turn.actorId,
            admission,
            () -> turn.target.executeActor(
                turn.actorId, payloadBytes, turnOperation));
    }

    CompletionStage<Void> enqueue(
        QueuedTurn turn,
        byte[] acceptedJournalRecord,
        Supplier<CompletionStage<Void>> operation) {
        return enqueue(
            turn, acceptedJournalRecord, operation, () -> { });
    }

    CompletionStage<Void> enqueue(
        QueuedTurn turn,
        byte[] acceptedJournalRecord,
        Supplier<CompletionStage<Void>> operation,
        Runnable relocationRelease) {
        Object admissionGate = admissionGate(turn.actorId);
        Supplier<CompletionStage<Void>> turnOperation = () -> {
            inStateLane(() -> {
                activeActorIds.add(turn.actorId);
                return null;
            });
            streamTrace(STREAM_TRACE ? "turn-start actor=" + turn.actorId : null);
            return runTurn(turn.actorId, operation)
                .whenComplete((ignored, error) -> {
                    streamTrace(STREAM_TRACE ? "turn-complete actor=" + turn.actorId
                        + " error=" + (error == null ? "none" : error) : null);
                    inStateLane(() -> {
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
                            "actor dispatch admission is closed: "
                                + turn.actorId));
            }
            //  Only the first part of a multi-part message carries an
            //  accepted-journal record; the remaining parts are handed an
            //  empty array (ZLinkJavaRawSpotNode). An empty record has nothing
            //  to replay, and enqueuing it as relocatable writes a zero-length
            //  entry into the relocation envelope - the reader takes the first
            //  byte of every journal record as its `kind`, so the empty entry
            //  makes it read the following field and reject the envelope.
            inStateLane(() -> {
                pendingAdmissions.computeIfAbsent(turn.actorId,
                    ignored -> new HashSet<>()).add(admission);
                return null;
            });
        }
        return enqueueAfterAdmission(
            turn.actorId,
            admission,
            () -> acceptedJournalRecord == null
                || acceptedJournalRecord.length == 0
                ? turn.target.executeActor(turn.actorId, turnOperation)
                : turn.target.executeActor(
                    turn.actorId,
                    acceptedJournalRecord, turnOperation, relocationRelease));
    }

    CompletionStage<Void> enqueueLazyRecord(
        QueuedTurn turn,
        Supplier<byte[]> acceptedJournalRecord,
        long acceptedJournalRecordSizeHint,
        Supplier<CompletionStage<Void>> operation,
        Runnable relocationRelease) {
        Object admissionGate = admissionGate(turn.actorId);
        Supplier<CompletionStage<Void>> turnOperation = () -> {
            inStateLane(() -> {
                activeActorIds.add(turn.actorId);
                return null;
            });
            return runTurn(turn.actorId, operation).whenComplete(
                (ignored, error) -> {
                    inStateLane(() -> {
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
                            "actor dispatch admission is closed: "
                                + turn.actorId));
            }
            inStateLane(() -> {
                pendingAdmissions.computeIfAbsent(turn.actorId,
                    ignored -> new HashSet<>()).add(admission);
                return null;
            });
        }
        return enqueueAfterAdmission(
            turn.actorId,
            admission,
            () -> turn.target.executeActorLazyRecord(
                turn.actorId,
                acceptedJournalRecord,
                acceptedJournalRecordSizeHint,
                turnOperation,
                relocationRelease));
    }

    CompletionStage<Void> beginTeardown(
        String actorId,
        Supplier<CompletionStage<Void>> cleanup) {
        TeardownSetup setup;
        Object admissionGate = admissionGate(actorId);
        synchronized (admissionGate) {
            setup = inStateLane(() -> {
                CompletionStage<Void> existing = teardowns.get(actorId);
                if (existing == null) {
                    CompletableFuture<Void> createdTerminal = new CompletableFuture<>();
                    teardowns.put(actorId, createdTerminal);
                    return new TeardownSetup(
                        createdTerminal, target(actorId), createdTerminal,
                        List.copyOf(pendingAdmissions.getOrDefault(
                            actorId, Set.of())), true);
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
        return isCurrent(actorId)
            ? CompletableFuture.completedFuture(null)
            : setup.teardown();
    }

    CompletionStage<Void> enqueueBarrier(
        String actorId,
        Supplier<CompletionStage<Void>> operation) {
        return target(actorId).executeActorLifecycleNext(
            actorId, () -> runTurn(actorId, operation));
    }

    Optional<ZLinkSerialExecutionQueue.RelocationSeal> trySeal(
        String actorId) {
        return target(actorId).trySealActorRelocation(actorId);
    }

    boolean abort(
        String actorId,
        ZLinkSerialExecutionQueue.RelocationSeal seal) {
        return target(actorId).abortActorRelocation(actorId, seal);
    }

    Optional<List<ZLinkSerialExecutionQueue.QueuedRecord>> commit(
        String actorId,
        ZLinkSerialExecutionQueue.RelocationSeal seal) {
        return target(actorId).commitActorRelocation(actorId, seal);
    }

    Optional<ZLinkRetainedSerialQueueCommit.Commit> retainCommit(
        String actorId,
        ZLinkSerialExecutionQueue.RelocationSeal seal) {
        return target(actorId).retainActorRelocationCommit(actorId, seal);
    }

    Optional<List<ZLinkSerialExecutionQueue.QueuedRecord>>
        freezeIngress(
        String actorId,
        ZLinkSerialExecutionQueue.RelocationSeal seal) {
        return target(actorId).freezeActorRelocationIngress(actorId, seal);
    }

    CompletionStage<Void> awaitQuiescence() {
        List<String> snapshot = inStateLane(() -> List.copyOf(knownActorIds));
        CompletableFuture<?>[] barriers = snapshot.stream()
            .map(actorId -> target(actorId).awaitActorQuiescence(actorId))
            .map(CompletionStage::toCompletableFuture)
            .toArray(CompletableFuture[]::new);
        return CompletableFuture.allOf(barriers);
    }

    <T> CompletionStage<T> runTurn(
        String actorId,
        Supplier<CompletionStage<T>> operation) {
        streamTrace(STREAM_TRACE ? "run-turn-enter actor=" + actorId : null);
        try (systems.zlink.framework.runtime.internal.handlers
                 .ZLinkSuspendInvocationContext.Scope ignored =
                 systems.zlink.framework.runtime.internal.handlers
                     .ZLinkSuspendInvocationContext.enterActorDispatch(actorId);
             ZLinkDeferredActorJoinScope.Scope joins =
                 ZLinkDeferredActorJoinScope.enter(
                     runtimeScope,
                     Objects.requireNonNull(
                         incarnationResolver.apply(actorId),
                         "actor incarnation"),
                     actorId)) {
            CompletionStage<T> handler = operation.get();
            streamTrace(STREAM_TRACE ? "run-turn-operation-return actor=" + actorId
                + " done=" + handler.toCompletableFuture().isDone() : null);
            CompletableFuture<T> completed = new CompletableFuture<>();
            joins.finish(handler, null).whenComplete((nothing, error) -> {
                streamTrace(STREAM_TRACE ? "run-turn-join-finish actor=" + actorId
                    + " error=" + (error == null ? "none" : error) : null);
                if (error != null) {
                    completed.completeExceptionally(error);
                    return;
                }
                handler.whenComplete((value, handlerError) -> {
                    streamTrace(STREAM_TRACE ? "run-turn-handler-finish actor=" + actorId
                        + " error=" + (handlerError == null
                            ? "none" : handlerError) : null);
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

    private static void streamTrace(String message) {
        if (STREAM_TRACE) {
            LOGGER.warning("[zlink-java-stream-trace] actor-dispatch " + message);
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
        } catch (RuntimeException failure) {
            admission.completeExceptionally(failure);
            removeAdmission(actorId, admission);
            return CompletableFuture.failedFuture(failure);
        }
    }

    private void removeAdmission(
        String actorId,
        CompletableFuture<Void> admission) {
        inStateLane(() -> {
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
        String actorId,
        TeardownSetup setup,
        Supplier<CompletionStage<Void>> cleanup) {
        CompletableFuture<?>[] admitted = setup.pendingAdmissions().stream()
            .map(CompletableFuture::toCompletableFuture)
            .toArray(CompletableFuture[]::new);
        CompletableFuture.allOf(admitted).whenComplete((ignored, admissionError) -> {
            if (admissionError != null) {
                completeTeardown(actorId, setup, admissionError);
                return;
            }
            try {
                setup.target().executeActorLifecycle(actorId, cleanup)
                    .whenComplete((nothing, error) ->
                        completeTeardown(actorId, setup, error));
            } catch (RuntimeException failure) {
                completeTeardown(actorId, setup, failure);
            }
        });
    }

    private void completeTeardown(
        String actorId,
        TeardownSetup setup,
        Throwable error) {
        inStateLane(() -> {
            teardowns.remove(actorId, setup.terminal());
            if (error == null) {
                knownActorIds.remove(actorId);
                activeActorIds.remove(actorId);
            }
            return null;
        });
        if (error == null) {
            setup.terminal().complete(null);
        } else {
            setup.terminal().completeExceptionally(error);
        }
    }

    record QueuedTurn(String actorId, ZLinkActorDispatchTarget target) {
    }

    private record TeardownSetup(
        CompletionStage<Void> teardown,
        ZLinkActorDispatchTarget target,
        CompletableFuture<Void> terminal,
        List<CompletableFuture<Void>> pendingAdmissions,
        boolean created) {
    }
}
