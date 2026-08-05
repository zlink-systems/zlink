package systems.zlink.framework.configuration;

public enum ZLinkDispatchMessageKind {
    REQUEST(0),
    SEND(1),
    PUBLISH(2),
    RESPONSE(3),
    ERROR(4),
    ACTOR_REQUEST(5),
    ACTOR_SEND(6);

    private final int value;

    ZLinkDispatchMessageKind(int value) { this.value = value; }

    public int value() { return value; }
}
