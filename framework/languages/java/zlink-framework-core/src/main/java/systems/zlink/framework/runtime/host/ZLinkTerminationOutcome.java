package systems.zlink.framework.runtime.host;

enum ZLinkTerminationOutcome {
    STOPPED(0),
    BLOCKED(1),
    FORCE_STOPPED(2);

    private final int wireValue;

    ZLinkTerminationOutcome(int wireValue) {
        this.wireValue = wireValue;
    }

    public int wireValue() {
        return wireValue;
    }
}
