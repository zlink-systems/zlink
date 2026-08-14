package systems.zlink.framework.monitoring;

import java.util.Objects;
import java.util.Optional;
import systems.zlink.framework.configuration.ZLinkCoreHwmProfile;

/** Framework projection of the Core-owned runtime HWM snapshot. */
public record ZLinkCoreHwmStatus(
    Optional<Long> configuredMemoryLimitBytes,
    Optional<Long> configuredBudgetBytes,
    ZLinkCoreHwmProfile configuredProfile,
    long effectiveBudgetBytes,
    long totalAppliedHwmBytes,
    long coreQueueAccountedBytes,
    long applicationAccountedBytes,
    long currentAccountedBytes,
    long provisionalAccountedBytes,
    long peakAccountedBytes,
    long completionCurrentAccountedBytes,
    long completionPeakAccountedBytes,
    long completionPendingMessageCount,
    long totalMessagingAccountedBytes,
    long monitorQueueAppliedHwmBytes,
    long monitorQueueAccountedBytes,
    long totalInstanceAppliedHwmBytes,
    long totalInstanceAccountedBytes,
    long blockedRatioPpm,
    long activeDirectionalQueueCount,
    long activeCompletionDirectionalQueueCount,
    long activeSendQueueCount,
    long activeReceiveQueueCount,
    long outstandingApplicationLeaseCount,
    long retiredQueueCount,
    long deferredOriginCreditBytes) {
    public ZLinkCoreHwmStatus {
        configuredMemoryLimitBytes = configuredMemoryLimitBytes == null
            ? Optional.empty() : configuredMemoryLimitBytes;
        configuredBudgetBytes = configuredBudgetBytes == null
            ? Optional.empty() : configuredBudgetBytes;
        Objects.requireNonNull(configuredProfile, "configuredProfile");
    }
}
