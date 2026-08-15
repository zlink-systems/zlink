package systems.zlink.framework.runtime.internal.backend;

import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;

class ZLinkBackendRequestResultTest {

    @Test
    void coarseTerminalUsesRemoteOwnershipClassification() {
        assertEquals(ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
            ZLinkBackendRequestResult.TIMED_OUT.toFrameworkErrorKind());
        assertEquals(ZLinkFrameworkErrorKind.NOT_FOUND,
            ZLinkBackendRequestResult.NOT_FOUND.toFrameworkErrorKind());
        assertEquals(ZLinkFrameworkErrorKind.SHUTTING_DOWN,
            ZLinkBackendRequestResult.TERMINATED.toFrameworkErrorKind());
        assertEquals(ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
            ZLinkBackendRequestResult.PROTOCOL_ERROR.toFrameworkErrorKind());
        assertEquals(ZLinkFrameworkErrorKind.REJECTED,
            ZLinkBackendRequestResult.REJECTED.toFrameworkErrorKind());
        //  A remote reply's conflict/busy is the target's owner/queue state:
        //  Unavailable, not a source-owned CapacityExceeded (spec 32:99-103).
        assertEquals(ZLinkFrameworkErrorKind.UNAVAILABLE,
            ZLinkBackendRequestResult.CONFLICT.toFrameworkErrorKind());
        assertEquals(ZLinkFrameworkErrorKind.UNAVAILABLE,
            ZLinkBackendRequestResult.BUSY.toFrameworkErrorKind());
        assertEquals(ZLinkFrameworkErrorKind.UNAVAILABLE,
            ZLinkBackendRequestResult.NOT_CONNECTED.toFrameworkErrorKind());
        assertEquals(ZLinkFrameworkErrorKind.INVALID_OPERATION,
            ZLinkBackendRequestResult.INVALID_ARGUMENT.toFrameworkErrorKind());
        assertEquals(ZLinkFrameworkErrorKind.INVALID_OPERATION,
            ZLinkBackendRequestResult.INVALID_STATE.toFrameworkErrorKind());
        assertEquals(ZLinkFrameworkErrorKind.INTERNAL_FAILURE,
            ZLinkBackendRequestResult.NOT_SUPPORTED.toFrameworkErrorKind());
        assertEquals(ZLinkFrameworkErrorKind.INTERNAL_FAILURE,
            ZLinkBackendRequestResult.INTERNAL_ERROR.toFrameworkErrorKind());
    }

    @Test
    void fineFailureCodeRefinesCoarseTerminal() {
        //  A generic Conflict terminal (107) carrying a fine framework failure
        //  code is classified precisely instead of collapsing to the coarse kind.
        assertEquals(ZLinkFrameworkErrorKind.UNAVAILABLE,
            ZLinkBackendRequestResult.CONFLICT.toFrameworkErrorKind(34)); // spotMoving
        assertEquals(ZLinkFrameworkErrorKind.UNAVAILABLE,
            ZLinkBackendRequestResult.CONFLICT.toFrameworkErrorKind(21)); // actorLocationStale
        assertEquals(ZLinkFrameworkErrorKind.INVALID_OPERATION,
            ZLinkBackendRequestResult.CONFLICT.toFrameworkErrorKind(33)); // spotGenerationStale
        assertEquals(ZLinkFrameworkErrorKind.DATA_LOST,
            ZLinkBackendRequestResult.INTERNAL_ERROR.toFrameworkErrorKind(35)); // relocationDataLost
        assertEquals(ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
            ZLinkBackendRequestResult.REJECTED.toFrameworkErrorKind(19)); // workerTimedOut
        assertEquals(ZLinkFrameworkErrorKind.INTERNAL_FAILURE,
            ZLinkBackendRequestResult.REJECTED.toFrameworkErrorKind(20)); // workerFailed
        assertEquals(ZLinkFrameworkErrorKind.UNAVAILABLE,
            ZLinkBackendRequestResult.REJECTED.toFrameworkErrorKind(18)); // workerQueueFull (remote)
        assertEquals(ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
            ZLinkBackendRequestResult.INTERNAL_ERROR.toFrameworkErrorKind(12)); // payloadDecodeFailed
        //  failureCode 0 (absent) falls back to the coarse terminal.
        assertEquals(ZLinkFrameworkErrorKind.UNAVAILABLE,
            ZLinkBackendRequestResult.CONFLICT.toFrameworkErrorKind(0));
        assertEquals(ZLinkFrameworkErrorKind.NOT_FOUND,
            ZLinkBackendRequestResult.NOT_FOUND.toFrameworkErrorKind(0));
    }
}
