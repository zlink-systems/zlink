package systems.zlink.framework.runtime.binding;

import java.util.HashMap;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executor;
import java.util.concurrent.ForkJoinPool;
import java.util.concurrent.RejectedExecutionException;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.framework.runtime.internal.binding.spot.OperationId;
import systems.zlink.framework.runtime.internal.binding.spot.RecordKind;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshDispatchRecord;

/** Correlates pull-dispatch completion records with submitted mesh operations. */
final class ZLinkJavaMeshOperationTracker implements AutoCloseable {
    private static final int MAX_PENDING_OPERATIONS = 4096;
    private static final int MAX_EARLY_COMPLETIONS = 4096;
    private final Object gate = new Object();
    private final Executor completionExecutor;
    private final Map<OperationId, CompletableFuture<ZLinkMeshDispatchRecord>> pending =
        new HashMap<>();
    private final Map<OperationId, ZLinkMeshDispatchRecord> early = new HashMap<>();
    private boolean closed;

    ZLinkJavaMeshOperationTracker() {
        this(ForkJoinPool.commonPool());
    }

    ZLinkJavaMeshOperationTracker(Executor completionExecutor) {
        this.completionExecutor = Objects.requireNonNull(
            completionExecutor, "completionExecutor");
    }

    CompletionStage<Void> track(OperationId operationId) {
        return trackCompletion(operationId).thenAccept(record -> {
            try {
                Terminal.requireSuccess(record);
            } finally {
                record.close();
            }
        });
    }

    CompletionStage<ZLinkMeshDispatchRecord> trackCompletion(
        OperationId operationId) {
        Objects.requireNonNull(operationId, "operationId");
        if (operationId.high() == 0 && operationId.low() == 0) {
            throw new IllegalArgumentException("MeshNode operation id must not be zero");
        }
        CompletableFuture<ZLinkMeshDispatchRecord> future = new CompletableFuture<>();
        ZLinkMeshDispatchRecord completed;
        synchronized (gate) {
            if (closed) {
                return CompletableFuture.failedFuture(
                    new IllegalStateException("MeshNode operation tracker is closed"));
            }
            completed = early.remove(operationId);
            if (completed == null) {
                if (pending.size() >= MAX_PENDING_OPERATIONS) {
                    return CompletableFuture.failedFuture(
                        new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.CAPACITY_EXCEEDED,
                            "MeshNode operation capacity is exhausted"));
                }
                if (pending.putIfAbsent(operationId, future) != null) {
                    throw new IllegalStateException(
                        "duplicate MeshNode operation: " + operationId);
                }
            }
        }
        if (completed != null) {
            ZLinkMeshDispatchRecord terminal = completed;
            dispatch(() -> {
                if (!future.complete(terminal)) {
                    terminal.close();
                }
            });
        } else {
            future.whenComplete((ignored, failure) -> {
                if (future.isCancelled()) {
                    synchronized (gate) {
                        pending.remove(operationId, future);
                    }
                }
            });
        }
        return future;
    }

    boolean accept(ZLinkMeshDispatchRecord record) {
        if (record.receive().kind() != RecordKind.COMPLETION
            || record.receive().operationId() == null) {
            return false;
        }
        OperationId operationId = record.receive().operationId();
        CompletableFuture<ZLinkMeshDispatchRecord> future;
        boolean closeRecord = false;
        synchronized (gate) {
            if (closed) {
                future = null;
                closeRecord = true;
            } else {
                future = pending.remove(operationId);
                if (future == null) {
                    if (early.size() >= MAX_EARLY_COMPLETIONS
                        || early.putIfAbsent(operationId, record) != null) {
                        closeRecord = true;
                    }
                }
            }
        }
        if (future != null) {
            CompletableFuture<ZLinkMeshDispatchRecord> terminal = future;
            dispatch(() -> {
                if (!terminal.complete(record)) {
                    record.close();
                }
            });
        } else if (closeRecord) {
            record.close();
        }
        return true;
    }

    @Override
    public void close() {
        List<CompletableFuture<ZLinkMeshDispatchRecord>> detachedPending;
        List<ZLinkMeshDispatchRecord> detachedEarly;
        synchronized (gate) {
            if (closed) {
                return;
            }
            closed = true;
            detachedPending = new ArrayList<>(pending.values());
            pending.clear();
            detachedEarly = new ArrayList<>(early.values());
            early.clear();
        }
        IllegalStateException failure =
            new IllegalStateException("MeshNode operation tracker closed");
        detachedPending.forEach(future ->
            dispatch(() -> future.completeExceptionally(failure)));
        detachedEarly.forEach(ZLinkMeshDispatchRecord::close);
    }

    private void dispatch(Runnable completion) {
        try {
            completionExecutor.execute(completion);
        } catch (RejectedExecutionException rejected) {
            CompletableFuture.runAsync(completion);
        }
    }

    static final class Terminal {
        private Terminal() {
        }

        static void requireSuccess(ZLinkMeshDispatchRecord record) {
            int result = record.receive().terminalResult();
            if (result != RequestResult.OK.value()) {
                throw new ZlinkRequestException(
                    RequestResult.fromValue(result),
                    record.receive().failureErrno());
            }
        }
    }
}
