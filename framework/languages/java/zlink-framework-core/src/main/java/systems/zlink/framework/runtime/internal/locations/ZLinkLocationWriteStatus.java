package systems.zlink.framework.runtime.internal.locations;

public enum ZLinkLocationWriteStatus {
    STORED(1),
    IGNORED_STALE(2),
    REJECTED_CONFLICT(3);

    private final int value;

    ZLinkLocationWriteStatus(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }
}
