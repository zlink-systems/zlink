package systems.zlink.framework.runtime.internal.locations;

public enum ZLinkObjectCommitResult {
    COMMITTED(1),
    ALREADY_COMMITTED(2),
    STALE(3),
    GENERATION_EXHAUSTED(4);

    private final int value;

    ZLinkObjectCommitResult(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }
}
