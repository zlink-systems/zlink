package systems.zlink.framework.runtime.internal.locations;

public enum ZLinkObjectRejectResult {
    REJECTED(1),
    ALREADY_REJECTED(2),
    STALE(3),
    GENERATION_EXHAUSTED(4);

    private final int value;

    ZLinkObjectRejectResult(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }
}
