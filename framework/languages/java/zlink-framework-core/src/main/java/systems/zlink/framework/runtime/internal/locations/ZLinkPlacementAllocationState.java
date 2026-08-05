package systems.zlink.framework.runtime.internal.locations;

public enum ZLinkPlacementAllocationState {
    PENDING(1),
    ACTIVE(2);

    private final int value;

    ZLinkPlacementAllocationState(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }
}
