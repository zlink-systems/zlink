package systems.zlink.framework.monitoring;

import java.time.Duration;
import java.util.Objects;
import java.util.Optional;
import systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile;

/** Current host-wide Application Job Queue accounting. */
public record ZLinkApplicationJobQueueStatus(
    ZLinkApplicationJobQueueProfile configuredProfile,
    Optional<Long> configuredManualMax,
    int configuredPauseThresholdPercent,
    int configuredResumeThresholdPercent,
    long effectiveProcessorCount,
    long effectiveMaxQueuedApplicationJobs,
    long pausePermitCount,
    long resumePermitCount,
    long reservedSupplyPermits,
    long queuedApplicationJobs,
    long permitsInUse,
    long peakPermitsInUse,
    ZLinkApplicationJobQueuePressureState pressureState,
    Duration currentPauseDuration,
    long capacityWaiters,
    long capacityWaitCount,
    Duration capacityWaitDuration) {
    public ZLinkApplicationJobQueueStatus {
        Objects.requireNonNull(configuredProfile, "configuredProfile");
        configuredManualMax = configuredManualMax == null
            ? Optional.empty() : configuredManualMax;
        Objects.requireNonNull(pressureState, "pressureState");
        Objects.requireNonNull(currentPauseDuration, "currentPauseDuration");
        Objects.requireNonNull(capacityWaitDuration, "capacityWaitDuration");
    }
}
