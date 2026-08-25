package systems.zlink.framework.runtime.internal.metrics;

import java.time.Duration;
import systems.zlink.framework.monitoring.ZLinkApplicationJobQueuePressureState;

/** Internal metric-only state omitted deliberately from the public status DTO. */
public record ZLinkApplicationJobQueuePressureMetrics(
    ZLinkApplicationJobQueuePressureState pressureState,
    long runningTransitionCount,
    long pausedTransitionCount,
    Duration currentPauseDuration,
    Duration cumulativePauseDuration,
    long flowStateConfigFailureCount) {
}
