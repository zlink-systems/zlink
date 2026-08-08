package systems.zlink.framework.configuration;

public enum ZLinkMessageFlowLogMode {
    OFF(0),
    ERRORS(1),
    NORMAL(2),
    DETAILED(3);

    private final int value;

    ZLinkMessageFlowLogMode(int value) { this.value = value; }

    public int value() { return value; }
}
