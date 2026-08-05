package systems.zlink.framework.runtime.internal.locations;

public enum ZLinkAggregateCommitResult {
    COMMITTED(1),
    ALREADY_COMMITTED(2),
    STALE(3),
    GENERATION_EXHAUSTED(4);

    private final int value;

    ZLinkAggregateCommitResult(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }
}
