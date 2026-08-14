/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.core;

import java.util.List;

/**
 * Immutable Core Auto HWM ABI-v1 snapshot. Completion counters are diagnostic
 * and are excluded from the application queue budget and directional count.
 */
public record CoreHwmBudgetSnapshot(
    int abiVersion,
    int structSize,
    long budgetGeneration,
    long measurementEpoch,
    long configuredMemoryLimitBytes,
    long runtimeMemoryLimitBytes,
    long resolvedMemoryLimitBytes,
    long configuredCoreBudgetBytes,
    long effectiveCoreBudgetBytes,
    long totalPlannedHwmBytes,
    long totalAppliedHwmBytes,
    long manualReservedHwmBytes,
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
    long oversizeAdmissionCount,
    long largestOversizeMessageBytes,
    long activeDirectionalQueueCount,
    long activeCompletionDirectionalQueueCount,
    long activeSendQueueCount,
    long activeReceiveQueueCount,
    long outstandingApplicationLeaseCount,
    long retiredQueueCount,
    long deferredOriginCreditBytes,
    long unlimitedManualQueueCount,
    int blockedRatioPpm,
    int flags,
    List<Long> reservedUInt64) {

    public CoreHwmBudgetSnapshot {
        reservedUInt64 = List.copyOf(reservedUInt64);
    }

    public boolean budgetPlanningActive() {
        return (flags & (1 << 0)) != 0;
    }

    public boolean budgetInsufficient() {
        return (flags & (1 << 1)) != 0;
    }

    public boolean aggregateHwmValid() {
        return (flags & (1 << 2)) != 0;
    }

    public boolean aggregateOverflow() {
        return (flags & (1 << 3)) != 0;
    }
}
