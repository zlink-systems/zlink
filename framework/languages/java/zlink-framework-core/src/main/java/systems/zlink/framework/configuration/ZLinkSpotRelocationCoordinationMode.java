package systems.zlink.framework.configuration;

/**
 * Selects when a SpotWide User Spot may begin relocation.
 */
public enum ZLinkSpotRelocationCoordinationMode {
    FRAMEWORK_MANAGED(0),
    APPLICATION_SIGNALED(1);

    private final int value;

    ZLinkSpotRelocationCoordinationMode(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }
}
