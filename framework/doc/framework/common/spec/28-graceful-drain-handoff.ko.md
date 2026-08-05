---
title: "Host Relocate와 Shutdown"
---

# Host Relocate와 Shutdown

[스펙 목차](README.ko.md) · [이전: Request 연결과 업무 흐름 식별](27-flow-correlation.ko.md) · [다음: Transport 연결 상태 확인](29-transport-liveness.ko.md)

> **이 장이 정의하는 것** — host의 stateful workload를 다른 node로 이전하거나 host를
> 종료할 때의 호출 순서와 결과.


## 1. 이 문서가 답하는 질문

이 문서는 application이 host의 stateful workload를 다른 node로 이전하거나 host를
종료할 때 어떤 operation을 호출하고, Framework가 어떤 순서로 처리하며, 호출이 어떤
결과로 끝나는지를 정의한다.

Message handler와 Actor를 실행하는 단위를
[Spot](01-glossary.ko.md#spot)이라고 한다. 이 문서에서 stateful workload는 host가
처리 중인 Actor·Spot, 아직 끝나지 않은 message와 timer를 뜻한다.

Application version을 유지한 채 node를 점검하거나 재부팅하려면
`PlannedMaintenance`로 `Relocate`를 호출한다. 준비한 새 application version으로
교체하려면 `RollingUpdate`로 호출한다. Target version 선택 방식을
[relocation mode](01-glossary.ko.md#relocation-mode)라고 한다.

두 mode 모두 성공하면 stateful workload만 source host에서 분리된다. Host와
infrastructure connection은 유지된다. Application 또는 deployment orchestrator는
이 결과를 확인한 뒤 [Shutdown](01-glossary.ko.md#shutdown)을 별도로 호출한다.

Stateful workload의 연속성을 보장하지 않고 host를 종료하려면 `Relocate` 없이
`Shutdown`만 호출한다.

### 1.1 장애 처리 범위

`Relocate`는 source runtime, 선택한 target runtime, Location Store와
Relocation Store가 operation을 끝낼 때까지 실행되는 graceful handoff만 지원한다.
같은 process 안에서 일시적인 Store 또는 transport 오류를 deadline 안에 다시 시도할
수는 있다. 그러나 source나 target process가 종료된 뒤 다른 runtime이 relocation을
이어받거나, 다른 target을 선택하여 자동으로 복구하지 않는다. 이 기능은 차기 version의
object failover 계약에서 정의한다.

이번 version에서도 owner를 동시에 둘로 만들지 않는 규칙은 반드시 지킨다. Location Store
변경 결과를 받지 못하면 성공이나 실패를 추측하지 않고 같은 record를 다시 읽는다. 실제
owner를 확인하기 전에는 source admission을 다시 열거나 target application message 처리를
시작하지 않는다.

같은 이름으로 연결된 node의 RouteMesh 그룹을 구분하는 이름을
[MeshName](01-glossary.ko.md#meshname)이라고 한다. 같은 Channel에 참여한 target을
고르는 이름은 [ChannelName](01-glossary.ko.md#channelname)이다. Application은
MeshName, ChannelName 또는 node RID로 일부 component만 골라 종료 순서를 직접
조립하지 않는다.

여러 node가 message를 주고받는 연결 그룹인
[RouteMesh](01-glossary.ko.md#routemesh), 그 그룹에 참여하는 runtime node인
[MeshNode](01-glossary.ko.md#meshnode), ClientServer server와
[Classic fanout](01-glossary.ko.md#classic-fanout) publisher를 host 단위로 함께
조정한다.

이 문서는 application이 관찰하는 host lifecycle과 handoff 결과를 소유한다.
Actor·Spot을 현재 처리하는 node를 [owner](01-glossary.ko.md#owner)라고 한다.
Actor·Spot의 owner와 위치를 여러 node가 함께 판단하는 record를
[authority](01-glossary.ko.md#authority)라고 한다. Framework가 authority와 두
Store를 사용하는 순서는 [40 Location runtime](21-location-runtime.ko.md)이
정의한다. 현재 owner와 위치를 저장하는
[Location Store](01-glossary.ko.md#location-store) provider 계약은
[41 Location Store provider](22-location-store-redis.ko.md)가 정의한다. 복원할
payload를 저장하는 provider 계약은
[42 Relocation Store provider](23-relocation-store-redis.ko.md)가 정의한다. 이
문서는 저장 형식을 반복하지 않고 host operation의 공개 순서만 정의한다.

## 2. Application이 선택하는 operation

### 2.1 Relocate mode 선택

Caller는 `Relocate`를 호출할 때 mode를 반드시 지정한다. 두 mode의 차이는 target
application version뿐이다. 이후의 queue seal, state 복원, authority 전환과 session
handoff 규칙은 같다.

| 값 | Mode | Caller가 지정하는 값 | Framework가 선택하는 target |
|---:|---|---|---|
| 0 | `PlannedMaintenance` | `TargetApplicationVersion`을 지정하지 않는다. | Source와 application version이 정확히 같은 node만 선택한다. |
| 1 | `RollingUpdate` | Source보다 큰 `TargetApplicationVersion`을 지정한다. | Caller가 지정한 application version과 정확히 같은 node만 선택한다. |

`PlannedMaintenance`의 effective target version은 source의 `ApplicationVersion`이다.
Target version을 함께 지정하면 argument error다. `RollingUpdate`는 target version이
없거나 source version 이하이면 argument error다. Framework는 이런 잘못된 조합을
runtime state와 admission을 변경하기 전에 거부한다.

Operation이 끝나야 하는 마지막 시각을
[deadline](01-glossary.ko.md#deadline)이라고 한다. 두 mode 모두 `Deadline`을
생략하면 30초를 사용한다. 명시한 값은 0보다 커야 한다.

### 2.2 Public operation

다음 .NET 선언은 공통 계약의 한 표현이다. 다른 언어의 정확한 이름과 signature는
각 언어의 interface 문서가 정의한다.

```csharp
public enum ZLinkFrameworkRelocationMode
{
    PlannedMaintenance = 0,
    RollingUpdate = 1
}

public sealed record ZLinkFrameworkRelocationOptions
{
    // 같은 version 점검인지 새 version 배포인지 선택한다.
    public required ZLinkFrameworkRelocationMode Mode { get; init; }

    // RollingUpdate에서만 source보다 큰 exact version을 지정한다.
    public long? TargetApplicationVersion { get; init; }

    // 생략하면 30초를 사용한다.
    public TimeSpan? Deadline { get; init; }
}

public readonly record struct ZLinkFrameworkRelocationResult(
    ZLinkFrameworkRelocationMode Mode,
    long TargetApplicationVersion,
    ZLinkFrameworkRelocationOutcome Outcome,
    ZLinkFrameworkRelocationReason Reason);

public interface IZLinkFrameworkRuntime
{
    // 현재 host lifecycle state와 마지막 결과를 제공한다.
    ZLinkFrameworkRuntimeStatus Status { get; }

    // Host state와 terminal result 변화를 순서대로 관찰한다.
    IAsyncEnumerable<ZLinkObservedStatus<ZLinkFrameworkRuntimeStatus>> ObserveAsync(
        CancellationToken cancellationToken = default);

    // Stateful workload를 이전하고 성공하면 Relocated 상태로 남는다.
    ValueTask<ZLinkFrameworkRelocationResult> RelocateAsync(
        ZLinkFrameworkRelocationOptions options,
        CancellationToken cancellationToken = default);

    // 새 relocation 없이 accepted work와 host resource를 정리한다.
    ValueTask<ZLinkFrameworkTerminationResult> ShutdownAsync(
        TimeSpan? deadline = null,
        CancellationToken cancellationToken = default);
}
```

Rolling update의 일반적인 호출 순서는 다음과 같다.

```csharp
var relocation = await runtime.RelocateAsync(
    new ZLinkFrameworkRelocationOptions
    {
        // N+1로 준비한 node만 target 후보로 사용한다.
        Mode = ZLinkFrameworkRelocationMode.RollingUpdate,
        TargetApplicationVersion = currentVersion + 1,
        Deadline = TimeSpan.FromSeconds(30)
    },
    cancellationToken);

if (relocation.Outcome == ZLinkFrameworkRelocationOutcome.Relocated)
{
    // Relocation 성공을 확인한 뒤 host resource를 별도로 정리한다.
    await runtime.ShutdownAsync(cancellationToken: cancellationToken);
}
```

`Relocate` 결과에는 mode와 effective target version이 항상 포함된다. Target을 찾지
못한 경우에도 caller가 어떤 조건으로 기다렸는지 확인할 수 있도록 같은 값을
보존한다. `Relocate`가 `Blocked`로 끝나면 caller는 다시 시도하거나 continuity 없이
`Shutdown`할 수 있다.

## 3. Host state와 완료 결과

Host lifecycle은 `FrameworkRuntimeState` 하나가 소유한다.

Host의 endpoint, 실행 세대와 제공 기능을 Store에 게시한 정보를
[descriptor](01-glossary.ko.md#descriptor)라고 한다. 새 application 작업을 받을
준비가 끝난 상태를 [ready](01-glossary.ko.md#ready)라고 한다.

| 값 | State | 의미 |
|---:|---|---|
| 0 | `Preparing` | Registration, bind, descriptor 검증과 recovery를 진행하며 application message를 받지 않는다. |
| 1 | `Serving` | Host가 ready 상태이며 새로운 application work를 받는다. |
| 2 | `Relocating` | 새 placement와 selection에서는 제외됐지만 아직 seal하지 않은 local unit은 message와 timer를 계속 처리한다. |
| 3 | `Relocated` | 모든 stateful object가 source dispatch에서 분리됐다. Host와 infrastructure 연결은 유지한다. |
| 4 | `Draining` | `Shutdown`이 새 admission을 닫고 이미 수락한 work와 resource를 정리한다. |
| 5 | `Stopped` | Application resource, infrastructure resource와 listener 정리가 끝났다. |
| 6 | `Error` | Startup 또는 runtime 오류 때문에 service를 제공할 수 없다. |

`IsReady`는 `Serving`에서만 true다.
Component lifecycle snapshot은 각 component의
상태를 관찰하는 정보이며 host state를 대신하지 않는다. Component별 `Drain`,
`AwaitDrained`, `Stop` 또는 일부 Mesh만 대상으로 하는 public operation은 제공하지
않는다.

```mermaid
stateDiagram-v2
    [*] --> Preparing
    Preparing --> Serving: required component 준비 완료
    Preparing --> Error: startup 오류
    Serving --> Relocating: Relocate intent 게시
    Serving --> Draining: Shutdown이 admission 봉인
    Serving --> Error: runtime 오류
    Relocating --> Serving: Blocked 뒤 source 처리 복원
    Relocating --> Relocated: 모든 relocation unit 분리
    Relocating --> Draining: Shutdown 요청
    Relocated --> Draining: Shutdown 요청
    Error --> Draining: bounded cleanup 시작
    Draining --> Stopped: resource cleanup 완료
```

첫 relocation commit 전에 실패하면 tentative 작업을 정리하고 `Serving`으로
돌아간다. Commit 뒤 실패에서도 아직 commit하지 않은 source workload의 처리를
복원할 수 있으므로 host는 `Serving`으로 돌아갈 수 있다. 이때 이미 commit한 unit은
target owner에 남는다. `Serving` 복귀가 모든 unit의 source rollback을 뜻하지 않는다.

Relocation outcome은 다음 값으로 고정한다.

| 값 | Outcome | 허용 reason | 의미 |
|---:|---|---|---|
| 0 | `Relocated` | `None` | 모든 stateful object가 source dispatch에서 분리됐다. |
| 1 | `Blocked` | `TargetUnavailable`, `StoreUnavailable`, `RelocationDisabled`, `StateIncompatible`, `DeadlineExceeded`, `RelocationFailed`, `RuntimeNotReady`, `ManualTopologyUnsupported`, `ShutdownRequested`, `OperationInProgress` | Relocation을 시작할 수 없거나 전체 workload 이전을 끝내지 못했다. |

Wire 값은 `Relocated=0`, `Blocked=1`이다. Reason은 `None=0`,
`TargetUnavailable=1`, `StoreUnavailable=2`, `RelocationDisabled=3`,
`StateIncompatible=4`, `DeadlineExceeded=5`, `RelocationFailed=6`,
`RuntimeNotReady=7`, `ManualTopologyUnsupported=8`, `ShutdownRequested=9`,
`OperationInProgress=10`이다. 정의하지 않은 outcome과 reason 조합은 protocol
오류다.

Shutdown outcome은 `Stopped=0`, `ForceStopped=1`이고 reason은 `None=0`,
`DeadlineExceeded=1`, `TeardownFailed=2`다. `ForceStopped`는 별도 host state가
아니다. Bounded teardown으로 정리를 끝낸 결과이며 host state는 `Stopped`다.
Relocation 실패는 relocation result가 소유하고 termination reason에 섞지 않는다.

## 4. Target을 선택하기 전에 확인하는 조건

`Serving`에서 시작한 `Relocate`는 host state와 application admission을 바꾸기 전에
host 전체를 한 번에 검사한다. 이때 이동 대상의 새 작업을 막거나 target 수용
공간을 미리 확보하지 않는다.

Application state를 bytes로 저장해 target에서 복원하는 방식은
[Preserve-state relocation policy](01-glossary.ko.md#preserve-state-relocation-policy)다.
Host가 현재 owner 자격을 유지하는 기간을
[owner lease](01-glossary.ko.md#owner-lease)라고 한다.

| 검사 항목 | 통과 조건 |
|---|---|
| 동시에 진행 중인 작업 | 새 object 생성, join, Instance placement, session binding과 inbound relocation이 있으면 먼저 끝낼 작업을 확정한다. |
| Local workload | 모든 MeshNode의 Actor, Spot, timer, session과 진행 중인 infrastructure operation을 확인한다. |
| 두 Store | Location Store의 현재 위치 record, 필요한 Relocation Store와 target descriptor의 owner lease를 사용할 수 있다. |
| Unit 호환성 | Application이 명시적으로 만드는 [User Spot](01-glossary.ko.md#entry-spot-user-spot과-instance-spot), 최초 message로 필요할 때 만드는 [Instance Spot](01-glossary.ko.md#entry-spot-user-spot과-instance-spot), 그 안의 Actor가 사용하는 relocation policy와 state adapter, target [factory](01-glossary.ko.md#factory), 수용 공간이 모두 호환된다. |
| Topology | Host가 사용하는 모든 service topology가 Store의 descriptor로 remote endpoint를 찾는다. 이 방식을 [automatic discovery](01-glossary.ko.md#automatic-discovery)라고 한다. |

Manual RouteMesh peer, ClientServer client endpoint, fanout subscriber endpoint 또는
descriptor를 게시하지 않는 manual fanout publisher가 하나라도 등록돼 있으면
`Blocked/ManualTopologyUnsupported`다. 현재 연결 여부가 아니라 registration을
검사한다. Automatic component가 listener 주소를 명시적으로 bind하는 설정은 manual
topology가 아니다.

Runtime이 확인하는 범위는 local registration이다. 다른 process가 Framework 밖에서
만든 connection까지 확인하지 않으므로, 참여 process 전체가 automatic discovery만
사용한다는 조건은 deployment가 보장한다.

## 5. Mode에 맞는 target을 선택한다

Framework는 다음 순서로 target을 좁힌다.

| 순서 | 조건 |
|---:|---|
| 1 | `PlannedMaintenance`는 source와 같은 application version만 남긴다. `RollingUpdate`는 caller가 지정한 version만 남기며 더 낮거나 높은 다른 version도 제외한다. |
| 2 | Source가 아니며 `Serving` 상태인 Object Server만 남긴다. |
| 3 | Application이 object를 만들도록 등록한 함수인 [factory](01-glossary.ko.md#factory), 언어 독립 object type 이름인 [stable type](01-glossary.ko.md#stable-type), relocation policy와 state adapter가 호환되는 node만 남긴다. |
| 4 | 필요한 수용 공간이 남아 있고, source에 [maintenance wave](01-glossary.ko.md#maintenance-wave)가 설정되어 있으면 값이 다른 node만 남긴다. Maintenance wave는 같은 점검 작업의 host를 구분하는 application 설정값이다. |
| 5 | 같은 시점의 descriptor 목록과 Core peer table에서 RID와 [lifecycle generation](01-glossary.ko.md#lifecycle-generation)이 모두 같은 node만 남긴다. Lifecycle generation은 같은 RID를 사용한 서로 다른 process 실행을 구분한다. 해당 peer는 `Admitted`와 `Ready`여야 한다. |

Version은 수용 공간과 placement weight보다 먼저 확인한다. 따라서 다른 version
node의 남은 공간이나 높은 weight는 선택 결과에 영향을 주지 않는다. 여러 후보에
새 작업을 배정할 상대 비중을 [weight](01-glossary.ko.md#weight)라고 한다. 조건을
만족한 target이 여러 개일 때만 이 값을 적용한다.

Descriptor가 게시됐거나 connect intent가 만들어진 것만으로 target이 ready라고
판단하지 않는다. 특정 시점의 상태를 읽기 전용으로 복사한 결과를
[snapshot](01-glossary.ko.md#snapshot)이라고 한다. Descriptor snapshot이 비어
있거나 source 자신만 포함하거나 모든 remote peer가 draining이면 target이 없는
상태다.

### 5.1 Target이 아직 없을 때

Target 탐색보다 먼저 이전할 unit이 있는지 확인한다. Source가 소유한 Actor, User Spot과
Instance Spot이 모두 없으면 옮길 것이 없으므로 target을 찾지 않고 `Relocated/None`으로
끝낸다. Relocation의 목적은 workload 이전이며, 이전할 workload가 없는 host를 target이
없다는 이유로 막지 않는다. 이 경우에도 host state 전이와 admission 종료는 다른 relocation과
같다.

이전할 unit이 있는데 요청한 version의 target이 없으면 source state와 admission을 유지한 채
deadline까지 descriptor와 Core peer table의 수렴을 기다린다. 여러 Mesh를 가진
process는 모든 Mesh에서 조건을 만족해야 한다. Deadline까지 target을 확보하지 못하면
tentative coordination을 정리하고 `Blocked/TargetUnavailable`을 반환한다.

```mermaid
sequenceDiagram
    participant Target as Replacement node
    participant Source as Source host
    participant Store as Location·Relocation Store
    participant App as Deployment orchestrator

    Target->>Store: Serving descriptor 게시
    Source->>Source: 요청한 version의 peer가 Ready인지 확인
    Source->>Store: Relocating 상태 게시
    Source->>Target: workload 복원 요청
    Target->>Store: authority 전환 완료
    Source->>Store: Relocated 상태 게시
    Source-->>App: Relocated 결과 반환
    App->>Source: Shutdown 호출
    Source->>Source: accepted work와 infrastructure 정리
    Source-->>App: Stopped 또는 ForceStopped 반환
```

`Relocated`에서는 descriptor, connection, listener와 infrastructure resource를 유지한다.
이 다이어그램은 target이 준비된 정상 흐름이다. Target이 없으면 앞 절의 deadline 규칙으로
`Blocked/TargetUnavailable`을 반환한다.

Automatic ClientServer client와 fanout subscriber는 replacement descriptor로 새
connection을 만들고 source 상태를 selection에 반영한다. Accepted work와 barrier가
남은 기존 connection은 descriptor 변화만으로 즉시 닫지 않는다.

## 6. Concurrent 호출과 cancellation

| 상황 | 결과 |
|---|---|
| Mode와 effective target version이 같은 concurrent `Relocate` | 최초 operation과 deadline을 공유한다. 뒤의 호출은 deadline을 바꾸지 않는다. |
| Mode 또는 effective target version이 다른 concurrent `Relocate` | 기다리지 않고 `Blocked/OperationInProgress`다. |
| `Blocked` 뒤 다시 `Relocate` | `Blocked`는 저장하지 않으므로 host 조건을 처음부터 다시 검사한다. |
| `Relocated`에서 다시 `Relocate` | 최초 `Relocated/None`의 mode와 effective target version을 반환한다. |
| Concurrent `Shutdown` | 같은 operation을 공유하고 terminal result를 저장한다. |
| `Stopped`에서 다시 `Shutdown` | 저장한 결과를 반환한다. 없으면 새 작업 없이 `Stopped/None`이다. |
| `Preparing`, `Error` 또는 `Stopped`에서 `Relocate` | Admission을 바꾸지 않고 `Blocked/RuntimeNotReady`다. |

Caller cancellation은 해당 waiter만 끝내며 shared operation은 취소하지 않는다.
`Shutdown`은 `Preparing`에서 startup을 중단하고 `Error`에서는 bounded cleanup을
시작한다.

## 7. Relocation unit과 실행량 제한

Framework가 서로 독립적으로 옮길 수 있는 Actor 또는 Spot 묶음을
[relocation unit](01-glossary.ko.md#relocation-unit)이라고 한다. Actor가 현재 어느 Entry Spot 또는 User Spot에 속하는 관계는
[Actor membership](01-glossary.ko.md#actor-membership)이다.

| Relocation unit | 경계 |
|---|---|
| `SpotWide` User Spot aggregate | User Spot과 새 작업을 막는 시점의 member Actor 전체 |
| Actor | Entry Spot 또는 `PerActor` User Spot에 속한 Actor 하나 |
| `PerActor` Spot authority 전환 | Target의 stateless Spot shell과 Spot-level queue authority |
| Instance Spot | Actor membership이 없는 Spot 하나 |

Entry Spot 자체는 이전하지 않는다. Source Entry Spot의 Actor는 Actor unit으로
target node의 Entry Spot에 이전한다. `PerActor` User Spot도 Spot application state를
이전하지 않고 Actor를 같은 단위로 이전한다.

Framework는 각 unit의 queue에 이동 준비 작업을 넣는다. 현재 application turn이
끝나면 동시 실행 수와 memory 제한을 모두 확보한 unit부터 이동한다. 제한 중 하나라도
확보하지 못하면 이미 확보한 제한을 반환하고 나중에 다시 시도한다. 아직 이동을
시작하지 않은 unit은 message와 timer를 계속 처리한다.

User Spot 전체가 하나의 실행 gate를 사용하는
[`SpotWide`](01-glossary.ko.md#user-spot-execution-mode)는 현재 turn이 끝나면 준비된다.
Actor마다 실행 lane을 나누는 `PerActor`와 Entry Spot은 Actor별 current turn이 끝난
unit부터 준비한다. 서로 다른 Actor는 동시에 이전할 수 있지만 Actor 하나의 queue
순서는 유지한다. `PerActor` Spot lane은 authority 전환 때만 잠시 막고 member Actor
전체가 같은 시점에 준비될 때까지 기다리지 않는다.

`SpotWide` factory의
[`Spot relocation readiness mode`](01-glossary.ko.md#spot-relocation-readiness-mode)가
`AnyTurnBoundary`이면 위의 일반 turn 경계를 사용한다. 기본값도 이 mode다.
`ApplicationSignaled`이면 preflight, target과 permit 준비를 끝낸 relocation만
application이 `RelocationReady().Defer()`로 등록한 경계를 소비한다. 준비된
relocation이 없으면 source에서 `Continued` completion callback을 다음 application
turn으로 실행한다.

경계를 소비한 relocation이 commit 전에 취소되면 source queue를 복원한 뒤
`Continued` callback을 실행한다. Commit 뒤에는 같은 target runtime의 object execution
queue에서 `Relocated` callback을 첫 application turn으로 실행한다.
Callback이 완료되기 전에는 복원한 기존 작업, source에서 relay한 message와 target에
직접 도착한 message의 application handler를 실행하지 않는다. Callback 자체는 Spot
interface의 기본 no-op 구현이며 override는 retry-safe해야 한다.

Application은 Location 설정에서 다음 다섯 상한을 양수로 지정할 수 있다. 설정을
바꾸면 새 relocation admission부터 적용한다. 아래 표는 공통 의미, .NET public
member와 기본값을 함께 보여 준다. 다른 언어의 이름은 해당 언어 interface 문서가
정한다. [.NET Location 설정](server/languages/dotnet/interfaces/08-location-maintenance.ko.md#2-location-option)에서
`ConfigureLocations()` 등록 위치와 정확한 선언을 확인할 수 있다.

| 제한하는 항목 | .NET public member | 기본 상한 |
|---|---|---:|
| Source에서 동시에 이전하는 unit | `MaxActiveOutboundRelocations` | 64 |
| Target에서 동시에 복원하는 unit | `MaxActiveInboundRelocations` | 64 |
| Process가 relocation을 위해 보관하는 encoded payload | `MaxRelocationPayloadInFlightBytes` | 256 MiB |
| 동시에 실행하는 `Capture` callback | `MaxConcurrentRelocationCaptures` | 8 |
| 동시에 실행하는 `Restore` callback | `MaxConcurrentRelocationRestores` | 8 |

Callback 동시 실행 수는 unit 수와 payload byte 제한에서 따로 계산한다. 이동을
시작하기 전에 `PreserveStateWith` object
하나의 최대 64 MiB와 queue, journal, timer, 목록 정보와 framing의 최대 저장 크기를
합산해 memory를 확보한다. Application에서 예상 크기를 받지 않는다. `Capture` 뒤
실제 크기가 작으면 확보량을 줄일 수 있지만 늘릴 수 없다. Adapter가 64 MiB를
넘으면 `Blocked/StateIncompatible`다.

256 MiB보다 큰 `SpotWide` User Spot 전체는 다른 payload 이동이 없을 때 하나만 진행한다. 실제
크기가 작아져도 해당 이동이 memory 사용을 끝낼 때까지 다른 payload 이동을
시작하지 않는다. Actor 하나와 Instance Spot은 256 MiB 제한 안에서만 진행한다.

### 7.1 Relocation unit별 서비스 중단 시간 목표

| 대상 | 측정 unit |
|---|---|
| Entry Spot Actor | Actor 하나 |
| `PerActor` User Spot | Spot direct admission 하나와 Actor 각각 |
| `SpotWide` User Spot | Spot과 member Actor를 포함한 aggregate 하나 |
| Instance Spot | Spot 하나 |

측정은 source admission seal이 적용된 시점에 시작한다. Target reservation, inbound
unit permit, Restore 실행 슬롯, 예상 payload memory permit, 현재 turn과 application
safe point를 확보하기 전의 대기 시간은 제외한다. 이 준비는 source admission을
막기 전에 끝내야 한다. Seal 뒤에 실행하는 Capture, encoding, Store 기록, authority
변경, target Restore, queue·timer 복원은 모두 측정에 포함한다. Target이 held work를
먼저 복원하고 message 처리를 시작할 수 있다고 source에 알리면 측정을 끝낸다.

각 unit은 기본 1초 이내를 목표로 한다. 1초는 timeout이나 correctness 조건이
아니다. 초과해도 relocation을 취소하거나 source로 rollback하지 않는다.
Framework는 같은 operation을 target message 처리 시작까지 계속하고 warning과
`zlink.relocation.interruption` histogram을 기록한다. Source application close는 target이
복원 완료와 처리 시작을 알린 뒤 수행한다.

Host operation deadline이 끝나면 새 unit relocation을 시작하지 않는다. 이미
시작한 unit은 owner 변경 전 안전한 abort를 수행한다. Owner 변경 뒤에는 같은 target
process가 실행 중일 때만 현재 단계를 끝까지 처리한다. Target process가 종료되면 해당
unit을 unavailable 상태로 두고 host relocation은 실패한다. 아직 source에 남은 unit이
있으면 host는 `Relocated`가 되지 않는다.

## 8. Unit 하나를 이전하는 순서

Host relocation을 시작하면 Application이 Actor나 Spot마다 별도 이동 API를 호출하지
않아도 Framework가 source host의 workload를 target node로 옮긴다. Actor와 Spot의 ID는
유지되며, 이동이 끝난 대상은 target에서 기존 queue 순서대로 message 처리를 계속한다.
Framework가 한 번에 옮기는 Actor 하나 또는 Spot 묶음을
[relocation unit](01-glossary.ko.md#relocation-unit)이라고 한다.

### 8.1 누가 무엇을 하는가

| 주체 | 하는 일 |
|---|---|
| Application | Host의 `Relocate`를 호출한다. `ApplicationSignaled`를 선택한 `SpotWide` User Spot만 안전한 이동 시점을 `RelocationReady().Defer()`로 알린다. |
| Source runtime | 현재 실행 중인 작업을 끝내고 새 작업을 잠시 보관한다. State와 아직 실행하지 않은 작업을 저장한 뒤 target에 복원을 요청한다. |
| Target runtime | 같은 ID를 사용하는 Actor나 Spot을 만들고 저장된 state와 작업을 복원한다. 위치 변경이 확정되면 message 처리를 시작한다. |
| Location Store | 현재 어느 node가 Actor나 Spot을 처리하는지 기록한다. 여러 값을 함께 바꿔야 할 때는 모두 바꾸거나 하나도 바꾸지 않는다. |
| Relocation Store | Application state와 아직 실행하지 않은 message·timer를 target이 읽을 때까지 보관한다. |

### 8.2 모든 Actor와 Spot이 따르는 공통 순서

1. Source runtime은 target에서 Actor나 Spot을 만들 수 있는지, 필요한 memory와 동시에
   실행할 수 있는 수가 남아 있는지 먼저 확인한다. 이 확인이 끝나기 전에는 source의
   새 작업을 막지 않는다.
2. 준비가 끝나면 현재 실행 중인 handler와 timer callback까지만 완료한다. 그 뒤 도착한
   message와 아직 시작하지 않은 timer는 source runtime의 크기가 제한된 ingress hold에
   보관한다. 이 hold는 source에만 두는 relocation용 임시 저장 공간이다.
3. Source runtime은 아직 실행하지 않은 message, timer 정보와 application state를
   Relocation Store에 저장한다. `PreserveStateWith`를 선택했다면 application adapter의
   `Capture`가 반환한 state도 함께 저장한다.
4. Source runtime은 target runtime에 Restore 요청을 먼저 보낸다. Target dispatcher는
   다음 packet을 dispatch하기 전에 object 종류, ID와 `ObjectGeneration`에 대한
   [relocation temporary queue](01-glossary.ko.md#relocation-temporary-queue)를 등록한다. 이후
   해당 object로 들어오는 message는 실제 instance를 찾지 않고 temporary queue에 넣는다.
5. Target은 Actor나 Spot을 만들고 application state를 Restore한다. 저장된 기존 작업과 timer는
   아직 실행하지 않는다. Source runtime은 ingress hold의 message와 이후 이전 route로 들어오는
   message를 target으로 계속 relay한다. Target dispatcher는 relay message도 temporary queue에 넣는다.
6. Target이 Restore를 마치면 Location Store에서 Actor나 Spot을
   처리할 node를 source에서 target으로 바꾼다. Actor가 Spot에 속한다면 Actor를 처리할 node와
   Actor가 속한 Spot도 함께 바꾼다. 둘 중 하나라도 바꿀 수 없으면 어느 값도 변경하지 않는다.
7. `ApplicationSignaled`인 `SpotWide` User Spot은 owner 변경 뒤 target에서
   `OnRelocationReadyCompleted(Relocated)`를 호출한다. 그 밖의 필요한 lifecycle 작업도 이
   단계에서 끝낸다.
8. Target은 저장된 기존 작업과 timer를 실제 object queue에 먼저 넣고 temporary queue의
   작업을 그 뒤에 옮긴다. 실제 queue가 이 작업을 모두 수락하면 temporary queue 등록을
   제거하고 기존 dispatch 경로로 전환한다. 이 전환이 끝난 뒤 queue 순서대로 application
   message를 처리하고 source에 전환 완료를 알린다. Source는 이 알림을 받을 때까지 ingress
   hold 원본을 유지한다.
9. 이동한 Actor가 Session에 bind되어 있으면 target runtime이 Session owner에 현재 Actor
   위치를 바꾸라는 `sessionActorLocationUpdateReqMsg`를 send한다. 이 응답을 기다리는 동안에도
   Target Actor는 message를 처리한다. 응답이 없을 때의 재전송 간격은
   [Session–Actor dispatch §5.1](20-session-actor-dispatch.ko.md#51-session-actor-위치-갱신-message)이
   정의한다.
10. User Spot과 Instance Spot의 이전 위치에 남은 instance에는 Location Store의 위치 변경 뒤
   `OnClosing(RelocationOut)`을 호출한다. Entry Spot 자체는 이동하지 않으므로 Entry Spot의
   closing callback은 호출하지 않는다.

Dispatch 전환은 atomic해야 한다. 전환 전에 들어온 message는 temporary queue에 남고, 전환
뒤 들어온 message는 실제 object queue로 바로 들어간다. 전환 중인 message가 두 queue에
중복으로 들어가거나 어느 queue에도 들어가지 않는 상태는 허용하지 않는다.

Relocation unit 하나의 temporary queue는 최대 1,024 record와 16 MiB다. 이 한도를 넘는
request는 `Unavailable`, one-way operation은 moving drop으로 끝낸다. Framework는 한도를
늘리기 위해 같은 object에 temporary queue를 추가로 만들지 않는다.

```mermaid
sequenceDiagram
    participant Source as Source runtime
    participant Dispatch as Target dispatcher
    participant TempQueue as Relocation temporary queue
    participant ObjectQueue as Object execution queue
    participant Object as Target Actor or Spot
    participant LocationStore as Location Store

    Source->>Dispatch: Restore 요청
    Dispatch->>TempQueue: temporary queue 등록
    Dispatch->>Object: factory 실행과 application state Restore
    Source->>Dispatch: ingress hold message relay
    Dispatch->>TempQueue: temporary queue에 message 추가
    Dispatch->>LocationStore: 현재 처리 node를 target으로 변경
    Dispatch->>ObjectQueue: 저장된 기존 작업과 timer 추가
    Dispatch->>ObjectQueue: temporary queue 작업 이동
    Dispatch->>TempQueue: 등록 제거 후 기존 dispatch로 전환
    Dispatch-->>Source: dispatch 전환 완료 알림
    ObjectQueue->>Object: queue 순서대로 작업 처리
```

이 다이어그램은 정상 경로만 보여준다. Commit 전에는 source가 ingress hold 원본을 유지한다.
Restore나 owner 변경이 실패하면 target temporary queue를 실행하지 않고 폐기하며 source가
원래 queue에 작업을 되돌린다. Commit 뒤에는 같은 target process가 temporary queue를 실제
queue로 옮기는 남은 절차를 계속한다. 같은 Restore request를 다시 받으면 temporary queue와 Restore를 다시
만들지 않고 기존 진행 상태를 사용한다. 이전 target attempt나 다른 `ObjectGeneration`의
temporary queue에는 message를 넣지 않는다.

| Policy | 처리 |
|---|---|
| `DisableRelocation` | 해당 object가 남아 있으면 `Blocked/RelocationDisabled`다. |
| `RecreateOnRelocation` | 같은 object ID로 target factory를 실행한다. Application state는 옮기지 않지만 아직 끝나지 않은 Framework 작업은 옮긴다. |
| `PreserveStateWith` | Adapter가 반환한 bytes를 저장하고 target factory instance에 `Restore`한다. Application이 bytes의 format, version과 migration을 관리한다. |

Framework는 별도 state contract ID나 generic state type을 추가하지 않는다.

### 8.3 Entry Spot에 속한 Actor

Entry Spot instance는 Object Server lifecycle에 속하므로 다른 node로 옮기지 않는다.
Source Entry Spot에 속한 Actor만 각각 독립된 relocation unit으로 옮긴다. Target runtime은
target node가 startup에서 이미 만든 Entry Spot에 Actor를 복원한다. 이 이동은 application이
요청한 join이 아니므로 target Entry Spot의 `OnJoinedActor`, source Entry Spot의
`OnLeaveActor`와 `OnActorJoin`을 호출하지 않는다.

```mermaid
sequenceDiagram
    participant SourceRuntime as Source runtime
    participant SourceActor as Source Actor
    participant RelocationStore as Relocation Store
    participant TargetRuntime as Target runtime
    participant TargetTemp as Actor temporary queue
    participant TargetQueue as Target Actor queue
    participant TargetEntry as Target Entry Spot
    participant TargetActor as Target Actor
    participant LocationStore as Location Store
    participant SessionOwner as Session owner

    SourceRuntime->>SourceActor: current turn 완료 뒤 새 message 보관
    SourceRuntime->>RelocationStore: Actor state와 queue·timer 저장
    SourceRuntime->>TargetRuntime: Actor restore 요청 먼저 전송
    TargetRuntime->>TargetTemp: Actor temporary queue 등록
    TargetRuntime->>TargetActor: factory 실행과 state Restore
    SourceRuntime->>TargetRuntime: ingress hold message relay
    TargetRuntime->>TargetTemp: temporary queue에 message 추가
    TargetRuntime->>LocationStore: Actor의 현재 node와 소속 Entry Spot을 함께 변경
    LocationStore-->>TargetRuntime: 위치 변경 완료
    TargetRuntime->>TargetQueue: 저장된 queue·timer 추가
    TargetRuntime->>TargetQueue: temporary queue 작업 이동
    TargetRuntime->>TargetTemp: 등록 제거 후 기존 dispatch로 전환
    TargetQueue->>TargetActor: queue 순서대로 message 처리
    opt bound session이 있으면
        TargetRuntime-)SessionOwner: sessionActorLocationUpdateReqMsg send
        SessionOwner-)TargetRuntime: sessionActorLocationUpdateResMsg send
        Note over TargetRuntime,SessionOwner: 응답이 없으면 정해진 간격으로 같은 ReqMsg 재전송
    end
    Note over SourceRuntime,TargetEntry: Entry Spot의 join·leave·closing callback은 호출하지 않음
```

### 8.4 PerActor User Spot

`PerActor` User Spot은 Spot message를 처리할 node와 각 Actor를 처리할 node를 따로
바꾼다. 따라서 일부 Actor가 아직 source에 있어도 이미 이동한 Actor와 새 Spot message는
target에서 처리할 수 있다.

1. Target runtime은 source와 같은 SpotId와 ObjectGeneration을 사용하는 빈 Spot instance를
   만든다. Location Store의 현재 위치를 바꾸기 전에는 이 instance를 외부 조회 결과로
   반환하지 않는다.
2. Source runtime은 현재 Spot handler와 진행 중인 Actor Create·Join을 끝낸다. 그 뒤
   도착한 Spot message는 source에서 잠시 보관한다.
3. Target runtime은 Spot relocation temporary queue를 등록한다. Source runtime은 보관한 Spot
   message와 이후 이전 route로 들어오는 message를 이 queue로 계속 relay한다.
4. Target의 Spot 준비가 끝나면 Location Store에서 Spot message를 처리할 node를 source에서
   target으로 바꾼다. 보관한 Spot message를 실제 Spot queue로 옮기고 temporary queue 등록을
   제거한 뒤 새 `ToSpot`, Actor Create와 Join을 기존 dispatch 경로로 처리한다.
5. Source member Actor는 각자 현재 turn을 끝낸 뒤 Actor unit으로 target에 이전한다.
   이전되지 않은 Actor의 `ToActor`는 source, 이전된 Actor의 `ToActor`는 target으로
   보낸다.
6. 마지막 Actor와 source에 보관했던 message를 모두 target에 전달하면 source Spot에
   `OnClosing(RelocationOut)`을 호출한다.

Framework는 이동 중에 임시 SpotId를 만들지 않는다. Location Store가 target을 현재
Spot message 처리 node로 기록한 뒤에는 source Spot이 새 `ToSpot`, Create와 Join을
처리하지 않는다. Source Spot은 아직 이동하지 않은 Actor와 이동 중 message 전달만
계속한다.

```mermaid
sequenceDiagram
    participant SourceRuntime as Source runtime
    participant SourceSpot as Source User Spot
    participant RelocationStore as Relocation Store
    participant TargetRuntime as Target runtime
    participant TargetSpot as Target User Spot
    participant TargetSpotTemp as Target Spot temporary queue
    participant TargetSpotQueue as Target Spot queue
    participant TargetActorTemp as Target Actor temporary queue
    participant TargetActorQueue as Target Actor queue
    participant LocationStore as Location Store
    participant SourceActor as Source Actor
    participant TargetActor as Target Actor
    participant SessionOwner as Session owner

    SourceRuntime->>TargetRuntime: 같은 SpotId의 빈 Spot 준비 요청
    TargetRuntime->>TargetSpot: 외부에 아직 공개하지 않는 Spot 생성
    SourceRuntime->>SourceSpot: 현재 Spot handler 종료 뒤 새 message 보관
    TargetRuntime->>TargetSpotTemp: Spot temporary queue 등록
    SourceRuntime->>TargetRuntime: 보관한 Spot message relay
    TargetRuntime->>TargetSpotTemp: Spot message 보관
    TargetRuntime->>LocationStore: Spot message를 처리할 node를 target으로 변경
    LocationStore-->>TargetRuntime: target이 새 Spot message를 처리함
    TargetRuntime->>TargetSpotQueue: temporary queue의 Spot message 이동
    TargetRuntime->>TargetSpotTemp: 등록 제거 후 기존 dispatch로 전환
    TargetRuntime->>TargetSpot: ToSpot·Create·Join 처리 시작
    loop member Actor마다 독립적으로
        SourceRuntime->>SourceActor: 현재 handler 종료 뒤 state·queue·timer 저장
        SourceRuntime->>TargetRuntime: Actor restore 요청 먼저 전송
        TargetRuntime->>TargetActorTemp: Actor temporary queue 등록
        TargetRuntime->>TargetActor: factory 실행과 state Restore
        SourceRuntime->>TargetRuntime: Actor message relay
        TargetRuntime->>TargetActorTemp: Actor message 보관
        TargetRuntime->>LocationStore: Actor를 처리할 node를 target으로 변경
        TargetRuntime->>TargetActorQueue: 저장된 queue·timer와 temporary 작업 이동
        TargetRuntime->>TargetActorTemp: 등록 제거 후 기존 dispatch로 전환
        TargetActorQueue->>TargetActor: queue 순서대로 message 처리
        opt bound session이 있으면
            TargetRuntime-)SessionOwner: sessionActorLocationUpdateReqMsg send
            SessionOwner-)TargetRuntime: sessionActorLocationUpdateResMsg send
        end
    end
    SourceRuntime->>SourceSpot: 마지막 Actor와 message 전달 뒤 OnClosing(RelocationOut)
```

Target에 만든 빈 Spot은 source Spot의 application field를 복원하지 않으므로 Spot
relocation adapter를 호출하지 않는다. Member Actor는 각자의 relocation policy와 Actor
adapter를 사용한다. Session 위치
갱신 응답은 각 Actor의 처리나 다음 Actor relocation을 막지 않는다.

### 8.5 SpotWide User Spot

`SpotWide` User Spot은 Spot과 새 작업을 막은 시점의 member Actor 전체를 하나의 이동
작업으로 옮긴다. Actor 하나라도 relocation policy,
state adapter 또는 target의 수용 공간 조건을 만족하지 못하면 Location Store를 바꾸지
않고 전체 이동을 중단한다. 이 이동을 구분하는 ID는 0이 아닌 128-bit 값이다.

Target은 Spot과 모든 member Actor를 같은 relocation temporary queue group에 등록한다. 각
record는 실제 target Spot 또는 Actor identity를 보존한다. 모든 participant의 Restore,
aggregate owner 변경과 `OnRelocationReadyCompleted`가 끝난 뒤 record를 실제
Spot queue와 Actor queue로 나눠 옮긴다. Participant 하나가 실패하면 temporary queue의 어느
작업도 실행하지 않고 group 전체를 폐기한다.

User Spot에 속한 Actor 총수에는 1,024개 상한을 두지 않는다. Framework는 이동 대상
목록을 Location Store의 여러 페이지로 나눈다. 한 페이지에는 최대 1,024개를
기록하며, encoded page 하나의 크기는 최대 1 MiB다. 예를 들어 Actor가 2,500개이면
최소 세 페이지에 나눠 기록한다. Framework는 전체 Actor 수와 각 페이지 내용이 처음
저장한 목록과 일치하는지 확인한다. 모두 일치할 때만 User Spot과 모든 Actor를 처리할
node를 source에서 target으로 한 번에 바꾼다. 중간에 충돌하면 일부 Actor의 위치만
바꾸지 않는다. 처음 읽은 Store version이 그대로일 때만 모두 바꾸거나 아무것도
바꾸지 않는 이 방식을 [CAS](01-glossary.ko.md#compare-and-set)라고 한다.

```mermaid
sequenceDiagram
    participant SourceRuntime as Source runtime
    participant SourceSpot as Source User Spot
    participant RelocationStore as Relocation Store
    participant TargetRuntime as Target runtime
    participant TargetTemp as Relocation temporary queue group
    participant TargetQueues as Target Spot and Actor queues
    participant TargetSpot as Target User Spot
    participant TargetObjects as Target Spot and Actors
    participant LocationStore as Location Store
    participant SessionOwner as Session owner

    SourceRuntime->>SourceSpot: 현재 handler 종료 뒤 Spot과 모든 Actor의 새 작업 보관
    SourceRuntime->>RelocationStore: Spot·Actor state와 queue·timer 전체 저장
    SourceRuntime->>TargetRuntime: Spot과 모든 Actor의 restore 요청 먼저 전송
    TargetRuntime->>TargetTemp: Spot과 모든 Actor temporary queue group 등록
    TargetRuntime->>TargetObjects: Spot과 모든 Actor 생성·state Restore
    SourceRuntime->>TargetRuntime: Spot·Actor message relay
    TargetRuntime->>TargetTemp: target identity와 함께 message 보관
    TargetRuntime->>LocationStore: Spot과 모든 Actor를 처리할 node를 한 번에 target으로 변경
    LocationStore-->>TargetRuntime: 전체 위치 변경 완료
    opt application-signaled 경계를 사용했으면
        TargetRuntime->>TargetSpot: OnRelocationReadyCompleted(Relocated)
    end
    TargetRuntime->>TargetQueues: 저장된 queue·timer를 먼저 추가
    TargetRuntime->>TargetQueues: temporary 작업을 target별 queue로 이동
    TargetRuntime->>TargetTemp: group 제거 후 기존 dispatch로 전환
    TargetQueues->>TargetObjects: queue 순서대로 message 처리
    loop bound Actor마다
        TargetRuntime-)SessionOwner: sessionActorLocationUpdateReqMsg send
        SessionOwner-)TargetRuntime: sessionActorLocationUpdateResMsg send
    end
    SourceRuntime->>SourceSpot: OnClosing(RelocationOut)
```

Member Actor의 `OnActorJoin`, `OnJoinedActor`와 `OnLeaveActor`는 호출하지 않는다. Bound
Session 위치 갱신은 Spot과 Actor가 message 처리를 시작한 뒤 Actor별로 진행하며, 한
Session owner의 응답이 다른 Actor나
Spot의 처리를 막지 않는다.

### 8.6 Instance Spot

Instance Spot은 Actor를 포함할 수 없으므로 Spot 하나가 relocation unit이다. Source의
현재 handler가 끝나면 direct message와 timer를 보관한다. Target runtime은 같은 SpotId로
Instance Spot을 만들고, `PreserveStateWith`이면 저장한 application state를 `Restore`로
복원한다. Location Store가 target을 현재 처리 node로 기록하면 target은 복원한 queue와
timer를 처리한다. Instance Spot에는 Actor가 없으므로 Actor 위치나 Session binding을
갱신하지 않는다.

```mermaid
sequenceDiagram
    participant SourceRuntime as Source runtime
    participant SourceSpot as Source Instance Spot
    participant RelocationStore as Relocation Store
    participant TargetRuntime as Target runtime
    participant TargetTemp as Target Spot temporary queue
    participant TargetQueue as Target Spot queue
    participant TargetSpot as Target Instance Spot
    participant LocationStore as Location Store

    SourceRuntime->>SourceSpot: 현재 handler 종료 뒤 새 direct message·timer 보관
    SourceRuntime->>RelocationStore: Spot state와 queue·timer 저장
    SourceRuntime->>TargetRuntime: Instance Spot restore 요청 먼저 전송
    TargetRuntime->>TargetTemp: Instance Spot temporary queue 등록
    TargetRuntime->>TargetSpot: factory 실행과 state Restore
    SourceRuntime->>TargetRuntime: direct message relay
    TargetRuntime->>TargetTemp: temporary queue에 message 보관
    TargetRuntime->>LocationStore: Instance Spot을 처리할 node를 target으로 변경
    LocationStore-->>TargetRuntime: 위치 변경 완료
    TargetRuntime->>TargetQueue: 저장된 queue·timer와 temporary 작업 이동
    TargetRuntime->>TargetTemp: 등록 제거 후 기존 dispatch로 전환
    TargetQueue->>TargetSpot: queue 순서대로 message 처리
    SourceRuntime->>SourceSpot: OnClosing(RelocationOut)
```

Host relocation은 source에 이미 존재하는 Instance Spot만 옮긴다. Source에 없는 Instance
Spot을 첫 message로 새로 만드는 [cold activation](01-glossary.ko.md#cold-activation)은
시작하지 않는다.

### 8.7 이동 중 호출하지 않는 callback

Entry Spot과 `PerActor` User Spot의 Actor relocation은 application이 요청한 join이나
leave가 아니다. 따라서 `OnActorJoin`, `OnJoinedActor`와 `OnLeaveActor`를 호출하지 않는다.
Framework가 Actor state, 실행 전 queue와 timer를 옮기고 현재 처리 node를 바꾼다.

`SpotWide` User Spot도 각 Actor가 어느 Spot에 속하는지 바꾸지 않고 처리 node만 바꾸므로 member
Actor의 join·joined·leave callback을 호출하지 않는다. `ApplicationSignaled`를 사용했다면
target에서 `OnRelocationReadyCompleted(Relocated)`만 먼저 호출한다.

User Spot과 Instance Spot의 source instance에는 Location Store의 위치 변경 뒤
`OnClosing(RelocationOut)`을 호출한다. Entry Spot instance는 이동하지 않으므로 Entry Spot의
closing callback을 호출하지 않는다. Instance Spot에는 Actor가 없으므로 Actor lifecycle
callback 자체가 없다.

Cross-node Actor join도 같은 policy와 adapter를 사용하지만 정확한 lifecycle은
[23 Spot Actor](15-spot-actor.ko.md)가 소유한다. Same-node join은 adapter를 호출하지
않는다. Instance Spot maintenance relocation은 source에 없는 Instance Spot을 새로 만들지
않는다.

### 8.8 중간에 실패하면 어느 위치를 유지하는가

위 sequence diagram은 위치 변경이 성공하는 정상 경로만 보여준다. Location Store에서
현재 처리 node를 바꾸기 전에 실패하면 source가 계속 message를 처리한다. Framework는 target에
만든 instance를 외부에 공개하지 않는다. Commit 전에는 source가 ingress hold record의 실행
소유권을 유지하므로 target temporary queue를 폐기하고 source의 message와 timer를 원래 queue에
되돌린다. Target은 temporary queue의 record로 request의 terminal 결과를 만들거나 one-way message를
실행하지 않는다.

Location Store가 target을 현재 처리 node로 기록한 뒤에는 source로 되돌리지 않는다. Target
runtime이 계속 실행 중이면 deadline 안에서 실패한 단계를 다시 시도할 수 있다. Source나
target process가 종료되면 다른 runtime이 이 relocation을 이어받지 않는다. Commit 뒤 target이
종료되면 source로 rollback하지 않고 해당 object를 unavailable 상태로 둔다. 이후 자동 복구는
계약에 포함하지 않는다. Session 위치 갱신 응답이 없어도 Actor 이동은 취소하지 않고,
실행 중인 target runtime만 `sessionActorLocationUpdateReqMsg`를 다시 보낸다. Application이
관찰하는 정확한 실패 결과는 [§10 Relocate 완료와 실패](#10-relocate-완료와-실패)가 정의한다.

## 9. 대기 중인 message, timer와 session을 옮긴다

같은 ID로 object를 삭제한 뒤 다시 만들었는지 구분하는 번호를
[ObjectGeneration](01-glossary.ko.md#objectgeneration)이라고 한다. Message나
request를 중복 처리하지 않도록 operation 하나를 구분하는 값은
[operation identity](01-glossary.ko.md#operation-identity)다.
이전하는 동안 source가 새 message를 임시 보관하는 구간을
[relocation ingress hold](01-glossary.ko.md#relocation-ingress-hold)라고 한다.

| Resource | 이동 규칙 |
|---|---|
| 새 작업 차단 뒤 도착한 message | Source는 최대 1,024 record와 저장 크기 16 MiB까지 임시 보관한다. Owner 변경이 성공하면 operation identity와 ObjectGeneration을 유지해 target에 전달한다. 변경을 취소하면 도착 순서대로 source queue에 되돌린다. |
| 임시 보관 한도 초과 | Request는 `Unavailable`, one-way operation은 moving drop으로 끝난다. Framework는 새 operation identity를 만들어 자동 재제출하지 않는다. |
| `SpotWide`·Instance Spot timer | Runtime handle과 continuation은 이전하지 않는다. Logical registration, 다음 실행 시각과 pending tick을 이전하며 target이 queue 순서에 맞춰 자동 복원한다. Application은 timer를 중복 capture하거나 restore에서 다시 등록하지 않는다. |
| Entry·`PerActor` Actor timer | Actor queue와 함께 Actor owner로 이전한다. Spot-level application timer는 이전하지 않으며 유지해야 하는 schedule은 application의 외부 state에서 관리한다. |
| Actor에 연결된 session | Request·reply와 push를 같은 연결에서 교환하는 [STREAM session](01-glossary.ko.md#stream-session)의 physical connection은 유지한다. 같은 ObjectGeneration에서 target runtime이 `sessionActorLocationUpdateReqMsg`를 send하여 해당 Actor의 [binding route](01-glossary.ko.md#binding-route)와 bound-session current Actor location snapshot을 target MeshName·NodeRid로 바꾼다. 응답은 별도의 `sessionActorLocationUpdateResMsg`로 받으며, 응답을 기다리는 동안에도 Target Actor는 message를 처리한다. ActorId·ObjectGeneration은 유지하며, relocation 대상에 포함되지 않은 다른 Actor의 route와 location snapshot은 바꾸지 않는다. |

이전 owner로 늦게 도착한 message를 target에 전달할 때도 operation identity와
authority generation을 유지한다. Session 위치 갱신 응답 전이라도 Message Follow route는
`MessageFollowDuration` 안에서만 이전 route로 도착한 packet을 Target Actor에 전달한다. 이전 generation의 packet과 reply는
거부한다. 같은 ActorId로 새로 만든 Actor는 application이 다시 bind해야 한다. 자세한
route 변경 순서는
[31 Session Actor dispatch](20-session-actor-dispatch.ko.md#5-actor-relocation-route-barrier)가 정의한다.

Instance Spot의 `Close`와 relocation은 같은 authority commit에서 순서를 정한다.
`Closing`이 먼저면 close를 완료하고 이전하지 않는다. Relocation이 먼저면 늦은
`Close`는 moving 결과이며 자동 재제출하지 않는다.

## 10. Relocate 완료와 실패

모든 unit이 source dispatch에서 분리되고 relocation ingress hold가 commit 또는 abort로
정리되면 host는 `Relocated`로 전환하고 `Relocated/None`을 반환한다. Descriptor lease,
listener, peer connection과 raw transport resource는 이때 정리하지 않는다.

Operation이 deadline까지 완료 조건을 만족하지 못한 결과를
[`DeadlineExceeded`](01-glossary.ko.md#deadlineexceeded)라고 한다.

| 발생 시점과 원인 | 결과 |
|---|---|
| 요청한 application version의 target이 deadline까지 준비되지 않는다. | `Blocked/TargetUnavailable` |
| Store 읽기, 쓰기 또는 owner lease 확인이 첫 owner 변경 전에 실패한다. | Owner를 바꾸지 않은 임시 record를 정리하고 `Blocked/StoreUnavailable` |
| `DisableRelocation` policy가 남아 있다. | `Blocked/RelocationDisabled` |
| Version, type 또는 state adapter가 호환되지 않거나 허용한 재시도에서 `Capture`와 `Restore`가 모두 실패한다. | `Blocked/StateIncompatible` |
| Framework가 deadline 때문에 callback을 취소하거나 owner 변경 전 작업이 deadline을 넘는다. | `Blocked/DeadlineExceeded` |
| 첫 owner 변경 뒤 같은 target runtime이 relocation을 끝내지 못한다. | Source로 rollback하지 않고 해당 object를 unavailable 상태로 두며 `Blocked/RelocationFailed` |

첫 owner 변경 전 실패는 임시 record를 정리하고 source authority와 queue가 새
작업을 다시 받게 한다. 첫 owner 변경 뒤 실패는 Location Store에 기록된 현재
owner를 유지한다. 이미 바꾼 owner와 Actor membership을 source로 되돌리지 않으며,
다른 target으로 자동 이전하지도 않는다. 아직 옮기지 않은 source workload만 다시
처리한 뒤 host를 `Serving`으로 전환한다.

Location Store가 가리키는 payload가 영구적으로 없거나 checksum 또는 이동 대상
목록의 내용 확인값이 다르면 다시 시도해도 복구할 수 없는
`DataLost`다. 이전 payload를 추측하거나 source로 되돌리지 않는다.
판정과 복구는
[42 Relocation Store](23-relocation-store-redis.ko.md)가 정의한다.

일부 MeshNode의 `Relocating` descriptor 기록 결과를 확인하지 못하면 시도한 모든
descriptor를 `Serving`으로 되돌린다. 모든 변경 취소를 확인해야
`Blocked/StoreUnavailable`이다. 하나라도 확인할 수 없으면 새 작업을 받지 않고
정해진 최대 시간 동안 정리한 뒤 `ForceStopped/TeardownFailed`로 끝낸다.

## 11. Shutdown과 Relocate의 경쟁

`Shutdown`은 target, policy, capacity 또는 Relocation Store 부재로 차단되지 않는다.
새 application 작업을 받지 않도록 바꾸는 동작을
[admission seal](01-glossary.ko.md#admission-seal)이라고 한다. Shutdown은 먼저 host
전체에 admission seal을 적용한다. Stateful workload의 연속성을 보장하지 않으며
다음 순서로 정해진 시간 안에 완료한다.

1. Host를 `Draining`으로 바꾸고 신규 application admission과 relocation reservation을
   닫는다.
2. `Draining` descriptor를 게시해 새 selection과 placement에서 제외한다.
3. 이미 수락한 handler, request completion, relocation unit과 session barrier를
   deadline까지 처리한다.
4. 새 object relocation은 시작하지 않는다. Actor membership과 local instance가
   유효한 상태에서 모든 Entry, User, Instance Spot에 `HostShutdown` closing context를
   전달한다. Actor별 closing callback은 호출하지 않는다.
5. Spot callback 뒤 local Actor와 Spot scope, owner record, descriptor, listener와
   transport를 순서대로 정리한다.
6. Deadline 안에 끝나면 `Stopped/None`, 끝나지 않으면 bounded teardown 뒤
   `ForceStopped/DeadlineExceeded` 또는 `ForceStopped/TeardownFailed`로 끝난다.

| 먼저 확정된 operation | 처리 |
|---|---|
| `Shutdown`의 admission seal | Target에 확보한 수용 공간을 반환하고 기다리던 Relocate 호출을 `Blocked/ShutdownRequested`로 끝낸다. |
| `Relocating` publication | 현재 unit만 terminal 상태까지 확정하고 나머지는 시작하지 않는다. Published authority를 보존하며 waiter는 `Blocked/ShutdownRequested`다. |

`Relocated`의 `Shutdown`은 accepted work와 infrastructure만 정리한다. `Serving`에서
바로 호출하면 object를 이전하지 않는다.

`Draining` 동안 descriptor와 owner lease를 계속 갱신한다. 이미 수락한 request,
relocation과 session route 변경이 끝나기 전에 owner 권한을 잃지 않도록 모든 작업이
끝난 뒤 lease 사용을 종료한다. 정리 순서는 다음과 같다.

Fanout publisher의 endpoint, identity와 실행 세대를 Store에 게시한 정보를
[fanout publisher descriptor](01-glossary.ko.md#fanout-publisher-descriptor)라고
한다.

1. Actor membership과 local instance를 유지한 채 Spot closing callback을 끝내고 local
   scope를 정리한다.
2. Current authority를 가진 source만 owner와 이동 대상 record를 다음 상태로
   바꾸거나 제거한다.
3. MeshNode, ClientServer server와 fanout publisher descriptor와 owner lease를
   release한다.
4. Peer connection, listener, executor와 binding transport를 닫는다.

표준 cooperative cancellation을 지원하는 언어는 Spot closing callback에 남은
deadline을 나타내는 정리용 cancellation signal을 전달한다. 이미 수락한 handler의
token은 재사용하지 않는다.
Callback exception은 `ForceStopped/TeardownFailed`, deadline 만료는
`ForceStopped/DeadlineExceeded`다. Hardware failure와 `SIGKILL`에서는 callback을
보장하지 않는다. 종료 중이던 relocation이나 cleanup을 다른 runtime이 자동으로
이어받는 동작을 보장하지 않는다.

## 12. State별 admission

Framework가 같은 ChannelName의 Server 후보 중 하나를 고르는 방식을
[select-one](01-glossary.ko.md#select-one)이라고 한다. Caller가 node RID를 직접
지정하는 호출은 [Node direct](01-glossary.ko.md#node-direct)다. 같은 Channel에
참여한 여러 Spot에 message를 보내는 기능은
[Logical Multicast](01-glossary.ko.md#logical-multicast)다.

Source runtime에서 현재 owner까지 message를 보내는 경로를
[owner route](01-glossary.ko.md#owner-route)라고 한다.

| 공개 기능 | `Relocating` | `Relocated` | `Draining` |
|---|---|---|---|
| ChannelName select-one | 새 selection에서 제외하지만 기존 direct owner route는 유지한다. | 새 selection에서 제외하고 infrastructure 연결은 유지한다. | 새 admission을 닫고 이미 제출한 operation만 terminal 상태까지 처리한다. |
| Logical Multicast | 새 target snapshot에서 제외하고 이미 수락한 제출은 유지한다. | 새 target snapshot에서 제외한다. | 새 admission을 닫고 이미 수락한 제출만 처리한다. |
| Node direct application request | 기존 owner request는 unit seal 전까지 수락한다. | Local stateful owner가 없으므로 새 request를 받지 않는다. | 새 request를 shutdown 결과로 끝낸다. |
| Node direct infrastructure control | Relocation, completion, binding과 recovery control을 계속 수락한다. | Monitoring과 shutdown control을 계속 수락한다. | Termination barrier에 필요한 control만 deadline까지 수락한다. |
| Spot·Actor direct | Unit seal 전까지 payload와 timer를 계속 처리한다. | Local stateful owner가 없으므로 새 payload를 받지 않는다. | 새 payload를 거부하고 이미 수락한 turn만 처리한다. |
| Spot·Actor create와 join | 새 owner와 membership admission을 거부한다. | 같은 거부를 유지한다. | 같은 거부를 유지한다. |
| Instance Spot placement | 새 target claim에서 제외하고 기존 direct route는 seal 전까지 유지한다. | 새 target claim에서 제외한다. | Seal 전에 수락한 activation만 terminal 상태까지 처리한다. |
| STREAM | 새 binding에서 제외하고 기존 session은 unit barrier로 처리한다. | 새 binding에서 제외하고 infrastructure connection은 유지한다. | 새 session을 받지 않고 pending reply와 binding barrier만 처리한다. |
| ClientServer server | 새 selection에서 제외하고 accepted handler와 reply route를 유지한다. | Service connection은 유지하지만 새 selection에서 제외한다. | Handler admission을 닫고 accepted request의 reply route만 유지한다. |
| Classic fanout publisher | 새 automatic subscriber connection을 만들지 않고 accepted event를 처리한다. | Infrastructure는 유지하지만 새 publish admission을 받지 않는다. | Publish admission을 닫고 accepted event만 처리한다. |

이미 수락한 request는 reply, error, timeout 또는 shutdown 중 하나로 한 번만 끝난다.
Application callback이 대기해도 infrastructure execution은 request completion, peer
lifecycle, recovery와 session binding을 계속 진행한다. Observer와 monitoring
callback은 maintenance를 막는 claim을 소유하지 않는다.

## 13. 관측 정보

State와 relocation result 변화는 `zlink.runtime.host.relocation_changed`, shutdown
result 변화는 `zlink.runtime.host.termination_changed`로 관찰한다. Terminal event는
observer overflow로 잃지 않는다. Relocation event와 제한된 개수의 진단 상태에는
mode와 effective target version을 포함한다. Version을 metric label로 추가하지 않는다.

Host state와 terminal 결과는 host status와 structured log에서 확인한다. 집계가 필요하면
host state, relocation mode·outcome·reason과 shutdown outcome·reason을
[51 Runtime metrics](25-runtime-metrics.ko.md#5-host-relocation과-shutdown)가 정의한
계기로 기록한다. Object relocation 계기와 host-wide operation 계기는 서로 다른 이름을 사용한다.

Spot을 system 전체에서 찾는 전역 문자열 주소를
[Spot ID](01-glossary.ko.md#spot-id)라고 한다. Metric label에는 Actor ID, Spot ID,
node RID, endpoint, session ID와 relocation ID를
넣지 않는다. 개별 blocker와 relocation 상태는 개수를 제한한 진단 조회와 trace에서
확인한다. Telemetry provider failure는 operation 진행을 막지 않는다. 전체 관측
계약은 [50 Runtime monitoring](24-runtime-monitoring.ko.md)과
[51 Runtime metrics](25-runtime-metrics.ko.md)가 소유한다.

## 14. Contract test 검증 요구

| 범위 | 반드시 검증할 항목 |
|---|---|
| Mode와 target | Planned maintenance는 같은 version만, rolling update는 요청한 더 높은 exact version만 선택하는지 검증한다. Version을 capacity와 weight보다 먼저 적용하고, 같은 wave를 제외하며, 모든 Mesh에서 exact Core peer가 ready일 때만 진행해야 한다. Target이 없으면 기다리고 manual topology이면 차단해야 한다. |
| Lifecycle | Preflight가 막히면 `Serving`을 유지하고, 성공하면 infrastructure를 유지한 `Relocated`가 되는지 검증한다. `Shutdown`은 별도로 호출하며 기본 deadline은 30초다. Caller cancellation은 waiter만 끝내고 잘못된 runtime state에서는 admission을 바꾸지 않아야 한다. |
| Concurrency | 같은 option의 relocation과 concurrent shutdown은 각각 하나의 operation을 공유하는지 검증한다. 다른 relocation option은 `OperationInProgress`, relocation 중 shutdown은 `ShutdownRequested`로 끝나며 terminal result를 반복 호출해도 같은 값을 반환해야 한다. |
| Unit gate | Outbound 64, inbound 64, payload 256 MiB, `Capture`와 `Restore` 각각 8, participant별 64 MiB를 검증한다. Permit은 한 번에 모두 얻어야 하며 oversized aggregate는 다른 payload가 없을 때 하나만 실행해야 한다. |
| Handoff | `SpotWide` User Spot aggregate를 한 번에 commit하고 queue, journal, timer와 pending tick을 함께 이전하는지 검증한다. Hold와 temporary queue는 각각 1,024 record와 16 MiB를 넘지 않아야 한다. Target dispatcher는 Spot과 모든 member Actor를 같은 relocation temporary queue group에 등록하되 record의 실제 target을 보존해야 한다. 모든 Restore, aggregate commit과 `OnRelocationReadyCompleted`가 끝난 뒤 저장된 기존 작업을 먼저 넣고 temporary 작업을 target별 실제 queue로 옮겨야 한다. 전환 전에는 어느 participant의 application 작업도 실행하면 안 된다. Message Follow route는 위치 갱신 응답을 받지 못해도 `MessageFollowDuration` 뒤 제거해야 한다. Instance Spot을 숨겨서 새로 만들면 안 된다. |
| PerActor handoff | Entry Spot과 `PerActor` User Spot이 Actor만 독립적으로 이전하고 Spot adapter나 membership callback을 호출하지 않는지 검증한다. Spot authority 전환 뒤 `ToSpot`·Create·Join은 target, `ToActor`는 Actor별 current owner를 사용해야 한다. Spot과 Actor relocation temporary queue는 독립적으로 등록해야 한다. 저장된 기존 작업, temporary 작업과 전환 뒤 direct 작업 순서를 보존하고 같은 relocation request를 재전송해도 temporary queue와 Restore를 두 번 만들면 안 된다. |
| Interruption 목표 | Actor, Instance Spot, `SpotWide` User Spot과 `PerActor` Spot direct message 각각에 대해 source가 새 작업을 막은 시점부터 target이 message 처리를 시작할 수 있다고 알릴 때까지 1초를 측정한다. 초과를 failure, rollback 또는 retry 조건으로 사용하지 않는다. Host deadline 뒤에는 새 unit을 시작하지 않고 이미 시작한 unit을 안전한 terminal 상태까지 처리한다. |
| Failure | Commit 전 abort에서는 target temporary queue를 실행하지 않고 폐기하며 source 원본만 queue에 되돌려야 한다. Request terminal 결과를 두 runtime에서 중복으로 만들면 안 된다. Commit 뒤 같은 target runtime이 실패하면 source로 rollback하거나 다른 target을 자동 선택하지 않는다. 정확한 `Blocked` reason을 반환하고 terminal result를 한 번만 완료하며 descriptor rollback을 확인할 수 없으면 bounded teardown을 수행해야 한다. Process 종료 뒤 relocation 자동 재개는 검증 대상이 아니다. |
| Cleanup과 관측 | Barrier가 끝날 때까지 lease를 갱신하고 accepted request를 한 번만 완료하는지 검증한다. Callback failure를 정해진 reason으로 분류하며 state, outcome, reason, event와 metric이 wire 값과 일치해야 한다. Topology cleanup은 다른 authority를 변경하면 안 된다. |
