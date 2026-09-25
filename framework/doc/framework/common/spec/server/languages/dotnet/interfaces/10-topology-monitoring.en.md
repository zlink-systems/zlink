# .NET Topology And Host Monitoring Public Interface

[.NET per-language interface table of contents](README.en.md) ·
[Runtime Monitoring](../../../06-observability/01-runtime-monitoring.en.md) ·
[Host Relocate, Shutdown & Handoff](../../../05-location-relocation/05-host-relocation-flow.en.md)

## 1. Scope

This document fixes the public interface a .NET application uses to
request host shutdown and confirm the operational status of RouteMesh/
ClientServer/Fanout. The status and observation stream only include the
values the application needs to judge state and choose a response.

Descriptor revision, lifecycle generation, remote descriptor endpoint, admission/claim/
reservation stage, and Location Store record — used when the framework
coordinates topology — aren't included in the public interface. These
values can't be changed by the application and are only used by the
framework to judge stale state and ownership.

## 2. Host Lifecycle

`Relocating`, `Relocated`, and `Draining` are provided as separate
states because they affect the application differently. In
`Relocating`, new placement and application admission aren't accepted,
and current objects are moved to a different node. In `Relocated`, the
move is complete but host infrastructure is kept. In `Draining`, remaining
application processing and resources are cleaned up, without relocation.

```csharp
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
    public required ZLinkFrameworkRelocationMode Mode { get; init; }
    public long? TargetApplicationVersion { get; init; }
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

public readonly record struct ZLinkObservationLoss(
    ulong CoalescedCount,
    ulong DiscardedTerminalCount);

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

public enum ZLinkListenerKind
{
    RouteMesh,
    ClientServer,
    Fanout,
    Stream,
}

public sealed record ZLinkListenerStatus(
    ZLinkListenerKind Kind,
    string Name,
    string Endpoint,
    DateTimeOffset ObservedAt);

public interface IZLinkFrameworkRuntime
{
    ZLinkFrameworkRuntimeStatus Status { get; }
    void ResetCapacityMetrics();
    ZLinkListenerStatus GetListenerStatus(ZLinkListenerKind kind, string name);

    IAsyncEnumerable<ZLinkObservedStatus<ZLinkFrameworkRuntimeStatus>> ObserveAsync(
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkFrameworkRelocationResult> RelocateAsync(
        ZLinkFrameworkRelocationOptions options,
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkFrameworkTerminationResult> ShutdownAsync(
        TimeSpan? deadline = null,
        CancellationToken cancellationToken = default);
}
```

In `ZLinkCoreHwmStatus`, `ApplicationAccountedBytes`, `OutstandingApplicationLeaseCount`,
`RetiredQueueCount`, and `DeferredOriginCreditBytes` are ABI-reserved compatibility fields and
are always `0` since 0.13.1. The framework projects them unchanged and does not reinterpret them
as Application Job Queue pressure.

`GetListenerStatus(kind, name)` is the listener state query of
[Network listener identity §3.1](../../../02-channel-transport/04-network-listener-identity.en.md#31-listener-state-the-publisher-checks). `name` is the configured
MeshName, ChannelName, or StreamNodeName. A configuration error defined in §3.1 is thrown as a
`ZLinkFrameworkException` whose `Kind` is `NotConfigured`.

`AcceptingWork` has type `bool`. Observation of readiness and new-work acceptance state follows
[Runtime monitoring §11](../../../06-observability/01-runtime-monitoring.en.md#11-verification-requirements).

[Host relocation and shutdown](../../../05-location-relocation/05-host-relocation-flow.en.md)
defines target selection, deadline, lifecycle outcomes, and waiter handling for
`RelocateAsync(...)` and `ShutdownAsync(...)`.
.NET maps an invalid mode/version combination to `ArgumentException`.

## 3. Common Topology State

Host state represents the whole process's lifecycle. `ZLinkTopologyState`
represents the availability of one topology registered by `MeshName` or
`ChannelName`. Even if the host is `Serving`, only that specific topology
can be `Degraded` if it has no ready peer or target.

Topology status only provides closed states from which the user can judge
readiness and failure scope. `ZLinkTopologyReason` is used by the
application to decide whether to check configuration or observe again
later. Detailed transport or Store errors are recorded in .NET logging
and tracing.

```csharp
public enum ZLinkTopologyState
{
    Starting = 0,
    Ready = 1,
    Degraded = 2,
    Stopping = 3,
    Stopped = 4,
    Failed = 5
}

public enum ZLinkTopologyReason
{
    RuntimeNotReady = 0,
    NoReadyPeer = 1,
    NoReadyTarget = 2,
    LocationUnavailable = 3,
    CapacityExceeded = 4,
    Draining = 5,
    InternalFailure = 6
}

public enum ZLinkPeerState
{
    Connecting = 0,
    Ready = 1,
    Draining = 2,
    NotConnected = 3,
    NotRequired = 4
}

public sealed record ZLinkChannelStatus(
    string ChannelName,
    bool IsReady,
    int ReadyTargetCount);

public sealed record ZLinkPeerStatus(
    RoutingId NodeRid,
    ZLinkPeerState State,
    ZLinkTopologyReason? UnavailableReason);
```

`NodeRid` is the MeshNode's transport identity, used to correlate a peer
with log and deployment information. A separate operational node identity
isn't added. Endpoint and connection generation aren't provided in public
status.

## 4. RouteMesh

RouteMesh status shows, at once, peer connections of the same MeshName,
channel readiness, and object placement availability. Placement count
only aggregates active objects that exist in this process.

```csharp
public sealed record ZLinkPlacementStatus(
    bool IsAvailable,
    int ActiveActorCount,
    int ActiveSpotCount,
    ZLinkTopologyReason? UnavailableReason);

public sealed record ZLinkRouteMeshStatus(
    string MeshName,
    ZLinkTopologyState State,
    bool IsReady,
    int ReadyPeerCount,
    IReadOnlyList<ZLinkChannelStatus> Channels,
    IReadOnlyList<ZLinkPeerStatus> Peers,
    ZLinkPlacementStatus Placement,
    ulong Sequence,
    DateTimeOffset ObservedAt);

public interface IZLinkRouteMeshRuntime
{
    ZLinkRouteMeshStatus GetStatus(string meshName);

    IAsyncEnumerable<ZLinkObservedStatus<ZLinkRouteMeshStatus>> ObserveAsync(
        string meshName,
        CancellationToken cancellationToken = default);
}
```

`IsReady` is true when the host is `Serving` and that RouteMesh can
process application traffic. `ReadyPeerCount` is the number of remote
MeshNodes in ready state. Since a local channel can also be used
normally, having 0 peers alone doesn't make every RouteMesh judged
unavailable.

[Channel topology §8](../../../02-channel-transport/01-channel-topology.en.md) defines NotRequired and NotConnected peer status and liveness accounting.

[Runtime monitoring](../../../06-observability/01-runtime-monitoring.en.md) defines the observed meaning of `Placement.IsAvailable`.

## 5. ClientServer

[ClientServer channel §4](../../../02-channel-transport/03-client-server-channel.en.md) defines the local and remote target set and selection conditions.

```csharp
public enum ZLinkClientServerRole
{
    Client = 1,
    Server = 2,
    ClientAndServer = 3
}

public sealed record ZLinkClientServerTargetStatus(
    RoutingId NodeRid,
    int Weight,
    ZLinkPeerState State,
    ZLinkTopologyReason? UnavailableReason);

public sealed record ZLinkClientServerStatus(
    string ChannelName,
    ZLinkClientServerRole LocalRole,
    ZLinkTopologyState State,
    bool IsReady,
    int ReadyTargetCount,
    IReadOnlyList<ZLinkClientServerTargetStatus> Targets,
    ulong Sequence,
    DateTimeOffset ObservedAt);

public interface IZLinkClientServerRuntime
{
    ZLinkClientServerStatus GetStatus(string channelName);

    IAsyncEnumerable<ZLinkObservedStatus<ZLinkClientServerStatus>> ObserveAsync(
        string channelName,
        CancellationToken cancellationToken = default);
}
```

`ReadyTargetCount` and `Targets` project the target status defined by [ClientServer channel §4](../../../02-channel-transport/03-client-server-channel.en.md).

## 6. Fanout

Fanout runtime status shows the publisher connections currently
available to an automatic subscriber. An individual publisher's
endpoint, discovery source, and generation are managed by the framework.

```csharp
public sealed record ZLinkFanoutStatus(
    string ChannelName,
    ZLinkTopologyState State,
    bool IsReady,
    int ReadyPublisherCount,
    IReadOnlyList<ZLinkPeerStatus> Publishers,
    ulong Sequence,
    DateTimeOffset ObservedAt);

public interface IZLinkFanoutRuntime
{
    ZLinkFanoutStatus GetStatus(string channelName);

    IAsyncEnumerable<ZLinkObservedStatus<ZLinkFanoutStatus>> ObserveAsync(
        string channelName,
        CancellationToken cancellationToken = default);
}
```

The connection list of a manual subscriber is owned by the manual
connection API. Querying a manual ChannelName through
`IZLinkFanoutRuntime` raises `ZLinkConfigurationException`.

## 7. Observation Stream

`ObserveAsync(...)` delivers `ZLinkObservedStatus<TStatus>`; `Loss` is
`ZLinkObservationLoss`, and `CoalescedCount` and `DiscardedTerminalCount`
are `ulong`. [Runtime monitoring](../../../06-observability/01-runtime-monitoring.en.md)
defines coalescing, loss, sequence, and `SafeToShutdown`.
`CancellationToken` is the .NET enumeration cancellation argument.

## 8. Dispatch Policy And Diagnostics

The unhandled-message policy and diagnostics configuration are handled by separate child
interfaces under `ConfigureDispatch()`. Core HWM and application-job-queue settings are owned
by the independent `ConfigureInboundDispatch()` surface. Tracing mode is owned by the
diagnostics child; observer, error sink, and file output aren't provided.

```csharp
public enum ZLinkUnhandledDispatchAction
{
    ReplyError = 0,
    LogAndDrop = 1,
    Drop = 2,
    Throw = 3
}

public interface IZLinkUnhandledDispatchOptions
{
    ZLinkUnhandledDispatchAction Request { get; set; }
    ZLinkUnhandledDispatchAction Send { get; set; }
    ZLinkUnhandledDispatchAction Publish { get; set; }
}

public enum ZLinkDiagnosticsLevel
{
    Off = 0,
    Errors = 1,
    Normal = 2,
    Detailed = 3
}

public interface IZLinkDiagnosticsOptions
{
    IZLinkDiagnosticsOptions SetLevel(ZLinkDiagnosticsLevel level);
    IZLinkDiagnosticsOptions SetSampleRate(double rate);
    IZLinkDiagnosticsOptions IncludeMessageSizes(bool include);
}

public interface IZLinkDispatchOptions
{
    IZLinkUnhandledDispatchOptions Unhandled { get; }
    IZLinkDiagnosticsOptions Diagnostics { get; }
}

public interface IZLinkInboundDispatchOptions
{
    ulong? CoreHwmMemoryLimitBytes { get; set; }
    ulong? CoreHwmBudgetBytes { get; set; }
    ZLinkCoreHwmProfile CoreHwmProfile { get; set; }
    ZLinkApplicationJobQueueProfile ApplicationJobQueueProfile { get; set; }
    ulong? MaxQueuedApplicationJobs { get; set; }
    uint ApplicationJobQueuePauseThresholdPercent { get; set; }
    uint ApplicationJobQueueResumeThresholdPercent { get; set; }
}

public interface IZLinkDiagnosticsRuntime
{
    ZLinkDiagnosticsLevel Level { get; set; }
    Task SetLevelAsync(ZLinkDiagnosticsLevel level);
}
```

`SetSampleRate(...)` only allows `0.0` to `1.0` inclusive. Out of range
raises `ArgumentOutOfRangeException`. Recording message size adds the
payload size distribution to telemetry, without recording payload
content.

`IZLinkDiagnosticsRuntime` is a process singleton obtained from DI.
`SetLevelAsync` is the canonical asynchronous control; the `Level` setter is the synchronous bridge over it.
Use `SetLevelAsync` from framework execution contexts such as handlers and callbacks.
Reading `Level` returns the level currently applied to the process.
Changing the value applies the new level starting from message processing
that begins afterward. The change is an atomic state change that doesn't
wait for message processing. A record already in the telemetry queue can
be delivered or dropped, and turning it back on doesn't retroactively
create records for earlier processing.

The .NET runtime provides trace as `ActivitySource`, metric as
`System.Diagnostics.Metrics.Meter`, and log as
`Microsoft.Extensions.Logging.ILogger`. The export destination and log
storage location are decided by the application's telemetry and logging
configuration. The framework doesn't take a file path or provide its own
exporter lifetime as a public API.
