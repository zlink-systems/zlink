---
title: "MeshNode"
---

# MeshNode

[스펙 목차](README.ko.md) · [이전: SPOT 메시징](12-spot-messaging.ko.md) · [다음: Actor 모델](14-actor-model.ko.md)

> **이 장이 정의하는 것** — RouteMesh에 참여하는 MeshNode의 identity, object role,
> object 배치 조건과 startup 순서.


## 1. 이 문서가 정의하는 범위

이 문서는 ZLink Framework에서 RouteMesh에 참여하는 MeshNode의 identity,
object role, object를 배치할 수 있는 조건과 startup 순서를 정의한다.

[MeshNode](01-glossary.ko.md#meshnode)는 다른 node와 물리적으로 연결되고 Channel membership을 제공한다. Actor와
Spot은 이 연결을 사용하지만 MeshNode RID를 자신의 logical identity로 사용하지 않는다.
Framework는 Actor와 [Spot](01-glossary.ko.md#spot)의 전역 logical identity를 현재 owner가 존재하는 MeshNode의
route로 연결한다.

## 2. MeshNode가 가지는 identity와 설정

MeshNode 하나에는 다음 정보가 속한다.

| 항목 | 의미와 변경 가능 여부 |
|---|---|
| `MeshName` | 어떤 물리 [RouteMesh](01-glossary.ko.md#routemesh)와 MeshNode descriptor namespace에 속하는지를 정한다. MeshNode가 시작된 뒤에는 바꿀 수 없다. |
| Routing ID(`RID`) | 현재 MeshNode lifecycle을 식별하는 transport identity다. |
| Endpoint | 다른 peer가 이 MeshNode의 ROUTER에 연결할 주소다. |
| `ChannelName` set | Server role로 참여하는 Channel 목록이다. 0개 이상 등록할 수 있으며 시작된 뒤에는 바꿀 수 없다. |
| Object role | `None`, `Client`, `Server` 중 하나다. Startup 전에 정한다. |
| Lifecycle generation | 같은 transport identity에서 서로 다른 lifecycle을 구분하는 0이 아닌 식별 값이다. |
| [Descriptor](01-glossary.ko.md#descriptor) revision | 같은 lifecycle 안에서 바뀔 수 있는 [MeshNode descriptor](01-glossary.ko.md#meshnode-descriptor) 내용을 구분하는 0이 아닌 값이다. |

`MeshName`은 ActorId나 SpotId의 identity에 포함되지 않는다. ActorId와 User·Instance
SpotId는 Location Store가 관리하는 전체 transaction 범위에서 각각 전역 key다.
`MeshName`은 object를 처음 배치할 Mesh와 현재 [owner](01-glossary.ko.md#owner)에게 도달할 물리 route를 나타내는
속성이다.

같은 process에는 같은 `MeshName`의 MeshNode를 하나만 등록할 수 있다. 서로 다른
`MeshName`의 MeshNode는 여러 개 등록할 수 있지만 Framework가 RouteMesh 사이의
transport relay를 자동으로 만들지는 않는다.

`ChannelName`을 추가해도 별도 socket이나 endpoint가 생기지 않는다. MeshNode
descriptor를 게시한 뒤에는 다음 설정을 바꿀 수 없다.

- Channel [membership](01-glossary.ko.md#membership)
- Object role
- Factory
- Stable type과 type capability

## 3. Routing ID

### 3.1 Automatic discovery에서 사용하는 RID

Automatic discovery를 사용하는 MeshNode의 RID는 Framework가 lifecycle마다 새로
만든다. Caller는 진단에 사용할 prefix만 지정할 수 있다. Prefix를 생략하면
Framework가 listener 종류에 맞는 기본 prefix를 사용한다.

| 구성 요소 | 계약 |
|---|---|
| Prefix | ASCII `[A-Za-z0-9._-]` 문자만 사용하며 길이는 `1..64`자다. |
| UUID | RFC 4122 UUID v4 bit layout을 사용하는 16-byte random value를 `8-4-4-4-12` 자리의 36자 lowercase canonical 문자열로 표현한다. |
| Full RID | `prefix-<lowercase-canonical-uuid-v4>` 형식이며 UTF-8로 encode한 크기는 255 bytes 이하다. |

Core binary RID, Framework prefix, Entry Spot과 caller-provided RID를 함께 다루는 전체 규칙은
[시스템 전체 Routing ID 정책](10-network-listener-identity.ko.md#7-시스템-전체-transport-rid와-spot-id-정책)이 정의한다.

Prefix와 UUID를 object placement, shard 또는 재시작 뒤에도 유지되는 application
identity로 해석하지 않는다.

MeshNode descriptor의 owner를 확정하는 CAS는 같은 `(MeshName, RID)`를 현재 다른
owner가 사용하고 있는지 확인한다. Active conflict가 확인되면 기존 descriptor를 변경하지 않고 두 번째
UUID나 claim을 만들지 않는다. Startup은 즉시 configuration error로 끝난다.

Replacement lifecycle은 이전 lifecycle의 RID를 재사용하지 않고 새 RID를 만든다.

### 3.2 Entry Spot ID

Object Server MeshNode는 같은 diagnostic prefix로 Entry Spot ID도 발급한다.

```text
MeshNode RID:   <prefix>-<node-uuid-v4>
Entry Spot ID: <prefix>-entry-<lowercase-canonical-uuid-v4>
```

MeshNode와 Entry Spot은 각각 별도의 UUID v4를 생성한다. 두 UUID 값의 비교로 관계를 판정하지 않는다.
Entry Spot ID는 같은 MeshNode lifecycle 동안 유지하고 replacement lifecycle에서는 새로 발급한다. Global
Spot ID authority의 active conflict가 확인되면 두 번째 UUID나 reservation을 만들지 않고 startup을 즉시
configuration error로 끝낸다.

Full Entry Spot ID는 UTF-8 255 bytes 이하여야 한다. Prefix를 생략하면 MeshNode automatic RID에 선택한
기본 diagnostic prefix를 Entry Spot에도 사용한다.

MeshNode descriptor는 exact Entry Spot ID를 lifecycle generation과 함께 게시한다. Actor placement와
Entry Spot join은 이 mapping을 사용하며 prefix나 `entry` marker를 parsing해 node 관계를 계산하지 않는다.
Prefix와 marker는 진단 정보이며 stable host identity, shard나 placement key가 아니다.

### 3.3 Fixed RID

Fixed RID는 [Location Store](01-glossary.ko.md#location-store)의 MeshNode descriptor와 [automatic discovery](01-glossary.ko.md#automatic-discovery)를 사용하지
않는 explicit manual topology에서만 허용한다.

Object role이 `Client` 또는 `Server`인 MeshNode에 fixed RID를 설정하거나 automatic
mode와 fixed RID를 함께 설정하면 startup configuration error다.

## 4. Object role과 등록할 수 있는 기능

MeshNode마다 object role을 한 번 선택한다.

| Object role | Logical object operation | Local [factory](01-glossary.ko.md#factory)와 Entry Spot | 새 object를 배치할 target |
|---|---|---|---|
| `None` | Object operation을 제공하지 않는다. | 만들지 않는다. | 후보에서 제외한다. |
| `Client` | Object create, find와 message를 시작할 수 있다. | 만들지 않는다. | 후보에서 제외한다. |
| `Server` | `Client`가 제공하는 기능을 모두 포함한다. | 등록한 type의 object와 Entry Spot을 host한다. | 등록한 type과 다른 placement 조건이 맞으면 후보가 된다. |

`Client`와 `Server` role에는 Location Store가 필요하다. `None`을 선택하면 object
manager, factory, placement 기능이나 숨겨진 local object runtime을 만들지 않는다.
Factory와 Entry Spot은 `Server` builder에서만 등록할 수 있다. Entry Spot ID는
Framework가 발급하며 caller가 생성하거나 fixed RID를 지정할 수 없다.

Object Client는 Spot·Actor factory와 application Node direct handler를 등록할 수
없다. Object 기능과 독립된 RouteMesh Channel Server는 같은 MeshNode에 등록할 수
있다. 이 조합은 object placement target이 아니지만 Channel target은 된다.

두 MeshNode가 모두 Object Client이고 양쪽 모두 RouteMesh Channel Server membership이
없을 때만 peer connection을 만들지 않는다. 어느 한쪽에 Server membership이 있으면
weight가 `0`이어도 연결한다. Channel Client membership만 있는 pair는 연결하지
않는다.

다음 .NET 발췌는 공통 role 선택과 factory 등록을 이해하기 위한 예시다. 다른 언어에
같은 signature를 요구하지 않으며, 정확한 .NET 계약은
[.NET configuration interface](server/languages/dotnet/interfaces/03-configuration-topology.ko.md)가
정의한다.

```csharp
public interface IZLinkMeshNodeBuilder
{
    IZLinkMeshNodeBuilder SetPlacementWeight(int weight);
    IZLinkMeshObjectRoleBuilder Objects(); // Object role 선택을 시작한다.
}

public interface IZLinkMeshObjectRoleBuilder
{
    IZLinkMeshObjectClientBuilder Client(); // Object operation만 시작할 수 있다.
    IZLinkMeshObjectServerBuilder Server(); // Factory와 Entry Spot도 등록할 수 있다.
}

public interface IZLinkMeshObjectServerBuilder
{
    IZLinkMeshObjectServerBuilder AddEntrySpot<TEntrySpot>()
        where TEntrySpot : class, IZLinkEntrySpot;
    IZLinkMeshObjectServerBuilder AddActorFactory<TActor, TFactory>(
        string actorType,
        Action<IZLinkActorFactoryBuilder<TActor>> configure)
        where TActor : class, IZLinkActor
        where TFactory : class, IZLinkActorFactory<TActor>;
}

public interface IZLinkActorFactoryBuilder<TActor>
    where TActor : class, IZLinkActor
{
    IZLinkActorFactoryBuilder<TActor> DisableRelocation();
    IZLinkActorFactoryBuilder<TActor> RecreateOnRelocation();
    IZLinkActorFactoryBuilder<TActor> PreserveStateWith<TAdapter>()
        where TAdapter : class, IZLinkActorRelocationAdapter<TActor>;
}
```

```csharp
var mesh = options
    .AddRouteMesh("world")
    .SetPlacementWeight(100); // 새 object의 placement 선택에만 사용하는 weight다.

mesh.Objects()
    .Server()
    .AddActorFactory<PlayerActor, PlayerActorFactory>(
        "player",
        factory => factory.RecreateOnRelocation());
        // Stable type, factory와 relocation 방식을 한 등록에 함께 고정한다.
```

Actor, User Spot과 Instance Spot factory를 등록할 때는 다음 두 값을 반드시 함께
지정한다.

- UTF-8 `1..255` bytes의 [stable type](01-glossary.ko.md#stable-type)
- `DisableRelocation`, `RecreateOnRelocation`, `PreserveStateWith` 중 하나의 relocation 방식

Framework는 factory 등록 호출 안에서 configure callback을 동기적으로 한 번 실행한다.
Callback이 정상 반환하면 구성을 고정하며, 이후 보관해 둔 builder를 다시 호출하면
configuration error다. Callback이 예외를 던지면 해당 factory를 등록하지 않고 그 예외를
호출자에게 전달한다.

Stable type은 대소문자를 구분하는 exact value다. Framework는 normalization을
적용하지 않으며 언어의 class FQN을 wire나 Store identity로 사용하지 않는다. 같은
`(object kind, stable type)` 조합을 두 번 등록하면 startup 오류다.

Relocation policy를 생략하는 overload나 compatibility default는 제공하지 않는다.

## 5. Object placement capability

Object Server의 MeshNode descriptor에는 node 전체에 적용할 placement weight,
Actor·Spot capacity projection과 등록한 type별 capability가 포함된다.

### 5.1 Weight와 capacity

| 항목 | 계약 |
|---|---|
| Placement [weight](01-glossary.ko.md#weight) | 범위는 `0..10000`, 기본값은 `100`이다. Channel weight와는 별개다. 범위 밖 값은 startup 설정과 runtime 변경에서 configuration error다. |
| Node별 Actor limit | 기본값 `0`은 제한 없음이다. 양수이면 `1..2^31-1` 범위의 최대 Actor 수다. 음수는 startup configuration error다. |
| Node별 Spot limit | 기본값 `0`은 제한 없음이다. 양수 범위는 `1..2^31-1`이며 User Spot과 Instance Spot을 합산한다. 음수는 startup configuration error다. |
| Spot stable type별 limit | 기본값 `0`은 제한 없음이다. 양수 범위는 `1..2^31-1`이며 해당 User·Instance Spot type에 적용한다. 음수는 startup configuration error다. |
| Entry Spot | Object Server node마다 하나로 고정하며 configurable Spot limit에서 제외한다. |
| Pending activation | 기본값 `128`이며 object population이 아니라 동시에 진행되는 activation admission을 제한한다. |

새 object를 만들거나 기존 object를 다른 node로 옮길 때는 Framework가 target
MeshNode를 선택한다. Placement weight가 `0`인 MeshNode는 이 두 작업의 새 target
후보에서 제외한다.

이미 그 MeshNode에 존재하는 object로 보내는 message는 target을 새로 선택하는
작업이 아니므로 이 weight만으로 차단하지 않는다. Weight를 `0`으로 바꿔도 이미
확정된 reservation을 취소하지 않는다.

Framework는 Active count와 reserved slot을 합해 설정한 Actor·Spot limit을 먼저
검사하고 그 뒤에 weight를 적용한다. Limit `0`은 검사를 생략한다. Capacity 조건을
만족하는 node가 하나도 없으면 `CapacityExceeded`다. Descriptor의 count는
후보 선택용 projection이며 Location Store의 atomic reservation이 최종 판정이다.
남은 후보의 positive placement weight 합계는 최소 64-bit 정수로 계산하여
overflow하지 않게 한다.

Startup builder, runtime option, MeshNode descriptor와 monitoring snapshot은 같은
weight와 capacity 값을 사용한다.

### 5.2 Target node를 선택하는 조건

Logical create의 caller는 target RID, predicate 또는 placement callback을 지정하지
않는다. `InMesh`를 지정한 경우에도 target node가 아니라 후보를 찾을 Mesh만
선택한다.

Framework는 다음 조건을 사용해 target을 선택한다.

1. Node가 `Serving` 상태인지 확인한다.
2. Current owner lease가 유효한지 확인한다.
3. 요청한 object kind와 stable type을 등록했는지 확인한다.
4. Active·pending capacity가 남아 있는지 확인한다.
5. 남은 후보의 node-wide placement weight를 적용한다.

Framework는 선택한 node에 object를 배치할 자리를 예약하여 다른 생성 작업과
capacity를 중복 사용하지 않게 한다. Application에 target RID나 owner token을
선택하도록 요구하지 않는다.

`GetOrCreate`가 이미 Ready인 object를 찾았다면 current owner의 capacity와 weight를
다시 적용하지 않는다.

## 6. 등록과 startup 순서

Framework는 MeshNode를 다음 순서로 시작한다.

1. `MeshName`, object role, routing mode, endpoint, Channel set, factory, stable type,
   relocation policy, factory option과 capacity를 검증한다.
2. Location Store가 필요한 role이면 host [owner lease](01-glossary.ko.md#owner-lease)를 확보하고 automatic RID의
   MeshNode descriptor owner CAS를 완료한다.
3. ROUTER를 bind한 뒤 다른 peer에 게시할 실제 endpoint를 확정한다.
4. 필요한 정보를 모두 포함한 MeshNode descriptor를 게시하고 어떤 peer와 연결해야
   하는지 계산한다.
5. Peer admission, local handler와 object runtime 준비를 마친 뒤 `Serving` 상태와
   신규 target selection을 공개한다.

Object role을 사용하는 host는 Location Store를 명시적으로 등록해야 한다.

Manual mode에서는 application이 endpoint를 제공한다. Expected RID를 사용할
구성이라면 expected RID와 endpoint를 함께 제공한다. Manual mode는 object runtime을
제공하지 않는다.

## 7. Peer admission과 메시징

### 7.1 Peer 연결

Peer handshake에서는 다음 정보를 교환한다.

- `MeshName`
- RID
- [Lifecycle generation](01-glossary.ko.md#lifecycle-generation)
- [Descriptor revision](01-glossary.ko.md#descriptor-revision)
- 변경할 수 없는 `ChannelName` set
- Security identity

`MeshName` 또는 trust profile이 다르거나 같은 lifecycle identity의 중복 pipe이면
admission하지 않는다.

Lifecycle generation은 0이 아닌 opaque equality token이다. 숫자 크기로 어느
lifecycle이 더 새로운지 판단하지 않는다.

Manual topology에서 fixed RID로 다시 연결할 때는 다음 조건을 모두 만족한 뒤 다른
generation의 connection을 target selection에 포함한다.

1. Application 구성에 해당 peer와 연결하려는 의도가 있다.
2. 인증된 connection handover를 완료했다.
3. Service liveness 확인으로 이전 pipe가 종료되었음을 확정했다.

Automatic RouteMesh에서는 RID가 더 작은 MeshNode만 connect를 시작한다. Manual
양방향 connect 또는 automatic 연결 경합으로 중복 후보가 생기면
[RouteMesh의 중복 peer 연결 규칙](07-channel-topology.ko.md#51-automatic은-rid가-더-작은-meshnode만-연결을-시작한다)에
따라 [ready](01-glossary.ko.md#ready) connection 하나만 유지한다.

### 7.2 Channel weight 갱신

Handshake는 Channel별 weight도 전달한다. Channel weight를 실행 중에 바꾸면
lifecycle generation은 유지하고 descriptor revision만 증가한다.

Peer는 현재 generation에서 더 큰 revision이 붙은 전체 weight [snapshot](01-glossary.ko.md#snapshot)만 적용한다.
Weight 변경은 connection을 다시 만들거나 application message를 replay하지 않으며,
node-wide placement weight도 바꾸지 않는다.

### 7.3 메시징 방식별 target 선택

| 메시징 방식 | Target을 선택하고 전달하는 방법 |
|---|---|
| Node direct | Caller가 지정한 `MeshName` 안에서 exact target RID로 한 번 제출한다. Object Client RID는 application Node direct target이 아니다. |
| Channel | Process-local `ChannelName` index가 RouteMesh를 정한다. 그 Mesh에서 ready 상태이고 Channel weight가 0보다 큰 Server 중 하나를 weight 비율에 따라 선택한다. |
| Logical Multicast | 먼저 해당 ChannelName에 참여하고 ready 상태이며 Channel weight가 0보다 큰 remote MeshNode를 모두 선택한다. 각 수신 MeshNode는 자신의 local Spot 중에서 ChannelName과 topic 조건이 일치하는 subscription에 message를 전달한다. |
| Actor direct | Global ActorId의 current Ready authority를 확인한 뒤 current owner route로 제출한다. |
| Spot direct | Global SpotId의 current [Ready](01-glossary.ko.md#ready) [authority](01-glossary.ko.md#authority)를 확인한 뒤 current [owner route](01-glossary.ko.md#owner-route)로 제출한다. |

Actor·Spot direct는 logical ID만 target으로 사용한다. `ObjectGeneration`을 어디에 쓰고 어디에
쓰지 않는지는
[Spot·Actor routing §2.5](18-object-routing.ko.md#25-objectgeneration을-어디에-쓰고-어디에-쓰지-않는가)가
정한다.

Target 선택과 message submit은 하나의 operation이다. Framework가 선택한 RID 목록을
application에 반환한 뒤 별도 send를 요구하지 않는다.

Node·Channel·Actor·Spot send와 request는 같은 MeshNode ROUTER를 사용한다. Classic
fanout은 별도의 PUB/SUB socket 계약이며 MeshNode membership에 포함하지 않는다.

[Node direct](01-glossary.ko.md#node-direct)는 exact `MeshName`과 RID가 operation 의미에 포함되는 infrastructure,
진단 또는 manual topology에 사용한다. 여러 node가 같은 기능을 제공하는 application
request에는 [ChannelName](01-glossary.ko.md#channelname)을 사용한다.

Actor와 Spot 메시징은 global ActorId 또는 SpotId를 target으로 사용한다. Caller는
NodeRid나 MeshName을 target으로 넘기지 않는다.

기존 Actor·Spot의 current `MeshName`과 NodeRid는 Location Store authority가
제공한다. Missing [Instance Spot](01-glossary.ko.md#entry-spot-user-spot과-instance-spot)에서만 [Spot direct](01-glossary.ko.md#spot-direct) fluent call의 Instance intent에
optional initial Mesh와 stable type을 지정할 수 있다. Initial Mesh는 cold activation
placement에만 사용하며 기존 owner의 현재 Mesh를 제한하거나 이동시키지 않는다.

Application payload는 owner의 application turn에서 직렬로 처리한다. Request completion과
liveness·admission·relocation·reply recovery service control은 기존 Completion connection에서 받고
send-ready는 Core callback으로 전달한다. Location reconcile과 reservation 같은 Framework 내부 작업도
application handler가 대기 중이어도 진행한다. Actor·Spot lifecycle application callback은 application turn에서 실행한다.
Transport readiness callback에서 application handler를 직접 실행하지 않는다.

ChannelName handler와 RID direct handler는 서로 다른 namespace를 사용한다.

- Channel handler context는 `ChannelName`과 reply source identity를 내부에
  보존한다. 업무 코드에 `MeshName`이나 물리 route 선택을 노출하지 않는다.
- RID direct handler context는 direct route의 `MeshName`과 source RID를 제공한다.

Spot [Logical Multicast](01-glossary.ko.md#logical-multicast)는 `(ChannelName, topic filter)` [subscription](01-glossary.ko.md#subscription)을 node-local로
검사한다. 송신 MeshNode는 target Channel의 remote node마다 routed message를 한 번
제출한다. 수신 MeshNode는 일치하는 local Spot마다 같은 immutable message storage의
reference를 확보하여 Spot queue에 넣는다.

## 8. Drain과 종료

`Relocating` node는 다음 신규 작업의 target에서 제외한다.

- Channel selection
- Object create와 membership
- Relocation

아직 relocation permit을 얻지 못한 unit의 existing owner message와 timer는 계속
처리한다. Unit별 seal을 마치고 source의 application dispatch를 모두 닫으면
`Draining`으로 전환한다.

이미 reservation을 끝낸 create, accepted message, completion과 relocation barrier는
정해진 deadline과 fence에 따라 terminal 상태까지 진행한다. 전체 종료와 handoff
순서는 [Host relocation와 shutdown](28-graceful-drain-handoff.ko.md)이 정의한다.

`Shutdown`은 새 relocation을 시작하지 않는다. `Relocate`는 등록한 relocation policy에
따라 Actor, User Spot aggregate와 Instance Spot을 이전한다.

Node weight를 `0`으로 바꾸거나 drain을 시작했다는 이유로 기존 object를 숨겨 다시
만들거나 application payload를 다른 owner에게 새 operation으로 제출하지 않는다.

## 9. 관측 정보

Runtime snapshot과 event는 다음 정보를 제공한다.

- `MeshName`, RID, lifecycle generation과 endpoint
- Object role과 node-wide placement weight
- Active·pending·maximum capacity
- Type capability와 reservation failure
- Drain state

RID와 endpoint는 진단 정보로만 사용하며 metric label로 사용하지 않는다. 세부 계약은
[Runtime monitoring](24-runtime-monitoring.ko.md)이 정의한다.

## 10. 구현 및 contract test 검증 요구

- 같은 process의 중복 `MeshName`과 잘못된 object role 구성이 startup에서 실패한다.
- `None`, `Client`, `Server`가 manager, factory와 placement capability를 계약대로 제한한다.
- Object role과 Location Store, automatic discovery와 fixed RID의 잘못된 조합이 startup에서 실패한다.
- Automatic RID가 prefix와 lowercase canonical UUID v4 형식을 따르고 active conflict에서 두 번째 claim
  없이 startup configuration error로 실패한다.
- Replacement lifecycle이 새 RID를 사용한다.
- Entry Spot ID가 MeshNode와 같은 diagnostic prefix, 별도로 생성한 UUID v4를 사용하며 descriptor가 exact
  lifecycle mapping을 게시한다.
- Replacement lifecycle이 새 Entry Spot ID를 발급하고 Entry Spot authority 충돌에서 즉시 실패한다.
- Stable type 중복과 relocation policy 생략이 startup에서 실패한다.
- Placement weight는 `0`, 기본값 `100`과 상한 `10000`을 허용하고 `-1`과 `10001`은
  startup 설정과 runtime 변경에서 거부한다.
- Capacity가 weight보다 먼저 적용되고 weight `0`이 existing object와 accepted reservation을 취소하지 않는다.
- Channel weight 변경이 placement weight를 바꾸지 않는다.
- Channel select-one이 Channel weight와 drain을 반영하고 Node direct에는 영향을 주지 않는다.
- Logical Multicast가 remote node마다 한 번 전송되고 node-local Spot queue가 immutable storage를 공유한다.
- ChannelName handler와 RID direct handler의 namespace 및 context가 구분된다.
- Draining node가 새 placement target이 되지 않고 accepted operation은 terminal 상태까지 진행한다.
- Actor·Spot application 호출이 NodeRid나 owner token을 target으로 요구하지 않는다.
