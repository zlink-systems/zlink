package systems.zlink.framework.runtime.actors;

import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.List;
import java.util.Optional;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executor;
import java.util.function.Supplier;
import java.util.function.Function;
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue;

final class ZLinkActorDispatchSerials {
    private static final boolean STREAM_TRACE =
        "1".equals(System.getenv("ZLINK_JAVA_STREAM_TRACE"));
    private static final java.util.logging.Logger LOGGER =
        java.util.logging.Logger.getLogger(ZLinkActorDispatchSerials.class.getName());
    private final Object runtimeScope;
    private final Function<String, Object> incarnationResolver;
    private final Executor executor;
    private final Map<String, ZLinkAsyncSerialQueue> queues = new HashMap<>();
    private final Set<String> activeActorIds = new HashSet<>();
    private final Map<String, CompletionStage<Void>> teardowns = new HashMap<>();
    private final Map<String, Object> admissionGates = new HashMap<>();

    ZLinkActorDispatchSerials() {
        this(
            ZLinkDeferredActorJoinScope.legacyRuntimeScope(),
            actorId -> actorId,
            null);
    }

    ZLinkActorDispatchSerials(
        Object runtimeScope,
        Function<String, Object> incarnationResolver) {
        this(runtimeScope, incarnationResolver, null);
    }

    ZLinkActorDispatchSerials(
        Object runtimeScope,
        Function<String, Object> incarnationResolver,
        Executor executor) {
        this.runtimeScope = java.util.Objects.requireNonNull(
            runtimeScope, "runtimeScope");
        this.incarnationResolver = java.util.Objects.requireNonNull(
            incarnationResolver, "incarnationResolver");
        this.executor = executor;
    }

    private ZLinkAsyncSerialQueue newQueue() {
        return executor == null
            ? new ZLinkAsyncSerialQueue(false)
            : new ZLinkAsyncSerialQueue(executor, false);
    }

    private synchronized Object admissionGate(String actorId) {
        return admissionGates.computeIfAbsent(
            actorId,
            ignored -> new Object());
    }

    boolean isCurrent(String actorId) {
        return actorId.equals(systems.zlink.framework.runtime.internal.handlers
            .ZLinkSuspendInvocationContext.currentActorDispatch());
    }

    synchronized boolean isActive(String actorId) {
        return activeActorIds.contains(actorId);
    }

    synchronized QueuedTurn prepare(String actorId) {
        if (teardowns.containsKey(actorId)) {
            throw new IllegalStateException(
                "actor dispatch admission is closed: " + actorId);
        }
        return new QueuedTurn(
            actorId,
            queues.computeIfAbsent(actorId, ignored -> newQueue()));
    }

    synchronized ZLinkAsyncSerialQueue relocationLane(String actorId) {
        return queues.computeIfAbsent(
            actorId, ignored -> newQueue());
    }

    synchronized void remove(String actorId) {
        queues.remove(actorId);
        activeActorIds.remove(actorId);
        teardowns.remove(actorId);
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
            synchronized (this) {
                activeActorIds.add(turn.actorId);
            }
            streamTrace("turn-start actor=" + turn.actorId);
            return runTurn(turn.actorId, operation)
                .whenComplete((ignored, error) -> {
                    streamTrace("turn-complete actor=" + turn.actorId
                        + " error=" + (error == null ? "none" : error));
                    synchronized (this) {
                        activeActorIds.remove(turn.actorId);
                    }
                });
        };
        synchronized (admissionGate) {
            synchronized (this) {
                if (teardowns.containsKey(turn.actorId)) {
                    return CompletableFuture.failedFuture(
                        new IllegalStateException(
                            "actor dispatch admission is closed: "
                                + turn.actorId));
                }
            }
            return turn.queue.enqueueWithPayloadBytes(
                payloadBytes,
                turnOperation);
        }
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
            synchronized (this) {
                activeActorIds.add(turn.actorId);
            }
            streamTrace("turn-start actor=" + turn.actorId);
            return runTurn(turn.actorId, operation)
                .whenComplete((ignored, error) -> {
                    streamTrace("turn-complete actor=" + turn.actorId
                        + " error=" + (error == null ? "none" : error));
                    synchronized (this) {
                        activeActorIds.remove(turn.actorId);
                    }
                });
        };
        synchronized (admissionGate) {
            synchronized (this) {
                if (teardowns.containsKey(turn.actorId)) {
                    return CompletableFuture.failedFuture(
                        new IllegalStateException(
                            "actor dispatch admission is closed: "
                                + turn.actorId));
                }
            }
            return acceptedJournalRecord == null
                ? turn.queue.enqueue(turnOperation)
                : turn.queue.enqueueRelocatable(
                    acceptedJournalRecord,
                    turnOperation,
                    relocationRelease);
        }
    }

    CompletionStage<Void> enqueueLazyRecord(
        QueuedTurn turn,
        Supplier<byte[]> acceptedJournalRecord,
        long acceptedJournalRecordSizeHint,
        Supplier<CompletionStage<Void>> operation,
        Runnable relocationRelease) {
        Object admissionGate = admissionGate(turn.actorId);
        Supplier<CompletionStage<Void>> turnOperation = () -> {
            synchronized (this) {
                activeActorIds.add(turn.actorId);
            }
            return runTurn(turn.actorId, operation).whenComplete(
                (ignored, error) -> {
                    synchronized (this) {
                        activeActorIds.remove(turn.actorId);
                    }
                });
        };
        synchronized (admissionGate) {
            synchronized (this) {
                if (teardowns.containsKey(turn.actorId)) {
                    return CompletableFuture.failedFuture(
                        new IllegalStateException(
                            "actor dispatch admission is closed: "
                                + turn.actorId));
                }
            }
            return turn.queue.enqueueRelocatableLazyRecord(
                acceptedJournalRecord,
                acceptedJournalRecordSizeHint,
                turnOperation,
                relocationRelease);
        }
    }

    CompletionStage<Void> beginTeardown(
        String actorId,
        Supplier<CompletionStage<Void>> cleanup) {
        CompletionStage<Void> teardown;
        ZLinkAsyncSerialQueue queue = null;
        CompletableFuture<Void> terminal = null;
        boolean created = false;
        Object admissionGate = admissionGate(actorId);
        synchronized (admissionGate) {
            synchronized (this) {
                teardown = teardowns.get(actorId);
                if (teardown == null) {
                    queue = queues.computeIfAbsent(
                        actorId,
                        ignored -> newQueue());
                    terminal = new CompletableFuture<>();
                    teardowns.put(actorId, terminal);
                    teardown = terminal;
                    created = true;
                }
            }
            if (created) {
                ZLinkAsyncSerialQueue teardownQueue = queue;
                CompletableFuture<Void> teardownTerminal = terminal;
                try {
                    teardownQueue.enqueueLifecycleBarrier(cleanup)
                        .whenComplete((ignored, error) -> {
                            synchronized (this) {
                                teardowns.remove(actorId, teardownTerminal);
                                if (error == null) {
                                    queues.remove(actorId, teardownQueue);
                                    activeActorIds.remove(actorId);
                                }
                            }
                            if (error == null) {
                                teardownTerminal.complete(null);
                            } else {
                                teardownTerminal.completeExceptionally(error);
                            }
                        });
                } catch (RuntimeException failure) {
                    synchronized (this) {
                        teardowns.remove(actorId, teardownTerminal);
                    }
                    teardownTerminal.completeExceptionally(failure);
                }
            }
        }
        // Waiting for a barrier queued behind the current turn would make the
        // current Actor handler wait for itself. Cleanup still runs next, but
        // the initiating turn may complete normally.
        return isCurrent(actorId)
            ? CompletableFuture.completedFuture(null)
            : teardown;
    }

    CompletionStage<Void> enqueueBarrier(
        String actorId,
        Supplier<CompletionStage<Void>> operation) {
        ZLinkAsyncSerialQueue queue;
        synchronized (this) {
            queue = queues.computeIfAbsent(
                actorId,
                ignored -> newQueue());
        }
        return queue.enqueueBarrierNext(() -> runTurn(actorId, operation));
    }

    Optional<ZLinkAsyncSerialQueue.RelocationSeal> trySeal(
        String actorId) {
        ZLinkAsyncSerialQueue queue = relocationLane(actorId);
        return queue.trySealRelocation();
    }

    boolean abort(
        String actorId,
        ZLinkAsyncSerialQueue.RelocationSeal seal) {
        ZLinkAsyncSerialQueue queue;
        synchronized (this) {
            queue = queues.get(actorId);
        }
        return queue != null && queue.abortRelocation(seal);
    }

    Optional<List<ZLinkAsyncSerialQueue.QueuedRecord>> commit(
        String actorId,
        ZLinkAsyncSerialQueue.RelocationSeal seal) {
        ZLinkAsyncSerialQueue queue;
        synchronized (this) {
            queue = queues.get(actorId);
        }
        return queue == null
            ? Optional.empty()
            : queue.commitRelocation(seal);
    }

    Optional<List<ZLinkAsyncSerialQueue.QueuedRecord>>
        freezeIngress(
            String actorId,
            ZLinkAsyncSerialQueue.RelocationSeal seal) {
        ZLinkAsyncSerialQueue queue;
        synchronized (this) {
            queue = queues.get(actorId);
        }
        return queue == null
            ? Optional.empty()
            : queue.freezeRelocationIngress(seal);
    }

    CompletionStage<Void> awaitQuiescence() {
        List<ZLinkAsyncSerialQueue> snapshot;
        synchronized (this) {
            snapshot = List.copyOf(queues.values());
        }
        CompletableFuture<?>[] barriers = snapshot.stream()
            .map(ZLinkAsyncSerialQueue::awaitQuiescence)
            .map(CompletionStage::toCompletableFuture)
            .toArray(CompletableFuture[]::new);
        return CompletableFuture.allOf(barriers);
    }

    <T> CompletionStage<T> runTurn(
        String actorId,
        Supplier<CompletionStage<T>> operation) {
        streamTrace("run-turn-enter actor=" + actorId);
        try (systems.zlink.framework.runtime.internal.handlers
                 .ZLinkSuspendInvocationContext.Scope ignored =
                 systems.zlink.framework.runtime.internal.handlers
                     .ZLinkSuspendInvocationContext.enterActorDispatch(actorId);
             ZLinkDeferredActorJoinScope.Scope joins =
                 ZLinkDeferredActorJoinScope.enter(
                     runtimeScope,
                     java.util.Objects.requireNonNull(
                         incarnationResolver.apply(actorId),
                         "actor incarnation"),
                     actorId)) {
            CompletionStage<T> handler = operation.get();
            streamTrace("run-turn-operation-return actor=" + actorId
                + " done=" + handler.toCompletableFuture().isDone());
            CompletableFuture<T> completed = new CompletableFuture<>();
            joins.finish(handler, null).whenComplete((nothing, error) -> {
                streamTrace("run-turn-join-finish actor=" + actorId
                    + " error=" + (error == null ? "none" : error));
                if (error != null) {
                    completed.completeExceptionally(error);
                    return;
                }
                handler.whenComplete((value, handlerError) -> {
                    streamTrace("run-turn-handler-finish actor=" + actorId
                        + " error=" + (handlerError == null
                            ? "none" : handlerError));
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

    record QueuedTurn(String actorId, ZLinkAsyncSerialQueue queue) {
    }
}
