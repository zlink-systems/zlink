package systems.zlink.framework.monitoring;

/** Reports the host-wide byte budget for inbound application dispatch. */
public record ZLinkInboundDispatchStatus(
    long applicationHwmBytes,
    long pendingPayloadBytes,
    long queuedPayloadBytes,
    long activePayloadBytes,
    boolean applicationReceivePaused,
    long pendingCompletionSends,
    long completionSendLimit) {
    public ZLinkInboundDispatchStatus {
        if (applicationHwmBytes < 0
            || pendingPayloadBytes < 0
            || queuedPayloadBytes < 0
            || activePayloadBytes < 0
            || queuedPayloadBytes + activePayloadBytes != pendingPayloadBytes
            || pendingCompletionSends < 0
            || completionSendLimit < 0) {
            throw new IllegalArgumentException(
                "invalid inbound dispatch status");
        }
    }
}
