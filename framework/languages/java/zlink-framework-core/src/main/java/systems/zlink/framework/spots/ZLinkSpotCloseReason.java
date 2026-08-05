package systems.zlink.framework.spots;

public enum ZLinkSpotCloseReason {
    EXPLICIT_CLOSE(0),
    HOST_SHUTDOWN(1),
    RELOCATION_OUT(2),
    IDLE_EVICTED(3);

    private final int value;

    ZLinkSpotCloseReason(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }
}
