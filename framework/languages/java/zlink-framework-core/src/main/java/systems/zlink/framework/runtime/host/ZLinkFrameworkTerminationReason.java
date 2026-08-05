package systems.zlink.framework.runtime.host;

public enum ZLinkFrameworkTerminationReason {
    NONE(0),
    DEADLINE_EXCEEDED(1),
    TEARDOWN_FAILED(2);

    private final int wireValue;

    ZLinkFrameworkTerminationReason(int wireValue) {
        this.wireValue = wireValue;
    }

    public int wireValue() {
        return wireValue;
    }
}
