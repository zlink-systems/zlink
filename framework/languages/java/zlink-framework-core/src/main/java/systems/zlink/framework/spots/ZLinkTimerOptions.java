package systems.zlink.framework.spots;

public record ZLinkTimerOptions(
    ZLinkTimerOverrunPolicy overrunPolicy,
    int maxCatchUpTicks,
    boolean stopOnUnhandledException) {
}
