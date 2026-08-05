package systems.zlink.framework.configuration;

public enum ZLinkMessageFlowLogMode {
    OFF(0),
    ERRORS_ONLY(1),
    KEY_TRANSITIONS(2),
    VERBOSE(3),
    DIAGNOSTIC(4);

    private final int value;

    ZLinkMessageFlowLogMode(int value) { this.value = value; }

    public int value() { return value; }
}
