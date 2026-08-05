# .NET Topology And Host Monitoring Public Interface

[.NET exact interface table of contents](README.en.md) ·
[Runtime Monitoring](../../../../24-runtime-monitoring.en.md) ·
[Host Relocate, Shutdown & Handoff](../../../../28-graceful-drain-handoff.en.md)

## 1. Scope

This document fixes the public interface a .NET application uses to
request host shutdown and confirm the operational status of RouteMesh/
ClientServer/Fanout. The status and observation stream only include the
values the application needs to judge state and choose a response.

Descriptor revision, lifecycle generation, endpoint, admission/claim/
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

public readonly record struct ZLinkInboundDispatchStatus(
    ulong ApplicationHwmBytes,
    ulong PendingPayloadBytes,
    ulong QueuedPayloadBytes,
    ulong ActivePayloadBytes,
    bool ApplicationReceivePaused,
    ulong PendingCompletionSends,
    ulong CompletionSendLimit);

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
    ZLinkFrameworkRuntimeStatus Status { get; }

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

`IsReady` is true only when `State == Serving`. `AcceptingWork` indicates
whether the current host is accepting new application operations. The
two values let readiness and admission be judged without exposing
relocation unit count or internal queue state to the application.

`RelocateAsync(...)` moves the current stateful objects to a target of
the application version the mode determines. `PlannedMaintenance`
doesn't specify `TargetApplicationVersion` — the framework fixes the
source host's `ApplicationVersion` as the effective target version.
`RollingUpdate` must specify a `TargetApplicationVersion` greater than
source. A different value combination is rejected with
`ArgumentException` before starting the operation.

The target version condition restricts the candidate set before applying
capability, capacity, and weight.

- `PlannedMaintenance`: only uses a target whose application version
  matches source.
- `RollingUpdate`: only uses a target that exactly matches the
  caller-specified application version. It doesn't automatically switch
  to a different, higher or lower, version.

Both modes exclude the source node and only use a target that's a
`Serving` Object Server compatible in stable type, relocation policy,
Relocation adapter, and capacity. If `MaintenanceWave` is set on the
source, a target of the same wave is excluded. If multiple candidates
remain, the existing node-wide placement weight is applied. If there's no
eligible target for the requested version, it waits until the deadline
for descriptor and Core ready state to converge, and then returns
`Blocked/TargetUnavailable`.

Once every object has finished moving, it returns `Relocated`, and the
host becomes `Relocated`. In this state, new application operations
aren't accepted, but infrastructure and connections are kept. If the move
can't be safely started or completed, it returns `Blocked`. The
framework cleans up not-yet-committed changes, and returns to `Serving`
if the host still has a local object to keep processing.

`ShutdownAsync(...)` doesn't start relocation. Calling it from `Serving`
cleans up remaining application processing and resources; calling it
from `Relocated` only cleans up infrastructure and connections. In both
cases, it becomes `Stopped` once shutdown finishes. If `deadline ==
null`, the default for each operation is 30 seconds.

If `ShutdownAsync(...)` is called during `Relocating`, only the current
atomic relocation unit's terminal result is confirmed, and the rest of
relocation isn't started. The relocation waiter receives
`Blocked/ShutdownRequested`, and the shutdown operation cleans up the
remaining objects and resources on the source.

The `CancellationToken` the caller passes only ends that waiter. An
already-started shared lifecycle operation keeps running and doesn't
affect other waiters or host lifecycle. A waiter that repeatedly calls
the same operation shares the in-progress operation and terminal result.
Only a call with the same `Mode` and effective target application
version joins. Calling with different options doesn't change the
existing operation and returns `Blocked/OperationInProgress`.

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

Between two Object Clients, a peer connection isn't made only when
neither side has RouteMesh Channel Server membership. The same applies
when only Channel Client membership exists. Each other's RID appears in
`Peers` as `NotRequired`, but isn't included in `ReadyPeerCount`. This
state is excluded from liveness probe/reconnect/health failure
aggregation, and doesn't change topology to `Degraded`. If either side
has Channel Server membership, including weight `0`, a connection is
needed, and the unconnected state is `NotConnected`. A peer that needs a
connection but has no ready connection is shown as `NotConnected` and
reflected in failure aggregation. Peer observation for a Channel-only
topology whose Object role is `None` keeps the existing rule.

`Placement.IsAvailable` is true when this node is Object Server role and
can accept new Actors/Spots. Both Actor and Spot capacity, and
activation concurrency, must have room. Population reservation, the
current value of activation concurrency, the activation barrier, and
per-stable-type internal counts aren't included in public status.

## 5. ClientServer

A Server registered on the same process is also a normal target subject
to the same weight rule as a remote Server. Status provides the total
selectable target count and per-target operational status, and doesn't
provide endpoint or discovery revision.

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

`ReadyTargetCount` includes every Ready Server with positive weight that
isn't draining, without distinguishing local/remote. `Targets` is a
read-only value for diagnostics. This list isn't used to select a
specific Server or change target weight.

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

The unit each `ObserveAsync(...)` delivers is
`ZLinkObservedStatus<TStatus>`. `Status` is a completed immutable status
at the moment state changed meaningfully, shared across observers.
`Loss` is a loss accumulator specific to this one enumeration, so it
isn't put inside status. If a consumer can't keep up with the rate of
change, intermediate statuses are coalesced and the latest status is
delivered. The status stream isn't an event log auditing every
transition.

`ZLinkObservationLoss.CoalescedCount` is the number of intermediate
statuses this consumer didn't see because per-source latest-slot
coalescing, and `DiscardedTerminalCount` is the number of terminal
statuses discarded for exceeding the retention cap. The two aren't
merged into one — because a consumer must distinguish "skipped by
catching up" from "never seen at all." Both values start at 0 per
`ObserveAsync(...)` call, increase monotonically within the same
enumeration, and are pinned at
`9223372036854775807` (`2^63 - 1`) once they exceed the `ulong`
representable range. This cap is the same across all four languages. The
framework doesn't end the enumeration just because the consumer's queue
is full. The definition of the delivery unit is owned by
[Runtime Monitoring §3](../../../../24-runtime-monitoring.en.md#3-querying-current-state-and-observing-changes).

A general-purpose event DTO whose nullable field meaning changes based on
an identifier isn't used. A consumer can use the entire received status
as the current state, without interpreting a field combination per event
kind.

`Sequence` is a value for comparing status order within the same runtime
instance. It can restart from 0 when the process restarts, and doesn't
guarantee persistence or a global order.

`CancellationToken` only ends that asynchronous enumeration. Once
cancellation is recognized, no new status is delivered, and it doesn't
affect other observers, topology connections, or host lifecycle.

## 8. Dispatch Policy And Diagnostics

The unhandled-message policy and diagnostics configuration are handled by
separate child interfaces. Dispatch configuration only acts as a root
that finds the two interfaces, and doesn't directly provide tracing mode,
observer, error sink, or file output.

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

public interface IZLinkDiagnosticsRuntime
{
    ZLinkDiagnosticsLevel Level { get; set; }
}
```

`SetSampleRate(...)` only allows `0.0` to `1.0` inclusive. Out of range
raises `ArgumentOutOfRangeException`. Recording message size adds the
payload size distribution to telemetry, without recording payload
content.

`IZLinkDiagnosticsRuntime` is a process singleton obtained from DI.
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
