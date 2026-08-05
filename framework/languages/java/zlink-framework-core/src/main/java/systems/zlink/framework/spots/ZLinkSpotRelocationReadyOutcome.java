package systems.zlink.framework.spots;

public enum ZLinkSpotRelocationReadyOutcome {
    CONTINUED(0),
    RELOCATED(1);

    private final int value;

    ZLinkSpotRelocationReadyOutcome(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }
}
