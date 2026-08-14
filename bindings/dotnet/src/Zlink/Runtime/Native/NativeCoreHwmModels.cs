// SPDX-License-Identifier: MPL-2.0

using System.Runtime.InteropServices;

namespace Systems.Zlink.Runtime.Native;

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct ZlinkAutoHwmBudgetSnapshot
{
    internal const uint CurrentAbiVersion = 1;

    public uint AbiVersion;
    public uint StructSize;
    public ulong BudgetGeneration;
    public ulong MeasurementEpoch;
    public ulong ConfiguredMemoryLimitBytes;
    public ulong RuntimeMemoryLimitBytes;
    public ulong ResolvedMemoryLimitBytes;
    public ulong ConfiguredCoreBudgetBytes;
    public ulong EffectiveCoreBudgetBytes;
    public ulong TotalPlannedHwmBytes;
    public ulong TotalAppliedHwmBytes;
    public ulong ManualReservedHwmBytes;
    public ulong CoreQueueAccountedBytes;
    public ulong ApplicationAccountedBytes;
    public ulong CurrentAccountedBytes;
    public ulong ProvisionalAccountedBytes;
    public ulong PeakAccountedBytes;
    public ulong CompletionCurrentAccountedBytes;
    public ulong CompletionPeakAccountedBytes;
    public ulong CompletionPendingMessageCount;
    public ulong TotalMessagingAccountedBytes;
    public ulong MonitorQueueAppliedHwmBytes;
    public ulong MonitorQueueAccountedBytes;
    public ulong TotalInstanceAppliedHwmBytes;
    public ulong TotalInstanceAccountedBytes;
    public ulong OversizeAdmissionCount;
    public ulong LargestOversizeMessageBytes;
    public ulong ActiveDirectionalQueueCount;
    public ulong ActiveCompletionDirectionalQueueCount;
    public ulong ActiveSendQueueCount;
    public ulong ActiveReceiveQueueCount;
    public ulong OutstandingApplicationLeaseCount;
    public ulong RetiredQueueCount;
    public ulong DeferredOriginCreditBytes;
    public ulong UnlimitedManualQueueCount;
    public uint BlockedRatioPpm;
    public uint Flags;
    public fixed ulong ReservedUInt64[8];
}
