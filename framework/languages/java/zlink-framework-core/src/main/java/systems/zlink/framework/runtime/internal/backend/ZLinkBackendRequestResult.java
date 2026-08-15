package systems.zlink.framework.runtime.internal.backend;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;

public enum ZLinkBackendRequestResult {
    OK,
    TIMED_OUT,
    NOT_FOUND,
    TERMINATED,
    PROTOCOL_ERROR,
    INTERNAL_ERROR,
    REJECTED,
    CONFLICT,
    BUSY,
    NOT_CONNECTED,
    INVALID_ARGUMENT,
    INVALID_STATE,
    NOT_SUPPORTED;

    /**
     * Maps a non-OK backend request terminal (decoded from a remote reply) to
     * the public framework error kind. Mirrors the authoritative C++
     * request_failure_mapper reply_header_exception coarse fallback: because the
     * terminal comes from a remote target, a terminal-only Conflict/Busy is the
     * target's owner/queue state (Unavailable), not a source-owned
     * CapacityExceeded. A fine failure code refines this — see
     * {@link #toFrameworkErrorKind(int)}. Spec 32-framework-error-model:81,99-103.
     */
    public ZLinkFrameworkErrorKind toFrameworkErrorKind() {
        return switch (this) {
            case TIMED_OUT -> ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED;
            case NOT_FOUND -> ZLinkFrameworkErrorKind.NOT_FOUND;
            case TERMINATED -> ZLinkFrameworkErrorKind.SHUTTING_DOWN;
            case PROTOCOL_ERROR -> ZLinkFrameworkErrorKind.PROTOCOL_ERROR;
            case REJECTED -> ZLinkFrameworkErrorKind.REJECTED;
            case CONFLICT, BUSY -> ZLinkFrameworkErrorKind.UNAVAILABLE;
            case NOT_CONNECTED -> ZLinkFrameworkErrorKind.UNAVAILABLE;
            case INVALID_ARGUMENT, INVALID_STATE ->
                ZLinkFrameworkErrorKind.INVALID_OPERATION;
            case OK, INTERNAL_ERROR, NOT_SUPPORTED ->
                ZLinkFrameworkErrorKind.INTERNAL_FAILURE;
        };
    }

    /**
     * Ownership-aware remote-reply translator. A fine framework failure code
     * (mirroring C++ reply_header_exception's failure-code switch) refines the
     * coarse terminal: worker/stale/moving/data-loss causes carried on a generic
     * Conflict/Busy terminal are classified precisely instead of collapsing to
     * the coarse kind. failureCode 0 (absent) falls back to the coarse terminal.
     * Spec 32-framework-error-model:81-118, 99-103 (resource-owner rule).
     */
    public ZLinkFrameworkErrorKind toFrameworkErrorKind(int failureCode) {
        ZLinkFrameworkErrorKind fine = failureCodeErrorKind(failureCode);
        return fine != null ? fine : toFrameworkErrorKind();
    }

    private static ZLinkFrameworkErrorKind failureCodeErrorKind(int failureCode) {
        return switch (failureCode) {
            //  routeHandlerNotFound(9), requestTargetNotFound(14)
            case 9, 14 -> ZLinkFrameworkErrorKind.NOT_FOUND;
            //  payloadDecodeFailed(12), requestProtocolError(16)
            case 12, 16 -> ZLinkFrameworkErrorKind.PROTOCOL_ERROR;
            //  routeNotConnected(13), actorLocationStale(21), spotMoving(34),
            //  workerQueueFull(18: remote queue full is Unavailable, spec 32:99)
            case 13, 18, 21, 34 -> ZLinkFrameworkErrorKind.UNAVAILABLE;
            case 15 -> ZLinkFrameworkErrorKind.REJECTED;         // requestRejected
            case 19 -> ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED; // workerTimedOut
            //  requestFailed(17), workerFailed(20)
            case 17, 20 -> ZLinkFrameworkErrorKind.INTERNAL_FAILURE;
            case 33 -> ZLinkFrameworkErrorKind.INVALID_OPERATION; // spotGenerationStale
            case 35 -> ZLinkFrameworkErrorKind.DATA_LOST;         // relocationDataLost
            default -> null;
        };
    }
}
