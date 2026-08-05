package systems.zlink.framework.runtime.host;

public enum ZLinkFrameworkRelocationOutcome {
    RELOCATED(0),
    BLOCKED(1);

    private final int wireValue;

    ZLinkFrameworkRelocationOutcome(int wireValue) {
        this.wireValue = wireValue;
    }

    public int wireValue() {
        return wireValue;
    }
}
