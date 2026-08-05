package systems.zlink.framework.runtime.host;

enum ZLinkTerminationReason {
    NONE(0),
    TARGET_UNAVAILABLE(1),
    STORE_UNAVAILABLE(2),
    RELOCATION_DISABLED(3),
    STATE_INCOMPATIBLE(4),
    DEADLINE_EXCEEDED(5),
    RELOCATION_FAILED(6),
    TEARDOWN_FAILED(7),
    RUNTIME_NOT_READY(8),
    MANUAL_TOPOLOGY_UNSUPPORTED(9);

    private final int wireValue;

    ZLinkTerminationReason(int wireValue) {
        this.wireValue = wireValue;
    }

    public int wireValue() {
        return wireValue;
    }
}
