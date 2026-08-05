package systems.zlink.framework.runtime.internal.locations;

public enum ZLinkAggregateAbortResult {
    ABORTED(1),
    ALREADY_ABORTED(2),
    STALE(3);

    private final int value;

    ZLinkAggregateAbortResult(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }
}
