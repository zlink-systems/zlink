package systems.zlink.framework.configuration;

public enum ZLinkUnhandledDispatchAction {
    REPLY_ERROR(0),
    LOG_AND_DROP(1),
    DROP(2),
    THROW(3);

    private final int value;

    ZLinkUnhandledDispatchAction(int value) { this.value = value; }

    public int value() { return value; }
}
