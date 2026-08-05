# 01. Host lifecycle

[레퍼런스 목차](README.ko.md)

이 category는 `IServiceCollection` 등록 진입점과 `IZLinkFrameworkRuntime`이 제공하는 진입점을
다룬다. 정확한 signature는
[Host 등록 exact interface](../../common/spec/server/languages/dotnet/interfaces/02-configuration-host.ko.md)와
[Topology monitoring exact interface](../../common/spec/server/languages/dotnet/interfaces/10-topology-monitoring.ko.md)가
소유한다.

---

## `AddZLinkFramework` (구성 시점)

Framework root를 ASP.NET Core `IServiceCollection`에 한 번 등록한다. 다른 모든 항목의 전제
조건이다.

```csharp
services.AddZLinkFramework(options =>
{
    var play = options.AddRouteMesh("play")
        .Listen(5501)
        .SetRoutingIdPrefix("play")
        .SetPlacementWeight(100);
});

services.AddHealthChecks()
    .AddZLinkDrainHealthCheck(); // ASP.NET Core HealthCheck에 host readiness를 연동한다
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `configure: Action<IZLinkFrameworkOptions>` | 필수 | topology, handler, Location Store 등 모든 등록의 진입점이다 |
| `services.AddHealthChecks().AddZLinkDrainHealthCheck()` | 별도 등록 | `IHealthChecksBuilder` extension이다. `AddZLinkFramework`가 아니라 `AddHealthChecks()`가 반환한 builder에 붙여 host readiness probe를 추가한다 |

**완료 결과.** 반환값 없이 동기로 등록된다. Host startup 시점에 network bind 전 구성을 검증하고,
실패하면 `ZLinkConfigurationException`으로 startup 자체를 실패시킨다 — 잘못된 구성이 message 처리
중에 처음 나타나지 않는다.

**선택 기준.** 모든 host가 정확히 한 번 호출한다. `IZLinkFrameworkOptions`의 topology·handler 등록
세부는 topology-discovery category를 참고한다.

같은 exact interface가 선언하는 `services.AddZLinkHttpClient(name, configure)`는 이 host가 다른
ZLink 서비스를 HTTP client로 호출할 때 쓰는 별도 등록이며, 이 category가 다루는 server topology와는
성격이 다르다 — 이 레퍼런스의 범위 밖이다.

---

## `RelocateAsync`

현재 host가 들고 있는 stateful object(User Spot·Actor)를 다른 eligible node로 이전한다. 계획된
점검이나 rolling update 전에 호출한다.

```csharp
ZLinkFrameworkRelocationResult result = await frameworkRuntime.RelocateAsync(
    new ZLinkFrameworkRelocationOptions
    {
        Mode = ZLinkFrameworkRelocationMode.RollingUpdate,
        TargetApplicationVersion = 2,
        Deadline = TimeSpan.FromMinutes(5),
    },
    ct);

if (result.Outcome == ZLinkFrameworkRelocationOutcome.Relocated)
{
    await frameworkRuntime.ShutdownAsync(cancellationToken: ct);
}
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `Mode` | 필수 | `PlannedMaintenance`(source와 같은 version만 target) 또는 `RollingUpdate`(지정한 version만 target) |
| `TargetApplicationVersion` | `PlannedMaintenance`에서는 생략(source로 고정), `RollingUpdate`에서는 필수 | 목표 application version이다. 조합이 맞지 않으면 시작 전 `ArgumentException`으로 완료한다 |
| `Deadline` | 없음(무기한 대기) | eligible target 수렴을 기다리는 상한이다 |

**완료 결과.** `ZLinkFrameworkRelocationResult.Outcome`이 `Relocated`면 모든 object 이전이 끝나고
host는 `Relocated` 상태가 된다(새 operation은 받지 않지만 infrastructure는 유지한다). `Blocked`면
`Reason`에 `TargetUnavailable`·`StoreUnavailable`·`DeadlineExceeded` 등이 담기고, host는 처리
중이던 local object가 남아 있으면 `Serving`으로 복귀한다.

**선택 기준.** 배포 전 무중단 이전이 필요할 때 쓴다. 이전 없이 바로 종료하려면 `ShutdownAsync`를
직접 호출한다. 같은 `Mode`와 목표 version으로 중복 호출하면 진행 중인 operation에 합류하고, 다른
값으로 호출하면 `Blocked/OperationInProgress`로 완료한다.

---

## `ShutdownAsync`

Host를 종료한다. Relocation을 시작하지 않는다 — 이전이 필요하면 먼저 `RelocateAsync`를 호출한다.

```csharp
ZLinkFrameworkTerminationResult result = await frameworkRuntime.ShutdownAsync(
    deadline: TimeSpan.FromSeconds(30),
    cancellationToken: ct);
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `deadline` | 30초 | 종료 정리 상한이다. 초과하면 `ForceStopped`로 완료한다 |

**완료 결과.** `ZLinkFrameworkTerminationResult.Outcome`이 `Stopped`(정상 정리) 또는
`ForceStopped`(deadline 초과·정리 실패)다. `Serving`에서 호출하면 남은 application 처리와
resource를 정리하고, `Relocated`에서 호출하면 infrastructure 연결만 정리한다. 두 경우 모두 끝나면
`Stopped` 상태가 된다.

**선택 기준.** Host를 종료할 때 항상 호출한다. `Relocating` 도중 호출하면 진행 중인 atomic
relocation unit의 결과만 확정하고 나머지는 시작하지 않는다 — 그 relocation을 기다리던 호출자는
`Blocked/ShutdownRequested`를 받는다.

---

## `ObserveAsync` (host 상태 stream)

Host 상태 변화를 실시간으로 관찰한다. Polling 없이 상태 전이를 그대로 받는다.

```csharp
await foreach (var observed in frameworkRuntime.ObserveAsync(ct))
{
    if (observed.Status.State == ZLinkFrameworkRuntimeState.Draining)
    {
        // 준비 종료 진입을 알린다
    }
}
```

**옵션.** 이 진입점에는 modifier가 없다 — `CancellationToken`만 받는다.

**완료 결과.** Terminal 완료 없이 `IAsyncEnumerable<ZLinkObservedStatus<ZLinkFrameworkRuntimeStatus>>`를
스트리밍한다. `ZLinkObservedStatus.Loss`는 소비가 느려 coalesce되거나 버려진 상태 개수를 알려준다 —
관찰 유실 여부는 이 필드로만 판단한다.

**선택 기준.** Host state를 push 기반으로 관찰하고 싶을 때 쓴다. 지금 시점 값만 필요하면 스트림
대신 `Status` 항목을 쓴다.

---

## `Status` (읽기)

Host의 현재 상태를 한 번 읽는다.

```csharp
ZLinkFrameworkRuntimeStatus status = frameworkRuntime.Status;
bool canAcceptNewOperations = status.IsReady && status.AcceptingWork;
```

**완료 결과.** 동기 property다. `IsReady`는 `State == Serving`일 때만 true이고,
`AcceptingWork`는 새 application operation 수락 여부를 나타낸다 — 두 값이 다를 수 있으므로 둘 다
확인한다.

**선택 기준.** 지금 이 순간의 상태 한 번만 필요할 때 쓴다. 상태 전이를 놓치지 않고 계속 받으려면
`ObserveAsync`를 쓴다.

---

전체 근거는
[Host 등록 exact interface](../../common/spec/server/languages/dotnet/interfaces/02-configuration-host.ko.md),
[Topology monitoring exact interface](../../common/spec/server/languages/dotnet/interfaces/10-topology-monitoring.ko.md),
[Host Relocate·Shutdown·Handoff](../../common/spec/28-graceful-drain-handoff.ko.md)를 참고한다.
