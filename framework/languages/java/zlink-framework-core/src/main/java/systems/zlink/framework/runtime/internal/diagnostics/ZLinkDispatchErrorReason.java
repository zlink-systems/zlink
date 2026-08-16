package systems.zlink.framework.runtime.internal.diagnostics;

public enum ZLinkDispatchErrorReason {
    HANDLER_MISSING(0, "no_handler"),
    PAYLOAD_DECODE_FAILED(1, "decode_error"),
    HANDLER_EXCEPTION(2, "handler_exception"),
    INVALID_FRAME(3, "invalid_frame"),
    REPLY_PATH_MISSING(4, "reply_path_missing"),
    UNEXPECTED_REPLY(5, "unexpected_reply"),
    BACKPRESSURE(6, "backpressure"),
    STALE_TARGET(7, "stale_target"),
    SHUTDOWN(8, "shutdown"),
    TARGET_CLOSED(9, "target_closed"),
    LOCATION_UNAVAILABLE(10, "location_unavailable"),
    ACTIVATION_REJECTED(11, "activation_rejected"),
    ACTIVATION_TIMEOUT(12, "activation_timeout");

    private final int value;
    private final String traceName;

    ZLinkDispatchErrorReason(int value, String traceName) {
        this.value = value;
        this.traceName = traceName;
    }

    public int value() { return value; }

    public String traceName() { return traceName; }
}
