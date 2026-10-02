package systems.zlink.framework.runtime.internal.backend;

import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;

public enum ZLinkBackendRequestResult {
    OK(RequestResult.OK),
    TIMED_OUT(RequestResult.TIMED_OUT),
    NOT_FOUND(RequestResult.NOT_FOUND),
    TERMINATED(RequestResult.TERMINATED),
    PROTOCOL_ERROR(RequestResult.PROTOCOL_ERROR),
    INTERNAL_ERROR(RequestResult.INTERNAL_ERROR),
    REJECTED(RequestResult.REJECTED),
    CONFLICT(RequestResult.CONFLICT),
    BUSY(RequestResult.BUSY),
    NOT_CONNECTED(RequestResult.NOT_CONNECTED),
    INVALID_ARGUMENT(RequestResult.INVALID_ARGUMENT),
    INVALID_STATE(RequestResult.INVALID_STATE),
    NOT_SUPPORTED(RequestResult.NOT_SUPPORTED),
    BACKPRESSURED(RequestResult.BACKPRESSURED);

    private static final ZLinkBackendRequestResult[] VALUES = values();
    private final RequestResult bindingResult;

    ZLinkBackendRequestResult(RequestResult bindingResult) {
        this.bindingResult = bindingResult;
    }

    /**
     * Maps a non-OK backend request terminal (decoded from a remote reply) to the public framework
     * error kind. Mirrors the authoritative C++ request_failure_mapper reply_header_exception
     * coarse fallback: because the terminal comes from a remote target, a terminal-only
     * Conflict/Busy is the target's owner/queue state (Unavailable), not a source-owned queue
     * capacity error. A fine failure code refines this — see {@link #toFrameworkErrorKind(int)}.
     * Spec 32-framework-error-model:81,99-103.
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
            case BACKPRESSURED -> ZLinkFrameworkErrorKind.UNAVAILABLE;
            case INVALID_ARGUMENT, INVALID_STATE -> ZLinkFrameworkErrorKind.INVALID_OPERATION;
            case OK, INTERNAL_ERROR, NOT_SUPPORTED -> ZLinkFrameworkErrorKind.INTERNAL_FAILURE;
        };
    }

    /**
     * Ownership-aware remote-reply translator. The shared mapping table uses a fine framework
     * failure code to refine the coarse terminal: worker/stale/moving/data-loss causes carried on a
     * generic Conflict/Busy terminal are classified precisely instead of collapsing to the coarse
     * kind. failureCode 0 (absent) falls back to the coarse terminal. Spec
     * 32-framework-error-model:81-118, 99-103 (resource-owner rule).
     */
    public ZLinkFrameworkErrorKind toFrameworkErrorKind(int failureCode) {
        ZLinkFrameworkErrorKind fine =
                ZLinkRequestFailureMapping.incoming(
                        failureCode, ZLinkRequestFailureMapping.Context.GENERAL);
        return fine != null ? fine : toFrameworkErrorKind();
    }

    /**
     * Maps a wire request-terminal value (0 = OK, 101.. = the non-OK terminals) to the coarse
     * backend result. Shared so every completion path classifies a remote reply terminal
     * identically. An unknown terminal is a ProtocolError.
     */
    public static ZLinkBackendRequestResult fromWireTerminal(int wireTerminal) {
        for (ZLinkBackendRequestResult value : VALUES) {
            if (wireTerminal == value.bindingResult.value()) {
                return value;
            }
        }
        return PROTOCOL_ERROR;
    }
}
