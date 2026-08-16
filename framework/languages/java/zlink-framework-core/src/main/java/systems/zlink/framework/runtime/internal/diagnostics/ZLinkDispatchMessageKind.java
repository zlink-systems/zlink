package systems.zlink.framework.runtime.internal.diagnostics;

public enum ZLinkDispatchMessageKind {
    REQUEST(0, "request"),
    SEND(1, "send"),
    PUBLISH(2, "send"),
    RESPONSE(3, "response"),
    ERROR(4, "error"),
    ACTOR_REQUEST(5, "request"),
    ACTOR_SEND(6, "send"),
    CONTROL(7, "control");

    private final int value;
    private final String traceName;

    ZLinkDispatchMessageKind(int value, String traceName) {
        this.value = value;
        this.traceName = traceName;
    }

    public int value() { return value; }

    public String traceName() { return traceName; }
}
