package systems.zlink.framework.spots;

public enum ZLinkTimerOverrunPolicy {
    SKIP_LATE_TICKS(0),
    CATCH_UP_BOUNDED(1),
    DELAY_NEXT_TICK(2);

    private final int value;

    ZLinkTimerOverrunPolicy(int value) { this.value = value; }

    public int value() { return value; }
}
