package systems.zlink.framework.configuration;

/**
 * Selects when a SpotWide User Spot may begin relocation.
 */
public enum ZLinkSpotRelocationReadinessMode {
    ANY_TURN_BOUNDARY(0),
    APPLICATION_SIGNALED(1);

    private final int value;

    ZLinkSpotRelocationReadinessMode(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }
}
