package systems.zlink.framework.monitoring;

/** Loss counters accumulated independently for one monitoring subscription. */
public record ZLinkObservationLoss(
    long coalescedCount,
    long discardedTerminalCount) {
    public ZLinkObservationLoss {
        if (coalescedCount < 0 || discardedTerminalCount < 0) {
            throw new IllegalArgumentException("observation loss counters must be non-negative");
        }
    }
}
