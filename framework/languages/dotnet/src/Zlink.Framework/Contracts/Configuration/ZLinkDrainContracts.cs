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

/// <summary>Reports the host-wide byte budget for inbound application dispatch.</summary>
/// <param name="ApplicationHwmBytes">Applied byte limit. Zero means unlimited.</param>
/// <param name="PendingPayloadBytes">Payload bytes that have not reached terminal completion.</param>
/// <param name="QueuedPayloadBytes">Pending payload bytes waiting to enter a handler.</param>
/// <param name="ActivePayloadBytes">Pending payload bytes currently owned by handlers.</param>
/// <param name="ApplicationReceivePaused">Whether the byte limit has paused new application receives.</param>
/// <param name="PendingCompletionSends">Replies waiting for or holding a completion send permit.</param>
/// <param name="CompletionSendLimit">Host-wide completion send permit limit.</param>
/// <remarks>
/// <paramref name="PendingPayloadBytes"/> equals the sum of
/// <paramref name="QueuedPayloadBytes"/> and <paramref name="ActivePayloadBytes"/>.
/// </remarks>
public readonly record struct ZLinkInboundDispatchStatus(
    ulong ApplicationHwmBytes,
    ulong PendingPayloadBytes,
    ulong QueuedPayloadBytes,
    ulong ActivePayloadBytes,
    bool ApplicationReceivePaused,
    ulong PendingCompletionSends,
    ulong CompletionSendLimit);

/// <summary>Counts status updates that were coalesced or discarded for one observer.</summary>
public readonly record struct ZLinkObservationLoss(
    ulong CoalescedCount,
    ulong DiscardedTerminalCount);

/// <summary>Returns a status snapshot together with loss observed by this subscription.</summary>
public readonly record struct ZLinkObservedStatus<TStatus>(
    TStatus Status,
    ZLinkObservationLoss Loss)
    where TStatus : notnull;

public sealed record ZLinkFrameworkRuntimeStatus(
    ZLinkFrameworkRuntimeState State,
    bool IsReady,
    bool AcceptingWork,
    DateTimeOffset? Deadline,
    ZLinkFrameworkRelocationResult? RelocationResult,
    ZLinkFrameworkTerminationResult? TerminationResult,
    ulong Sequence,
    DateTimeOffset ObservedAt,
    ZLinkInboundDispatchStatus InboundDispatch = default);

public interface IZLinkFrameworkRuntime
{
    /// <summary>Gets the latest immutable host lifecycle status.</summary>
    ZLinkFrameworkRuntimeStatus Status { get; }

    /// <summary>Observes the latest host status without changing host lifecycle.</summary>
    IAsyncEnumerable<ZLinkObservedStatus<ZLinkFrameworkRuntimeStatus>> ObserveAsync(
        CancellationToken cancellationToken = default);

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
