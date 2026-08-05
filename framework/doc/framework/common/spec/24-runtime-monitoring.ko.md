---
title: "Runtime 상태 조회와 운영 진단"
---

# Runtime 상태 조회와 운영 진단

[스펙 목차](README.ko.md) · [이전: Relocation Store provider SPI와 공식 Redis 구현](23-relocation-store-redis.ko.md) · [다음: Runtime metric과 집계 규칙](25-runtime-metrics.ko.md)

> **이 장이 정의하는 것** — 운영자가 Framework runtime의 현재 상태를 조회하고 변화를
> 관찰하며 원인을 log에서 찾는 방법.


## 1. 이 문서가 정의하는 계약

이 문서는 application 운영자가 Framework runtime의 현재 상태를 한 번 조회하고, 이후
변화를 관찰하며, 상태가 바뀐 이유를 log에서 찾는 방법을 정의한다. Application은 이
정보로 새 작업을 받을 수 있는지와 장애 범위, relocation·shutdown 결과를 판단한다.

이 문서는 특정 시점의 완전한 status, status 변화 stream과 structured log identifier를
소유한다. 시간에 따라 누적하거나 수집하는 수치의 이름·단위·label은
[Runtime metrics](25-runtime-metrics.ko.md), message 한 건의 진행 기록은
[Message flow tracing](26-message-flow-tracing.ko.md), relocation과 shutdown의 상태 전이는
[Host relocation와 shutdown](28-graceful-drain-handoff.ko.md)이 소유한다.

| 주체 | 책임 |
|---|---|
| Application | 등록 이름으로 status를 조회·관찰하고, logger provider와 backend를 구성한다. |
| Framework | 내부 service 값을 조합하여 완전한 status를 만들고 상태 변화의 표준 identifier를 기록한다. |
| Provider | Application이 선택한 logger backend로 log를 전달한다. Provider 실패가 runtime 결과를 바꾸지 않게 한다. |
| Remote runtime | 자신의 service 제공 가능 여부와 운영 상태를 게시한다. 현재 runtime은 이를 topology status에 반영한다. |

Remote 등록 정보가 바뀐 순서를 나타내는
[descriptor revision](01-glossary.ko.md#descriptor-revision)과 host가 현재 lifecycle의
소유권을 계속 사용할 수 있음을 나타내는
[owner lease](01-glossary.ko.md#owner-lease)는 내부 판단에만 사용한다. Public interface에는
이 두 값, 작업 수락의 내부 상태, claim, capacity reservation, socket 상태, exporter,
저장소, raw event DTO와 native handle을 노출하지 않는다.

## 2. Application이 한 번에 읽는 상태

Application은 startup에 등록한 이름으로 기능별 status를 읽는다. 여러 내부 service의
값을 직접 조합하지 않는다.

처음 등장하는 등록 이름을 구분하면 다음과 같다.

- 한 process에서 RouteMesh의 peer 연결과 Channel 메시징을 제공하는 runtime 단위를
  [MeshNode](01-glossary.ko.md#meshnode)라고 한다. 여러 MeshNode가 같은 메시징 규칙을
  공유하는 논리 runtime을 [RouteMesh](01-glossary.ko.md#routemesh)라고 하며, RouteMesh
  하나를 식별하는 startup 등록 이름을 [MeshName](01-glossary.ko.md#meshname)이라고 한다.
- Channel 하나를 식별하는 startup 등록 이름을
  [ChannelName](01-glossary.ko.md#channelname)이라고 한다.
- Client와 Server가 ChannelName으로 요청과 reply를 교환하는 topology를
  [ClientServer Channel](01-glossary.ko.md#clientserver-channel)이라고 한다.
- 주소와 상태를 가지고 message를 받는 논리 실행 단위를
  [Spot](01-glossary.ko.md#spot)이라고 한다.
- 기능별 serving 조건을 모두 만족하여 application message를 받을 수 있는 상태를
  [Ready](01-glossary.ko.md#ready)라고 한다.

| 상태 범위 | 한 status에서 확인하는 값 |
|---|---|
| Host | Runtime state, ready 여부, 새 작업 수락 여부, deadline, relocation·shutdown 결과와 inbound dispatch 상태 |
| RouteMesh | `MeshName`, 전체 state, ready peer 수, Channel별 ready target 수, peer별 운영 상태와 현재 process의 Actor·Spot 수 |
| ClientServer | `ChannelName`, local role, 전체 state, ready target 수와 target별 운영 상태·weight |
| Automatic fanout | `ChannelName`, 전체 state, 연결을 시도하는 publisher 수와 ready publisher 수 |

Status는 호출이 끝난 뒤에도 보관할 수 있는 변경 불가능한 값이다. Native handle, caller
buffer, payload와 application metadata를 참조하지 않는다.

다음 C#은 공통 동작을 보여 주는 비규범적 발췌다. 다른 언어에 같은 signature를
요구하지 않는다. 정확한 type과 signature는
[.NET topology monitoring](server/languages/dotnet/interfaces/10-topology-monitoring.ko.md)이
정한다.

```csharp
public interface IZLinkRouteMeshRuntime
{
    ZLinkRouteMeshStatus GetStatus(string meshName); // 등록한 RouteMesh의 현재 상태를 읽는다.

    IAsyncEnumerable<ZLinkObservedStatus<ZLinkRouteMeshStatus>> ObserveAsync(
        string meshName,
        CancellationToken cancellationToken = default); // 이후의 완전한 상태와 유실 누계를 순서대로 받는다.
}
```

Host 상태는 특정 `MeshName`에 속하지 않는다. Relocation과 shutdown의 최종 결과도 host
status에서 한 번만 제공한다.

### 2.1 Host 상태

Host runtime state는 다음 값으로 닫혀 있다. 표에 없는 값을 추가해서는 안 된다.
새 작업을 차단하고 이미 수락한 작업을 제한 시간 안에 정리하는 절차를
[drain](01-glossary.ko.md#drain과-draining)이라고 한다. 그 제한 시각을
[deadline](01-glossary.ko.md#deadline)이라고 한다.

| 값 | Application이 관찰하는 의미 |
|---|---|
| `preparing` | Startup 구성을 검증하고 runtime을 준비하고 있다. |
| `serving` | 새 application operation을 받을 수 있다. |
| `relocating` | 새 작업 수락을 중단하고 stateful object를 다른 node로 이전하고 있다. |
| `relocated` | Relocation을 끝냈으며 infrastructure와 연결은 유지한다. |
| `draining` | Relocation 없이 남은 처리와 resource를 정리하고 있다. |
| `stopped` | Runtime과 infrastructure 정리가 끝났다. |
| `error` | Runtime을 계속 운영할 수 없는 오류가 발생했다. |

`IsReady`는 `State`가 `serving`일 때만 `true`다. `AcceptingWork`는 현재 host가 새
application operation을 수락하는지를 나타낸다. 두 값을 별개의 조건처럼 재해석하지
않는다. Relocation option, deadline과 result의 정확한 의미는
[Host relocation와 shutdown](28-graceful-drain-handoff.ko.md)이 정한다.

Host status의 inbound dispatch 항목은 Framework가 수신한 application payload가 현재 얼마나 처리 중인지
보여 준다. 값은 queue를 조회할 때마다 순회해서 만들지 않고 dispatch accounting에서 유지한 누계를 읽는다.

| 값 | 의미 |
|---|---|
| Applied HWM bytes | startup에서 확정한 [Application HWM](01-glossary.ko.md#application-hwm)이다. `0`은 제한하지 않는다는 뜻이다. |
| Pending payload bytes | Queue 대기와 handler 실행 중 payload byte의 합계다. |
| Queued payload bytes | 아직 handler를 시작하지 않은 payload byte다. |
| Active payload bytes | Handler가 실행 중인 payload byte다. |
| Application receive paused | HWM 때문에 새 application receive를 시작하지 않는지를 나타낸다. |
| Pending completion sends | Reply permit을 기다리거나 확보한 request 수다. |
| Completion send limit | Host가 동시에 보유할 수 있는 completion send permit 상한이다. |

`PendingPayloadBytes = QueuedPayloadBytes + ActivePayloadBytes`를 만족해야 한다. 이 status는 payload,
Actor ID, Spot ID, message type이나 owner별 목록을 포함하지 않는다. Owner별 top-N은 public contract로
제공하지 않는다. 일반 status 갱신과 message hot path에 owner별 label이나 event를 추가하지 않는다.

### 2.2 Topology 상태

Topology state는 host state와 다른 범위를 나타낸다. Host state는 process 전체의
startup, relocation과 shutdown 진행 상태다. Topology state는 `MeshName` 또는
`ChannelName`으로 등록한 RouteMesh·ClientServer·automatic fanout 하나가 현재
application message를 처리할 수 있는지를 나타낸다.

따라서 host가 `serving`이어도 특정 ClientServer Channel에 ready target이 없으면 그
topology만 `degraded`일 수 있다. 반대로 host가 `relocating`, `relocated` 또는
`draining`이면 연결이 남아 있어도 모든 topology의 `IsReady`는 `false`다.
이때 연결된 peer·target 수는 현재 연결 상태를 그대로 제공한다. Host가 application
traffic을 받지 않는다는 이유로 count를 `0`으로 바꾸지 않는다.

| 상태 종류 | 허용 값 |
|---|---|
| Topology state | `starting`, `ready`, `degraded`, `stopping`, `stopped`, `failed` |
| Topology reason | `runtime_not_ready`, `no_ready_peer`, `no_ready_target`, `location_unavailable`, `capacity_exceeded`, `draining`, `internal_failure` |
| Peer state | `connecting`, `ready`, `draining`, `not_connected`, `not_required` |
| ClientServer local role | `client`, `server`, `client_and_server` |

| Topology state | 의미 |
|---|---|
| `starting` | 해당 topology의 listener, connection과 registration을 준비하고 있다. |
| `ready` | Host가 `serving`이고 해당 topology가 application message를 처리할 수 있다. |
| `degraded` | 일부 peer·target 또는 Location Store를 사용할 수 없어 해당 topology의 기능 전부를 제공할 수 없다. |
| `stopping` | Host shutdown에 따라 해당 topology가 이미 수락한 작업과 연결을 정리하고 있다. |
| `stopped` | 해당 topology의 작업과 연결 정리가 끝났다. |
| `failed` | 해당 topology를 계속 운영할 수 없는 오류가 발생했다. |

RouteMesh peer는 node의 transport identity인
[Routing ID](01-glossary.ko.md#routing-id)를 Node RID 값으로 제공한다. Endpoint,
descriptor revision과 connection generation은 제공하지 않는다.

Peer state는 연결이 없는 두 경우를 구분한다.

| Peer state | 의미 | Ready·장애 집계 |
|---|---|---|
| `not_connected` | Topology상 연결이 필요하지만 현재 ready connection이 없다. | Ready peer 수에서 제외한다. Topology degraded 여부와 liveness·health failure 집계에 반영한다. |
| `not_required` | 두 MeshNode가 모두 Object Client이고 양쪽 모두 RouteMesh Channel Server membership이 없어 연결할 필요가 없다. Automatic은 descriptor 확인 단계에서 제외하고 Manual은 handshake에서 확인한다. | Ready peer 수에서 제외한다. Liveness probe·reconnect·health failure 집계 대상도 아니다. |

`not_required` peer도 status의 peer 목록에는 남긴다. 운영자는 이 값으로 정상적인 연결
생략과 연결 장애를 구분할 수 있다. 이 상태 하나만으로 RouteMesh를 `degraded`로
바꾸지 않는다.

RouteMesh placement 상태는 새 object 수락 여부와 현재 active Actor·Spot 수를 제공한다.
Status는 Spot과 그 안에서 application message를 처리하는 Actor의 개수를 각각
제공한다. Startup에 등록한 type별 capacity reservation, Spot 초기화가 끝나기 전에
최초 message 전달을 막는
[activation barrier](01-glossary.ko.md#activation-barrier)와 내부 capacity counter는
제공하지 않는다.

Placement의 `IsAvailable`은 host가 `serving`이고 Object Server이며 placement weight가
양수이고, Actor 또는 Spot capacity와 activation concurrency에 모두 여유가 있을 때만
`true`다. Activation concurrency의 현재 값과 limit은 public status에 별도 field로
노출하지 않는다.

새 target 선택 비율에 사용하는 [weight](01-glossary.ko.md#weight)는 signed integer
`0..10000`이다. 값이 `0`이면 새 placement 대상으로 선택하지 않는다.

같은 process의 ClientServer Server도 remote Server와 같은 후보다. Status는 target 수와
각 target의 상태·weight를 제공한다. `client_and_server`는 같은 `ChannelName`에 두 역할이
등록되었다는 뜻이며 별도 registration role이 아니다.

Automatic fanout publisher는 socket을 연결한 뒤 application record 또는 Framework가
연결 상태를 확인하려고 주고받는
[liveness beacon](01-glossary.ko.md#liveness와-liveness-beacon)을 받으면 ready가 된다.
Disconnect를 확인하거나 15초 동안 record가 없으면 해당 publisher만 후보에서 제외한다.
연결 계획이나 `connect` 수락만으로 ready가 되지 않는다.

## 3. 현재 상태 조회와 변화 관찰

각 언어는 현재 status 조회와 비동기 변화 관찰을 제공한다. 이름과 type은 언어별 exact
interface가 정한다.

```csharp
var current = routeMeshRuntime.GetStatus("game-mesh");
// Readiness와 target 상태가 같은 시점에 만들어진 한 값에 들어 있다.

await foreach (var observed in routeMeshRuntime.ObserveAsync("game-mesh", cancellationToken))
{
    await RecordStatusAsync(observed.Status, cancellationToken);
    // observed.Loss가 이 관찰자가 지금까지 놓친 개수다.
    // 관찰 코드는 routing이나 lifecycle 결정을 변경하지 않는다.
}
```

Status는 runtime instance 안에서 단조 증가하는 `Sequence`와 관찰 시각을 포함한다. 같은
source에서는 큰 `Sequence`가 더 나중 상태다. 서로 다른 source의 값은 비교하지 않는다.
Process가 다시 시작되면 `Sequence`는 0부터 시작할 수 있다.

변화 stream의 각 항목은 일부 field만 담은 event가 아니라 완전한 status다. Nullable
field를 조합하는 범용 event DTO는 제공하지 않는다. 관찰자가 `Sequence` gap을 발견하면
현재 status를 다시 조회하여 모든 field를 복원한다.

### Source의 정의

합치기의 단위인 **source**는 **`Sequence`를 소유한 것**과 같다. 각 status 항목이
`Sequence` 하나를 갖고 오므로, 그 `Sequence`를 발행하는 주체가 source다.

| Stream 종류 | Source | Source 키 |
|---|---|---|
| Host 상태 | 이 runtime instance 하나 | runtime instance ID. process 수명 동안 하나다 |
| Topology 상태 | topology runtime 하나 | RouteMesh는 `MeshName`, ClientServer·fanout은 `ChannelName` |

**peer와 객체 이동은 별도 source가 아니다.** 이들은 topology status 안에 목록으로 실려
오며 자기 `Sequence`를 갖지 않는다. peer 하나가 바뀌면 그 topology의 status 전체가 새
`Sequence`로 발행된다. peer별·이동별 slot을 따로 두려면 **먼저 별도 stream과 언어별
계약을 정의해야** 하며, 그때까지 이 합치기 규칙의 단위가 아니다.

Source 키는 그 대상이 처음 관측 대상이 될 때 만들고, **terminal status가 전달되거나
폐기된 뒤** 없앤다. 키가 살아 있는 동안 `Sequence`는 그 키 안에서 단조 증가한다.

### 합치기

Framework는 느린 관찰자 때문에 message dispatch, location claim과 host lifecycle이
지연되지 않도록 중간 status를 합칠 수 있다. 합치기는 **source별 최신 status 한 자리**를
유지하는 방식이다. 같은 source의 이전 중간 status는 최신 status가 대신한다. 이 경우에도
다음 결과를 보장한다.

- **보관 중인 source**에 대해 가장 최근 status의 `Sequence`를 전달한다.
- Relocation과 shutdown의 terminal status는 중간 status로 덮어쓰지 않는다.
- 한 관찰자의 지연, 취소 또는 실패가 다른 관찰자와 runtime 결과를 바꾸지 않는다.
- 누계 field는 합친 뒤에도 최신 값을 반영한다. Backpressure와 drop counter의 증가분을
  합치기로 잃지 않는다.

Source별 한 자리 구조에서도 관찰자가 계속 읽지 않으면 종료된 source의 terminal status가
쌓인다. 이 보관량에는 상한이 있으며, 상한을 넘기면 framework는 **가장 오래된 terminal
status부터 버린다.** Terminal status를 무한히 보관하는 구조는 느린 관찰자 하나가 runtime
memory를 소진시키므로 허용하지 않는다.

버릴 때 관찰자가 유실을 알 수 있어야 한다. 유실 수는 관찰자마다 다르므로 **status
안에 넣지 않는다** — status는 관찰자 사이에 공유하는 값이고, 여기에 관찰자별 값을 넣으면
공유할 수 없게 된다.

대신 stream이 전달하는 단위를 **status와 전달 정보의 쌍**으로 정의한다.

| 구성 | 내용 |
|---|---|
| status | 지금까지와 같은 완전한 status. 관찰자 사이에 공유한다 |
| 유실 누계 | 이 관찰자가 구독을 시작한 뒤 잃은 항목 수 |

유실 누계는 **중간 status 합치기로 사라진 것과 terminal 폐기로 사라진 것을 각각** 센다.
둘을 하나로 합치면 관찰자가 "따라잡기로 건너뛴 것"과 "영영 못 보는 것"을 구분하지 못한다.
구독을 새로 시작하면 0에서 다시 시작한다. 표현 범위를 넘으면 최댓값으로 고정한다.

runtime 전체 metric은 이 값을 대신할 수 없다 — 어느 관찰자가 무엇을 잃었는지 판정하지
못하기 때문이다. 관찰자는 이 값의 증가와 `Sequence` gap으로 유실을 안다.

Source의 최신 slot은 그 source lifecycle이 끝나고 terminal status가 전달되거나 폐기된 뒤에
제거한다. 제거된 source는 위 "보관 중인 source"에서 빠진다.

Framework는 관찰자 queue가 가득 찼다는 이유로 stream을 종료하지 않는다. 위의 합치기와
보관 상한으로만 따라잡으며, 관찰자가 계속 느려도 stream은 열려 있다. 관찰 취소는 해당
stream만 종료한다. 이미 수락한 runtime 작업이나 다른 관찰자는 취소하지 않는다.

## 4. Object의 현재 위치 조회

운영 도구는 Actor ID 또는 Spot의 전역 논리 주소인
[Spot ID](01-glossary.ko.md#spot-id)로 현재 위치를 정확히 조회하거나, 저장된 위치
정보의 관리 범위를 page 단위로 열거할 수 있다. 이 결과를 messaging target이나
placement selector로 사용하지 않는다.

위치 정보의 기준 저장소인 [Location Store](01-glossary.ko.md#location-store)가 제공하는
field, page 크기와 cache 계약은
[Location runtime의 운영 조회](21-location-runtime.ko.md#64-운영-도구에서-현재-위치를-조회한다)가
정한다.

## 5. Structured log

Framework는 상태가 바뀐 이유를 표준 structured logger에 기록한다. Application은 logger
provider와 backend를 구성한다. Framework public interface는 sink, file path, exporter
lifecycle과 event DTO를 제공하지 않는다.

다음 identifier는 모든 언어에서 같은 문자열을 사용한다.

| Identifier | 기록하는 변화 |
|---|---|
| `zlink.runtime.mesh_node.state_changed` | MeshNode의 lifecycle 또는 ready 상태가 바뀌었다. |
| `zlink.runtime.mesh_node.peer_changed` | Peer의 작업 수락, ready 또는 service 상태가 바뀌었다. |
| `zlink.runtime.mesh_node.channel_changed` | Channel weight, ready target 수 또는 선택 가능 상태가 바뀌었다. |
| `zlink.runtime.object.placement_changed` | Reservation, Ready, abort, capacity exhaustion 또는 relocation으로 placement 집계가 바뀌었다. |
| `zlink.runtime.mesh_node.routing_id_conflict` | Automatic Node RID owner claim이 active conflict로 실패했다. |
| `zlink.runtime.host.relocation_changed` | Relocation mode, effective target version, host state 또는 terminal result가 바뀌었다. |
| `zlink.runtime.host.termination_changed` | Shutdown state 또는 terminal result가 바뀌었다. |
| `zlink.runtime.relocation.changed` | Actor 또는 Spot relocation phase·recovery 상태가 바뀌거나 한 unit의 admission 중단 시간이 1초를 넘었다. |
| `zlink.runtime.client_server.state_changed` | ClientServer local role, lifecycle 또는 ready 상태가 바뀌었다. |
| `zlink.runtime.client_server.server_changed` | ClientServer target의 weight, ready 또는 service 상태가 바뀌었다. |
| `zlink.runtime.fanout.publisher_changed` | Automatic publisher의 연결 대상 또는 ready 상태가 바뀌었다. |
| `zlink.runtime.location.store_changed` | Location Store가 ready와 degraded 사이에서 바뀌었다. |

Log는 timestamp, source 종류와 등록 이름을 기록한다. 필요한 변화에는 Node RID, weight,
reason과 state를 추가한다. Payload, metadata, Actor ID, Spot ID, owner token, generation,
raw frame와 native handle은 기록하지 않는다.

Mailbox에 work가 들어오거나 빠질 때 structured log나 전용 metric을 기록하지 않는다.
Framework는 mailbox의 개별 enqueue·dequeue와 turn을 운영 event로 만들지 않는다.
Operation 실패는 drop·timeout·backpressure metric으로 집계하고, 개별 message 지연은
[Message flow tracing](26-message-flow-tracing.ko.md)으로 조사한다.

Publisher 상태는 `excluded_draining`, `excluded_stale`, `reconnecting`, `disconnected`로
기록한다. Log는 기록 시점의 판단이며 현재 위치나 상태의 기준이 아니다. 현재 상태는
fanout status에서 읽는다.

Relocation unit의 source admission seal부터 target admission-open ACK까지 1초를
넘기면 `zlink.runtime.relocation.changed`에 `unit_kind`, 필요한 경우
`execution_mode`, `interruption_target_exceeded=true`와 실제 duration을 기록한다.
`unit_kind`는 `actor`, `instance_spot`, `user_spot` 중 하나다. 이는 운영
경고이며 relocation outcome이나 recovery 판단을 바꾸지 않는다. Actor ID와 Spot
ID는 structured log에 넣지 않고 제한된 trace에서만 확인한다.

## 6. Startup과 실패

- 등록하지 않은 `MeshName`이나 `ChannelName`의 status를 요청하면 구성 오류다.
- Manual subscriber로만 등록한 fanout `ChannelName`에 automatic status를 요청하면 구성
  오류다.
- Location Store가 없는 runtime은 store 상태를 `not_configured`로 표시한다.
- Object role이 `Client` 또는 `Server`인데 Location Store가 없으면 host startup이
  실패한다.
- Metric이나 trace를 끄더라도 runtime status는 계속 사용할 수 있다.
- Logger provider의 실패는 message dispatch, reply, topology 조정과 host lifecycle
  결과를 바꾸지 않는다.

## 7. 구현 및 contract test 검증 요구

- Host status 하나만으로 readiness, 새 작업 수락 여부, relocation과 shutdown 결과를
  판단할 수 있어야 한다.
- RouteMesh, ClientServer와 automatic fanout은 각각 하나의 완전한 status로 readiness와
  target 상태를 제공해야 한다.
- Public status에는 endpoint, descriptor revision, owner lease, claim, reservation,
  native handle과 raw event DTO가 없어야 한다.
- Publish target 수와 target별 수락·실패 결과를 status나 runtime structured log에
  포함해서는 안 된다.
- Automatic fanout의 15초 record timeout은 해당 publisher만 unavailable로 바꿔야 한다.
- 느린 관찰자, 관찰 취소와 logger provider 실패가 dispatch, reply와 lifecycle terminal
  result를 바꾸면 안 된다.
- `Sequence` gap 뒤 현재 status를 다시 조회하여 모든 상태를 복원할 수 있어야 한다.
- Placement weight `0`, capacity exhaustion과 recovery가 public status와 일치해야 한다.
- Object 위치 조회는 Location runtime의 page와 cache 계약을 지켜야 한다.
