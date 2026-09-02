// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

/// <summary>
///     Immutable projection of Core's context-wide Auto HWM budget ABI v1.
///     Completion counters are diagnostic and are excluded from application
///     budget and directional queue counts.
/// </summary>
public sealed class CoreHwmBudgetSnapshot
{
    internal CoreHwmBudgetSnapshot()
    {
    }

    /// <summary>Gets the native ABI version.</summary>
    public uint AbiVersion { get; internal init; }
    /// <summary>Gets the native structure size in bytes.</summary>
    public uint StructSize { get; internal init; }
    /// <summary>Gets the budget plan generation.</summary>
    public ulong BudgetGeneration { get; internal init; }
    /// <summary>Gets the metrics measurement epoch.</summary>
    public ulong MeasurementEpoch { get; internal init; }
    /// <summary>Gets the configured memory limit in bytes.</summary>
    public ulong ConfiguredMemoryLimitBytes { get; internal init; }
    /// <summary>Gets the managed runtime memory hint in bytes.</summary>
    public ulong RuntimeMemoryLimitBytes { get; internal init; }
    /// <summary>Gets the resolved memory limit in bytes.</summary>
    public ulong ResolvedMemoryLimitBytes { get; internal init; }
    /// <summary>Gets the configured manual Core budget in bytes.</summary>
    public ulong ConfiguredCoreBudgetBytes { get; internal init; }
    /// <summary>Gets the effective Core budget in bytes.</summary>
    public ulong EffectiveCoreBudgetBytes { get; internal init; }
    /// <summary>Gets total planned application HWM bytes.</summary>
    public ulong TotalPlannedHwmBytes { get; internal init; }
    /// <summary>Gets total applied application HWM bytes.</summary>
    public ulong TotalAppliedHwmBytes { get; internal init; }
    /// <summary>Gets HWM bytes reserved by manual queues.</summary>
    public ulong ManualReservedHwmBytes { get; internal init; }
    /// <summary>Gets bytes accounted in Core application queues.</summary>
    public ulong CoreQueueAccountedBytes { get; internal init; }
    /// <summary>Gets the ABI-reserved application-accounted field; always zero.</summary>
    public ulong ApplicationAccountedBytes { get; internal init; }
    /// <summary>Gets current application-accounted bytes.</summary>
    public ulong CurrentAccountedBytes { get; internal init; }
    /// <summary>Gets provisional multipart bytes.</summary>
    public ulong ProvisionalAccountedBytes { get; internal init; }
    /// <summary>Gets peak application-accounted bytes.</summary>
    public ulong PeakAccountedBytes { get; internal init; }
    /// <summary>Gets current Completion-connection bytes for count-2 ROUTER-ROUTER peers.</summary>
    public ulong CompletionCurrentAccountedBytes { get; internal init; }
    /// <summary>Gets peak Completion-connection bytes for count-2 ROUTER-ROUTER peers.</summary>
    public ulong CompletionPeakAccountedBytes { get; internal init; }
    /// <summary>Gets pending completion message count.</summary>
    public ulong CompletionPendingMessageCount { get; internal init; }
    /// <summary>Gets total application and completion bytes.</summary>
    public ulong TotalMessagingAccountedBytes { get; internal init; }
    /// <summary>Gets monitor queue applied HWM bytes.</summary>
    public ulong MonitorQueueAppliedHwmBytes { get; internal init; }
    /// <summary>Gets monitor queue accounted bytes.</summary>
    public ulong MonitorQueueAccountedBytes { get; internal init; }
    /// <summary>Gets total instance applied HWM bytes.</summary>
    public ulong TotalInstanceAppliedHwmBytes { get; internal init; }
    /// <summary>Gets total instance accounted bytes.</summary>
    public ulong TotalInstanceAccountedBytes { get; internal init; }
    /// <summary>Gets the oversize admission count.</summary>
    public ulong OversizeAdmissionCount { get; internal init; }
    /// <summary>Gets the largest admitted oversize message in bytes.</summary>
    public ulong LargestOversizeMessageBytes { get; internal init; }
    /// <summary>Gets active application directional queue count.</summary>
    public ulong ActiveDirectionalQueueCount { get; internal init; }
    /// <summary>Gets active completion directional queue count.</summary>
    public ulong ActiveCompletionDirectionalQueueCount { get; internal init; }
    /// <summary>Gets active application send queue count.</summary>
    public ulong ActiveSendQueueCount { get; internal init; }
    /// <summary>Gets active application receive queue count.</summary>
    public ulong ActiveReceiveQueueCount { get; internal init; }
    /// <summary>Gets the ABI-reserved application-lease count; always zero.</summary>
    public ulong OutstandingApplicationLeaseCount { get; internal init; }
    /// <summary>Gets the ABI-reserved retired-queue count; always zero.</summary>
    public ulong RetiredQueueCount { get; internal init; }
    /// <summary>Gets the ABI-reserved deferred-credit field; always zero.</summary>
    public ulong DeferredOriginCreditBytes { get; internal init; }
    /// <summary>Gets unlimited manual queue count.</summary>
    public ulong UnlimitedManualQueueCount { get; internal init; }
    /// <summary>Gets the send-blocked ratio in parts per million.</summary>
    public uint BlockedRatioPpm { get; internal init; }
    /// <summary>Gets the native snapshot flag mask.</summary>
    public uint Flags { get; internal init; }
    /// <summary>Gets reserved ABI extension slots.</summary>
    public IReadOnlyList<ulong> ReservedUInt64 { get; internal init; } =
        Array.Empty<ulong>();

    /// <summary>Gets whether budget planning is active.</summary>
    public bool BudgetPlanningActive => (Flags & (1U << 0)) != 0;
    /// <summary>Gets whether the budget is insufficient.</summary>
    public bool BudgetInsufficient => (Flags & (1U << 1)) != 0;
    /// <summary>Gets whether aggregate HWM totals are valid.</summary>
    public bool AggregateHwmValid => (Flags & (1U << 2)) != 0;
    /// <summary>Gets whether aggregate HWM totals overflowed.</summary>
    public bool AggregateOverflow => (Flags & (1U << 3)) != 0;
}
