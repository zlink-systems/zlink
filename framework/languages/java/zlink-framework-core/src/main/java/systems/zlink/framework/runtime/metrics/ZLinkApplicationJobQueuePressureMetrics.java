package systems.zlink.framework.runtime.internal.metrics;

import systems.zlink.framework.monitoring.ZLinkApplicationJobQueuePressureState;

import java.time.Duration;

/** Internal metric-only state omitted deliberately from the public status DTO. */
public record ZLinkApplicationJobQueuePressureMetrics(
        ZLinkApplicationJobQueuePressureState pressureState,
        long runningTransitionCount,
        long pausedTransitionCount,
        Duration currentPauseDuration,
        Duration cumulativePauseDuration,
        long flowStateConfigFailureCount) {}
