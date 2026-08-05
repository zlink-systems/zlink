package systems.zlink.framework.runtime.internal.backend;

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
    NOT_SUPPORTED
}
