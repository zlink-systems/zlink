# .NET topology와 host monitoring 공개 인터페이스

[.NET 언어별 interface 목차](README.ko.md) ·
[Runtime monitoring](../../../06-observability/01-runtime-monitoring.ko.md) ·
[Host Relocate, Shutdown & Handoff](../../../05-location-relocation/05-host-relocation-flow.ko.md)

## 1. 범위

이 문서는 .NET application이 host 종료를 요청하고 RouteMesh·ClientServer·Fanout의 운영 상태를
확인할 때 사용하는 public interface를 고정한다. Status와 관찰 stream에는 application이 상태를 판단하거나
대응 방법을 선택하는 데 필요한 값만 포함한다.

Framework가 topology를 조정할 때 사용하는 descriptor revision, lifecycle generation, remote descriptor endpoint,
admission·claim·reservation 단계와 Location Store record는 public interface에 포함하지 않는다.
이 값은 application이 변경할 수 없으며 Framework가 stale state와 ownership을 판정할 때만 사용한다.

## 2. Host lifecycle

`Relocating`, `Relocated`와 `Draining`은 application에 미치는 영향이 다르므로 별도 상태로 제공한다.
`Relocating`에서는 새 placement와 application admission을 받지 않고 현재 object를 다른 node로 이전한다.
`Relocated`에서는 이전이 완료되었지만 host infrastructure를 유지한다. `Draining`에서는 relocation 없이
남아 있는 application 처리와 resource를 정리한다.

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

`ZLinkCoreHwmStatus`의 `ApplicationAccountedBytes`, `OutstandingApplicationLeaseCount`,
`RetiredQueueCount`, `DeferredOriginCreditBytes`는 ABI 호환용 reserved field이며 0.13.1 이후 항상 `0`이다.
Framework는 이를 그대로 투영하며 Application Job Queue pressure로 다시 해석하지 않는다.

`GetListenerStatus(kind, name)`은 [Network listener identity §3.1](../../../02-channel-transport/04-network-listener-identity.ko.md#31-publisher가-확인하는-listener-상태)의 listener 상태 조회다.
`name`은 설정한 MeshName, ChannelName 또는 StreamNodeName이다. §3.1이 정한 configuration error는
`Kind`가 `NotConfigured`인 `ZLinkFrameworkException`으로 던진다.

`AcceptingWork`의 타입은 `bool`이다. Readiness와 새 작업 수락 상태의 관찰은
[Runtime monitoring §11](../../../06-observability/01-runtime-monitoring.ko.md#11-검증-요구)을 따른다.

`RelocateAsync(...)`와 `ShutdownAsync(...)`의 target 선택, deadline, lifecycle 결과와 waiter 처리는
[Host relocation과 shutdown](../../../05-location-relocation/05-host-relocation-flow.ko.md)이 정한다.
.NET은 잘못된 mode·version 조합을 `ArgumentException`으로 표현한다.

## 3. 공통 topology 상태

Host state는 process 전체의 lifecycle을 나타낸다. `ZLinkTopologyState`는 `MeshName` 또는
`ChannelName`으로 등록한 topology 하나의 가용성을 나타낸다. Host가 `Serving`이어도 특정
topology에 ready peer나 target이 없으면 그 topology만 `Degraded`일 수 있다.

Topology status는 사용자가 readiness와 장애 범위를 판단할 수 있는 닫힌 상태만 제공한다.
`ZLinkTopologyReason`은 application이 설정을 확인하거나 잠시 후 다시 관찰할지를 결정하는 데 사용한다.
세부 transport 또는 Store 오류는 .NET logging과 tracing에 기록한다.

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

`NodeRid`는 MeshNode의 transport identity이며 peer를 log와 deployment 정보에 대응시키는 데 사용한다.
별도의 운영용 node identity를 추가하지 않는다. Endpoint와 connection generation은 public status에서
제공하지 않는다.

## 4. RouteMesh

RouteMesh status는 같은 MeshName의 peer 연결, channel readiness와 object placement 가능 여부를
한 번에 보여 준다. Placement count는 이 process에 존재하는 active object만 집계한다.

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

`IsReady`는 host가 `Serving`이고 해당 RouteMesh가 application traffic을 처리할 수 있을 때 true다.
`ReadyPeerCount`는 ready 상태인 remote MeshNode 수다. Local channel도 정상적으로 사용할 수 있으므로
peer가 0개라는 이유만으로 모든 RouteMesh를 unavailable로 판정하지 않는다.

`NotRequired`·`NotConnected` peer 상태와 liveness 집계는 [Channel topology §8](../../../02-channel-transport/01-channel-topology.ko.md)가 정한다.

`Placement.IsAvailable`의 관찰 의미는 [Runtime monitoring](../../../06-observability/01-runtime-monitoring.ko.md)이 정한다.

## 5. ClientServer

Local·remote target 집합과 선택 조건은 [ClientServer channel §4](../../../02-channel-transport/03-client-server-channel.ko.md)가 정한다.

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

`ReadyTargetCount`와 `Targets`는 [ClientServer channel §4](../../../02-channel-transport/03-client-server-channel.ko.md)의 target 상태를 투영한다.

## 6. Fanout

Fanout runtime status는 automatic subscriber가 현재 사용할 수 있는 publisher 연결을 보여 준다.
개별 publisher의 endpoint, discovery source와 generation은 Framework가 관리한다.

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

Manual subscriber의 연결 목록은 manual connection API가 소유한다. Manual ChannelName을
`IZLinkFanoutRuntime`으로 조회하면 `ZLinkConfigurationException`이 발생한다.

## 7. 관찰 stream

`ObserveAsync(...)`는 `ZLinkObservedStatus<TStatus>`를 전달하며 `Loss`는 `ZLinkObservationLoss`다.
`CoalescedCount`와 `DiscardedTerminalCount`는 `ulong`이다.
관찰 합치기·유실·순서와 `SafeToShutdown`의 의미는
[Runtime monitoring](../../../06-observability/01-runtime-monitoring.ko.md)이 정한다.
`CancellationToken`은 .NET enumeration 취소 인자다.

## 8. Dispatch policy와 diagnostics

Unhandled message 정책과 diagnostics 설정은 `ConfigureDispatch()`의 별도 child interface가 담당한다.
Core HWM과 Application Job Queue 설정은 독립된 `ConfigureInboundDispatch()` surface가 소유한다.
Tracing mode는 diagnostics child가 소유하며 observer, error sink와 file output은 제공하지 않는다.

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

`SetSampleRate(...)`는 `0.0` 이상 `1.0` 이하만 허용한다. 범위를 벗어나면
`ArgumentOutOfRangeException`이 발생한다. Message size를 기록하면 payload 크기 분포가 telemetry에
추가되며 payload 내용은 기록하지 않는다.

`IZLinkDiagnosticsRuntime`은 DI에서 얻는 process singleton이다. `SetLevelAsync`가 정본 비동기 제어이고
`Level` setter는 그 위의 동기 bridge다. handler나 callback 같은 Framework 실행 문맥에서는 setter 대신
`SetLevelAsync`를 사용한다. `Level`을 읽으면 현재 process에
적용하는 level을 반환한다. 값을 바꾸면 이후에 시작하는 message 처리부터 새 level을 적용한다.
변경은 message 처리를 기다리지 않는 원자적 상태 변경이다. 이미 telemetry queue에 들어간 기록은
전달하거나 버릴 수 있으며, 다시 켜도 이전 처리의 기록을 소급해서 만들지 않는다.

.NET runtime은 trace를 `ActivitySource`, metric을 `System.Diagnostics.Metrics.Meter`, log를
`Microsoft.Extensions.Logging.ILogger`로 제공한다. Export 대상과 log 저장 위치는 application의
telemetry와 logging configuration이 결정한다. Framework는 file path를 받거나 자체 exporter lifecycle을
public API로 제공하지 않는다.
