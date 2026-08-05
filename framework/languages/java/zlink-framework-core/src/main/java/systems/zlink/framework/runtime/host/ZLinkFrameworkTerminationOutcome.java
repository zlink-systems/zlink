package systems.zlink.framework.runtime.host;

public enum ZLinkFrameworkTerminationOutcome {
    STOPPED(0),
    FORCE_STOPPED(1);

    private final int wireValue;

    ZLinkFrameworkTerminationOutcome(int wireValue) {
        this.wireValue = wireValue;
    }

    public int wireValue() {
        return wireValue;
    }
}
