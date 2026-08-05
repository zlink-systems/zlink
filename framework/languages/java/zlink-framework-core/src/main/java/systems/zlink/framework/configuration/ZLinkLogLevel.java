package systems.zlink.framework.configuration;

public enum ZLinkLogLevel {
    TRACE(0),
    DEBUG(1),
    INFO(2),
    WARN(3),
    ERROR(4);

    private final int value;

    ZLinkLogLevel(int value) { this.value = value; }

    public int value() { return value; }
}
