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
     * Maps a non-OK backend request terminal to the public framework error
     * kind. Mirrors the authoritative C++ request completion mapping
     * (request_failure_mapper completion_exception) so request terminals are
     * classified identically across languages instead of collapsing to
     * InternalFailure. See spec 32-framework-error-model:81
     * (request-terminal classification).
     */
    public ZLinkFrameworkErrorKind toFrameworkErrorKind() {
        return switch (this) {
            case TIMED_OUT -> ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED;
            case NOT_FOUND -> ZLinkFrameworkErrorKind.NOT_FOUND;
            case TERMINATED -> ZLinkFrameworkErrorKind.SHUTTING_DOWN;
            case PROTOCOL_ERROR -> ZLinkFrameworkErrorKind.PROTOCOL_ERROR;
            case REJECTED -> ZLinkFrameworkErrorKind.REJECTED;
            case CONFLICT, BUSY -> ZLinkFrameworkErrorKind.CAPACITY_EXCEEDED;
            case NOT_CONNECTED -> ZLinkFrameworkErrorKind.UNAVAILABLE;
            case INVALID_ARGUMENT, INVALID_STATE ->
                ZLinkFrameworkErrorKind.INVALID_OPERATION;
            case OK, INTERNAL_ERROR, NOT_SUPPORTED ->
                ZLinkFrameworkErrorKind.INTERNAL_FAILURE;
        };
    }
}
