package systems.zlink.framework.runtime.host;

public enum ZLinkFrameworkRelocationReason {
    NONE(0),
    TARGET_UNAVAILABLE(1),
    STORE_UNAVAILABLE(2),
    RELOCATION_DISABLED(3),
    STATE_INCOMPATIBLE(4),
    DEADLINE_EXCEEDED(5),
    RELOCATION_FAILED(6),
    RUNTIME_NOT_READY(7),
    MANUAL_TOPOLOGY_UNSUPPORTED(8),
    SHUTDOWN_REQUESTED(9),
    OPERATION_IN_PROGRESS(10);

    private final int wireValue;

    ZLinkFrameworkRelocationReason(int wireValue) {
        this.wireValue = wireValue;
    }

    public int wireValue() {
        return wireValue;
    }
}
