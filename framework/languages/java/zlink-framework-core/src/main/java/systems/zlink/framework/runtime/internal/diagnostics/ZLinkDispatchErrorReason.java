package systems.zlink.framework.runtime.internal.diagnostics;

public enum ZLinkDispatchErrorReason {
    HANDLER_MISSING(0),
    PAYLOAD_DECODE_FAILED(1),
    HANDLER_EXCEPTION(2),
    INVALID_FRAME(3),
    REPLY_PATH_MISSING(4),
    UNEXPECTED_REPLY(5);

    private final int value;

    ZLinkDispatchErrorReason(int value) { this.value = value; }

    public int value() { return value; }
}
