package systems.zlink.framework.runtime.internal.diagnostics;

public enum ZLinkDispatchErrorAction {
    REPLY_ERROR(0, "reply_error"),
    DROP(1, "drop"),
    FAIL_CALLER(2, "fail_caller");

    private final int value;
    private final String traceName;

    ZLinkDispatchErrorAction(int value, String traceName) {
        this.value = value;
        this.traceName = traceName;
    }

    public int value() { return value; }

    public String traceName() { return traceName; }
}
