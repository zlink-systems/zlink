namespace Zlink.Framework.Contracts.Configuration;

public enum ZLinkFrameworkRuntimeState
{
    Preparing = 0,
    Serving = 1,
    Relocating = 2,
    Relocated = 3,
    Draining = 4,
    Stopped = 5,
    Error = 6
}

public enum ZLinkFrameworkRelocationOutcome
{
    Relocated = 0,
    Blocked = 1
}

public enum ZLinkFrameworkRelocationMode
{
    PlannedMaintenance = 0,
    RollingUpdate = 1
}

public enum ZLinkFrameworkRelocationReason
{
    None = 0,
    TargetUnavailable = 1,
    StoreUnavailable = 2,
    RelocationDisabled = 3,
    StateIncompatible = 4,
    DeadlineExceeded = 5,
    RelocationFailed = 6,
    RuntimeNotReady = 7,
    ManualTopologyUnsupported = 8,
    ShutdownRequested = 9,
    OperationInProgress = 10
}

public sealed record ZLinkFrameworkRelocationOptions
{
    /// <summary>Selects same-version maintenance or an exact-version update.</summary>
    public required ZLinkFrameworkRelocationMode Mode { get; init; }

    /// <summary>
    /// Identifies the exact target version for a rolling update. It must be
    /// omitted for planned maintenance.
    /// </summary>
    public long? TargetApplicationVersion { get; init; }

    /// <summary>Limits this relocation attempt. The default is 30 seconds.</summary>
    public TimeSpan? Deadline { get; init; }
}

public readonly record struct ZLinkFrameworkRelocationResult(
    ZLinkFrameworkRelocationMode Mode,
    long TargetApplicationVersion,
    ZLinkFrameworkRelocationOutcome Outcome,
    ZLinkFrameworkRelocationReason Reason);

public enum ZLinkFrameworkTerminationOutcome
{
    Stopped = 0,
    ForceStopped = 1
}

public enum ZLinkFrameworkTerminationReason
{
    None = 0,
    DeadlineExceeded = 1,
    TeardownFailed = 2
}

public readonly record struct ZLinkFrameworkTerminationResult(
    ZLinkFrameworkTerminationOutcome Outcome,
    ZLinkFrameworkTerminationReason Reason);

/// <summary>Counts status updates that were coalesced or discarded for one observer.</summary>
public readonly record struct ZLinkObservationLoss(
    ulong CoalescedCount,
    ulong DiscardedTerminalCount);

/// <summary>Returns a status snapshot together with loss observed by this subscription.</summary>
public readonly record struct ZLinkObservedStatus<TStatus>(
    TStatus Status,
    ZLinkObservationLoss Loss)
    where TStatus : notnull;

public readonly record struct ZLinkCoreHwmStatus(
    ulong? ConfiguredMemoryLimitBytes,
    ulong? ConfiguredBudgetBytes,
    ZLinkCoreHwmProfile ConfiguredProfile,
    ulong EffectiveBudgetBytes,
    ulong TotalAppliedHwmBytes,
    ulong CoreQueueAccountedBytes,
    ulong ApplicationAccountedBytes,
    ulong CurrentAccountedBytes,
    ulong ProvisionalAccountedBytes,
    ulong PeakAccountedBytes,
    ulong CompletionCurrentAccountedBytes,
    ulong CompletionPeakAccountedBytes,
    ulong CompletionPendingMessageCount,
    ulong TotalMessagingAccountedBytes,
    ulong MonitorQueueAppliedHwmBytes,
    ulong MonitorQueueAccountedBytes,
    ulong TotalInstanceAppliedHwmBytes,
    ulong TotalInstanceAccountedBytes,
    ulong BlockedRatioPpm,
    ulong ActiveDirectionalQueueCount,
    ulong ActiveCompletionDirectionalQueueCount,
    ulong ActiveSendQueueCount,
    ulong ActiveReceiveQueueCount,
    ulong OutstandingApplicationLeaseCount,
    ulong RetiredQueueCount,
    ulong DeferredOriginCreditBytes);

public readonly record struct ZLinkApplicationJobQueueStatus(
    ZLinkApplicationJobQueueProfile ConfiguredProfile,
    ulong? ConfiguredManualMax,
    uint ConfiguredPauseThresholdPercent,
    uint ConfiguredResumeThresholdPercent,
    ulong EffectiveProcessorCount,
    ulong EffectiveMaxQueuedApplicationJobs,
    ulong PausePermitCount,
    ulong ResumePermitCount,
    ulong ReservedSupplyPermits,
    ulong QueuedApplicationJobs,
    ulong PermitsInUse,
    ulong PeakPermitsInUse,
    ulong CapacityWaiters,
    ulong CapacityWaitCount,
    TimeSpan CapacityWaitDuration,
    ZLinkApplicationJobQueuePressureState PressureState,
    TimeSpan CurrentPauseDuration);

public readonly record struct ZLinkHostCapacityStatus(
    ulong MeasurementEpoch,
    ZLinkCoreHwmStatus CoreHwm,
    ZLinkApplicationJobQueueStatus ApplicationJobQueue);

//  SafeToShutdown (spec 30 §11): the source-published observation that
//  every relocation unit this source started has reached its Message
//  Follow route removal point (S4) and its cutover retransmission window
//  has ended. A source-local observation, never a completion ACK from a
//  target. True when this source has not started a relocation.
public sealed record ZLinkFrameworkRuntimeStatus(
    ZLinkFrameworkRuntimeState State,
    bool IsReady,
    bool AcceptingWork,
    DateTimeOffset? Deadline,
    ZLinkFrameworkRelocationResult? RelocationResult,
    ZLinkFrameworkTerminationResult? TerminationResult,
    ulong Sequence,
    DateTimeOffset ObservedAt,
    ZLinkHostCapacityStatus Capacity = default,
    bool SafeToShutdown = true);

public interface IZLinkFrameworkRuntime
{
    /// <summary>Gets the latest immutable host lifecycle status.</summary>
    ZLinkFrameworkRuntimeStatus Status { get; }

    /// <summary>Observes the latest host status without changing host lifecycle.</summary>
    IAsyncEnumerable<ZLinkObservedStatus<ZLinkFrameworkRuntimeStatus>> ObserveAsync(
        CancellationToken cancellationToken = default);

    /// <summary>
    /// Resets host-capacity epoch counters while preserving configuration and
    /// current gauges. The Framework runtime must be active.
    /// </summary>
    void ResetCapacityMetrics();

    /// <summary>
    /// Moves stateful workload from this host. Successful completion leaves
    /// infrastructure active in the Relocated state.
    /// </summary>
    ValueTask<ZLinkFrameworkRelocationResult> RelocateAsync(
        ZLinkFrameworkRelocationOptions options,
        CancellationToken cancellationToken = default);

    /// <summary>
    /// Stops this host. It does not implicitly request stateful relocation.
    /// </summary>
    ValueTask<ZLinkFrameworkTerminationResult> ShutdownAsync(
        TimeSpan? deadline = null,
        CancellationToken cancellationToken = default);
}
