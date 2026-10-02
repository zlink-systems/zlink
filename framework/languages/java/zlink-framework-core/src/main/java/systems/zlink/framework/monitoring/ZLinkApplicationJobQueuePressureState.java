package systems.zlink.framework.monitoring;

/** Current host-wide receive-flow pressure state for application jobs. */
public enum ZLinkApplicationJobQueuePressureState {
    RUNNING("running"),
    PAUSED("paused");

    private final String wire;

    ZLinkApplicationJobQueuePressureState(String wire) {
        this.wire = wire;
    }

    public String wire() {
        return wire;
    }
}
