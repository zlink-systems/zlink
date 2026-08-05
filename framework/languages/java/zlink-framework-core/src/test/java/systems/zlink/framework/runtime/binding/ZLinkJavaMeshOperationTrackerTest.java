package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.List;
import java.util.concurrent.CompletionException;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.runtime.internal.binding.spot.OperationId;
import systems.zlink.framework.runtime.internal.binding.spot.OperationKind;
import systems.zlink.framework.runtime.internal.binding.spot.OwnerKind;
import systems.zlink.framework.runtime.internal.binding.spot.ReadyRecord;
import systems.zlink.framework.runtime.internal.binding.spot.ReceiveRecord;
import systems.zlink.framework.runtime.internal.binding.spot.RecordKind;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshDispatchRecord;

class ZLinkJavaMeshOperationTrackerTest {
    @Test
    void correlatesCompletionBeforeOrAfterTrackWithoutLosingWakeup() {
        OperationId first = new OperationId(1L, 1L);
        OperationId second = new OperationId(2L, 2L);
        try (ZLinkJavaMeshOperationTracker tracker = new ZLinkJavaMeshOperationTracker()) {
            var pending = tracker.track(first);
            tracker.accept(completion(first, RequestResult.OK));
            assertDoesNotThrow(() -> pending.toCompletableFuture().join());

            tracker.accept(completion(second, RequestResult.OK));
            assertDoesNotThrow(() -> tracker.track(second).toCompletableFuture().join());
        }
    }

    @Test
    void mapsTerminalFailureToExceptionalCompletion() {
        OperationId operation = new OperationId(3L, 3L);
        try (ZLinkJavaMeshOperationTracker tracker = new ZLinkJavaMeshOperationTracker()) {
            var pending = tracker.track(operation);
            tracker.accept(completion(operation, RequestResult.TIMED_OUT));
            assertThrows(
                CompletionException.class,
                () -> pending.toCompletableFuture().join());
        }
    }

    @Test
    void correlatesNonStreamCompletionsForFormalMeshOperations() {
        OperationId operation = new OperationId(4L, 4L);
        try (ZLinkJavaMeshOperationTracker tracker = new ZLinkJavaMeshOperationTracker();
             ZLinkMeshDispatchRecord record =
                 completion(operation, RequestResult.OK, OperationKind.ACTOR_REQUEST)) {
            var pending = tracker.trackCompletion(operation);
            assertTrue(tracker.accept(record));
            assertDoesNotThrow(() -> pending.toCompletableFuture().join());
        }
    }

    private static ZLinkMeshDispatchRecord completion(
        OperationId operation,
        RequestResult result) {
        return completion(operation, result, OperationKind.STREAM_BIND);
    }

    private static ZLinkMeshDispatchRecord completion(
        OperationId operation,
        RequestResult result,
        OperationKind kind) {
        return new ZLinkMeshDispatchRecord(
            new ReadyRecord(OwnerKind.NODE, 2, null, null),
            new ReceiveRecord(
                RecordKind.COMPLETION,
                2,
                null,
                null,
                null,
                operation,
                kind,
                null,
                null,
                null,
                null,
                result.value(),
                0,
                0,
                0),
            List.of());
    }
}
