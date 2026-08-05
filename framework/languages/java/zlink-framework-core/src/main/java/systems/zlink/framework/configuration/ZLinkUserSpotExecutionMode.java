package systems.zlink.framework.configuration;

public enum ZLinkUserSpotExecutionMode {
    SPOT_WIDE(0),
    PER_ACTOR(1);

    private final int value;

    ZLinkUserSpotExecutionMode(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }
}
