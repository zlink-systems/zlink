package systems.zlink.framework.configuration;

public enum ZLinkDispatchErrorAction {
    REPLY_ERROR(0),
    DROP(1);

    private final int value;

    ZLinkDispatchErrorAction(int value) { this.value = value; }

    public int value() { return value; }
}
