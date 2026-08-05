package systems.zlink.framework.runtime.binding;

import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.framework.runtime.internal.binding.spot.OperationId;
import systems.zlink.framework.runtime.internal.binding.spot.RecordKind;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshDispatchRecord;

/** Correlates pull-dispatch completion records with submitted mesh operations. */
final class ZLinkJavaMeshOperationTracker implements AutoCloseable {
    private static final int MAX_EARLY_COMPLETIONS = 4096;
    private final Map<OperationId, CompletableFuture<ZLinkMeshDispatchRecord>> pending =
        new HashMap<>();
    private final Map<OperationId, ZLinkMeshDispatchRecord> early = new HashMap<>();
    private boolean closed;

    synchronized CompletionStage<Void> track(OperationId operationId) {
        return trackCompletion(operationId).thenAccept(record -> {
            Terminal.requireSuccess(record);
            record.close();
        });
    }

    synchronized CompletionStage<ZLinkMeshDispatchRecord> trackCompletion(
        OperationId operationId) {
        if (closed) {
            return CompletableFuture.failedFuture(
                new IllegalStateException("MeshNode operation tracker is closed"));
        }
        ZLinkMeshDispatchRecord completed = early.remove(operationId);
        if (completed != null) {
            return CompletableFuture.completedFuture(completed);
        }
        CompletableFuture<ZLinkMeshDispatchRecord> future = new CompletableFuture<>();
        if (pending.putIfAbsent(operationId, future) != null) {
            throw new IllegalStateException("duplicate MeshNode operation: " + operationId);
        }
        return future;
    }

    synchronized boolean accept(ZLinkMeshDispatchRecord record) {
        if (record.receive().kind() != RecordKind.COMPLETION
            || record.receive().operationId() == null) {
            return false;
        }
        OperationId operationId = record.receive().operationId();
        CompletableFuture<ZLinkMeshDispatchRecord> future = pending.remove(operationId);
        if (future == null) {
            if (early.size() >= MAX_EARLY_COMPLETIONS) {
                record.close();
            } else {
                ZLinkMeshDispatchRecord replaced = early.put(operationId, record);
                if (replaced != null) {
                    replaced.close();
                }
            }
        } else {
            future.complete(record);
        }
        return true;
    }

    @Override
    public synchronized void close() {
        if (closed) {
            return;
        }
        closed = true;
        IllegalStateException failure =
            new IllegalStateException("MeshNode operation tracker closed");
        pending.values().forEach(future -> future.completeExceptionally(failure));
        pending.clear();
        early.values().forEach(ZLinkMeshDispatchRecord::close);
        early.clear();
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
