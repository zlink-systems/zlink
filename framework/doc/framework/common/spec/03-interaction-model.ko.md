---
title: "ZLink Framework 상호작용 모델"
---

# ZLink Framework 상호작용 모델

[스펙 목차](README.ko.md) · [이전: ZLink Framework 개요](02-overview.ko.md) · [다음: Framework 메시지 계약](04-message-model.ko.md)

> **이 장이 정의하는 것** — Framework operation의 대상, 완료 의미와 실행 owner.


## 1. 목적

이 문서는 ZLink Framework operation의 대상, 완료 의미와 실행 owner를 정의한다. 정확한 언어별 메서드
시그니처는 각 package의 `languages/<lang>/` 문서가 소유한다.

같은 Channel에 참여한 여러 remote node와 local Spot에 message 하나를 게시하는
방식을 `Logical Multicast`라 한다. Location Store가 global [Spot](01-glossary.ko.md#spot)이나 Actor를 현재
어느 node가 처리하는지 기록한 값을 `authority`라 한다.

## 2. 공통 모델

| 모델 | 대상 선택 | 호출자가 관찰하는 완료 |
|---|---|---|
| node direct send | Caller가 같은 `MeshName`에 속한 RID 하나를 직접 지정한다. | Source-local queue가 message를 수락하면 반환 데이터 없이 완료한다. |
| [node direct](01-glossary.ko.md#node-direct) request | Caller가 같은 `MeshName`에 속한 RID 하나를 직접 지정한다. | Reply, timeout 또는 route 오류 가운데 하나로 완료한다. |
| channel send | Framework가 `ChannelName`에 등록된 RouteMesh 또는 ClientServer 송신 경로에서 ready target 하나를 선택한다. | 선택한 송신 경로의 source-local queue가 수락하면 반환 데이터 없이 완료한다. |
| channel request | Framework가 `ChannelName`에 등록된 [RouteMesh](01-glossary.ko.md#routemesh) 또는 ClientServer 송신 경로에서 [ready target](01-glossary.ko.md#ready-target) 하나를 선택한다. | Reply, timeout 또는 route 오류 가운데 하나로 완료한다. |
| [Logical Multicast](01-glossary.ko.md#logical-multicast) | Framework가 `ChannelName`의 remote member와 local Spot 중에서 조건에 맞는 대상을 선택한다. | Bounded worker와 source-local capacity를 확보해 publish transaction을 시작하면 반환 데이터 없이 완료한다. Target별 제출과 handler 완료를 기다리지 않는다. |
| Spot message | Caller가 global Spot ID를 지정하고 Framework가 current Ready [authority](01-glossary.ko.md#authority)의 [owner](01-glossary.ko.md#owner)를 찾는다. | Send는 source-local queue 수락 뒤 반환 데이터 없이, request는 reply 결과로 완료한다. |
| Actor message | Caller가 global Actor ID를 지정하고 Framework가 current [Ready](01-glossary.ko.md#ready) authority의 owner를 찾는다. | Send는 source-local queue 수락 뒤 반환 데이터 없이, request는 reply 결과로 완료한다. |
| Object create·get-or-create | Caller가 global ID와 stable type을 지정하고 필요하면 placement intent를 추가한다. | Exact `ActorRef`·`SpotRef` 또는 typed creation 오류를 반환한다. |
| classic fanout | Framework가 준비된 subscriber 집합을 대상으로 사용한다. | Local publisher queue가 수락하면 반환 데이터 없이 완료한다. |
| STREAM | Caller가 session RID로 식별되는 연결을 사용한다. | One-way packet은 local queue 수락 뒤 반환 데이터 없이 완료하고 request는 reply를 반환한다. |

Channel operation에서 Framework가 조건에 맞는 target 하나를 고르는 방식을
`select-one`이라 한다.

### 2.1 상호작용을 시작하는 public interface

다음 표는 application이 각 상호작용을 어디서 시작하는지 보여준다. `client`는 DI나 현재 handler context를
통해 얻으며, application이 transport socket이나 endpoint를 직접 선택하지 않는다.

| 상호작용 | 시작 interface | 호출자가 지정하는 대상 |
|---|---|---|
| Node direct·Channel select-one | `IZLinkRouteClient` | Node direct는 MeshName과 target RID, Channel은 ChannelName |
| Spot send·request | `IZLinkSpotClient` | Global Spot ID |
| Actor send·request | `IZLinkActorClient` | Global Actor ID |
| User Spot 생성·조회 | `IZLinkSpotManager` | Stable Spot type과 필요하면 global Spot ID |
| Actor 생성·조회 | `IZLinkActorManager` | Global Actor ID와 stable Actor type |
| Logical Multicast | `IZLinkSpotPublisherClient` | ChannelName과 topic |
| Classic fanout | `IZLinkFanoutClient` | Fanout ChannelName과 optional topic |
| STREAM send·reply | `IZLinkSessionClient` | 현재 STREAM session |

아래 코드는 공통 상호작용의 모양을 보여 주기 위해 .NET 표기로 줄인 설명용 선언이다. 언어별 정확한
시그니처는 [.NET Channel messaging](server/languages/dotnet/interfaces/04-channel-messaging.ko.md),
[.NET Spot](server/languages/dotnet/interfaces/05-spots.ko.md),
[.NET Actor](server/languages/dotnet/interfaces/06-actors.ko.md)와
[.NET STREAM session](server/languages/dotnet/interfaces/07-stream-session.ko.md)이 소유한다.

```csharp
public interface IZLinkRouteClient
{
    // Caller가 Mesh와 target node를 모두 지정하는 direct operation이다.
    IZLinkSendCall SendToNode<T>(string meshName, RoutingId targetNodeRid, T message);
    IZLinkRequestCall RequestToNode<T>(string meshName, RoutingId targetNodeRid, T request);

    // Framework가 ChannelName의 ready target 하나를 선택하는 select-one operation이다.
    IZLinkSendCall SendToChannel<T>(string channelName, T message);
    IZLinkRequestCall RequestToChannel<T>(string channelName, T request);
}

public interface IZLinkSpotClient
{
    // Framework가 global Spot ID의 current owner를 찾아 전송한다.
    IZLinkSpotSendCall SendToSpot<T>(string spotId, T message);
    IZLinkSpotRequestCall RequestToSpot<T>(string spotId, T request);
}

public interface IZLinkActorClient
{
    // Framework가 global Actor ID의 current owner를 찾아 전송한다.
    IZLinkActorSendCall SendToActor<T>(string actorId, T message);
    IZLinkActorRequestCall RequestToActor<T>(string actorId, T request);
}

public interface IZLinkSpotPublisherClient
{
    // 조건에 맞는 remote MeshNode와 local Spot subscription에 함께 게시한다.
    IZLinkPublishCall Publish<T>(string channelName, string topic, T message);
}

public interface IZLinkSpotManager
{
    // Create는 새 RID를 발급하고, GetOrCreate는 application이 지정한 RID를 사용한다.
    IZLinkSpotCreateCall Create(string spotType);
    IZLinkSpotGetOrCreateCall GetOrCreate(string spotId, string spotType);
}

public interface IZLinkActorManager
{
    // Actor create 계열은 global Actor ID와 stable Actor type을 항상 함께 받는다.
    IZLinkActorCreateCall Create(string actorId, string actorType);
    IZLinkActorGetOrCreateCall GetOrCreate(string actorId, string actorType);
}

public interface IZLinkFanoutClient
{
    // Classic fanout 전용 publisher transport에 event를 제출할 call을 만든다.
    IZLinkFanoutPublishCall Publish<T>(string channelName, string topic, T message);
}

public interface IZLinkSessionClient
{
    // Send는 server-initiated packet, Reply는 현재 STREAM request의 reply다.
    IZLinkSessionSendCall Send<T>(T message);
    IZLinkSessionReplyCall Reply<T>(T message);
}
```

`Send...` call은 `Async()`로 local outbound admission까지 기다리고 결과값 없이 완료한다. `Request...` call은
`Async<TReply>()`로 reply를 기다린다. `Yield<TReply>()`가 선언된 언어에서도 이 operation은
`SpotWide` User Spot 또는 Instance Spot의 shared turn에서만 사용할 수 있다.

## 3. Node direct와 channel select-one

Node direct는 infrastructure와 명시적 owner routing에 사용한다. Target RID가 현재 Mesh member가 아니면
`NotFound`, member이지만 pipe가 준비되지 않았으면 send readiness 한계까지 기다린 뒤
`Unavailable`로 끝난다. Node direct operation은 실패한 request를 다른 node에 자동으로 다시
보내지 않는다. Global Spot·Actor message는 cached Ready route와 committed Message Follow route만 사용한다.
Message Follow 제한 안에서 current owner로 relay할 수 없으면 `Unavailable`로 끝내며 source가 Store를 읽어
다른 owner에게 같은 operation을 다시 제출하지 않는다.

Channel operation은 ChannelName으로 process-local 송신 경로를 먼저 결정한다. RouteMesh 경로는 호출 순간의
ready member 가운데 weight가 0보다 큰 하나를 고르고, ClientServer 경로는 ready server 가운데 하나를
고른다. 선택과 submit 사이에 application callback을 두지 않는다. [Weight](01-glossary.ko.md#weight) 0은 새 channel 선택에서
제외하며, RouteMesh에서는 Logical Multicast remote target에서도 제외한다. RID direct와 이미 제출한
operation에는 영향을 주지 않는다.

Select-one의 non-blocking submit이 capacity 부족으로 수락되지 않은 경우, 그 첫 선택은 public
target commitment가 아니다. Framework service runtime은 send-ready 이후 성공한 admission 전까지 같은 [ChannelName](01-glossary.ko.md#channelname)의 현재
eligible member 가운데 다른 target을 선택할 수 있다. Target은 transport queue가 operation을 수락한 시점에
확정되며, 그 이후에는 같은 operation을 다른 member에게 replay하지 않는다. Direct call은 이 재선택
규칙을 사용하지 않는다. Node direct는 RID, Spot·Actor는 global ID, session은 binding token을 유지하며 물리
peer lifecycle generation을 public target identity로 노출하지 않는다.

같은 ChannelName을 여러 물리 송신 경로에 등록할 수 없으므로 호출자는 MeshName이나 ClientServer 종류를
지정하지 않는다. 같은 process에서 ChannelName을 서로 다른 topology에 등록하면 host startup이
설정 오류로 실패한다.

Node direct는 [MeshName](01-glossary.ko.md#meshname)·RID를 계속 사용한다. Logical Multicast 호출자는 ChannelName과 topic만 지정하며
process-local channel index가 owner RouteMesh의 MeshNode를 결정한다. 선택된 owner MeshName은 내부
routing과 runtime monitoring에서만 관측한다.

## 4. Send와 request

`send`는 reply가 없는 one-way operation이다. Public call은 비동기 submit 하나만 제공하며, 즉시 한 번만
시도하는 동기 terminator는 제공하지 않는다. 반환은 destination handler가 실행되었다는 확인이 아니라
Framework가 message를 local outbound queue에 받아들였는지를 나타낸다. Queue가 일시적으로 가득 차면
유한한 send timeout까지 admission을 기다린다. 이미 수락한 뒤 발생한 one-way 오류는 runtime error sink와
monitoring으로 보고한다.

Global Spot·Actor send도 같은 비동기 terminator를 사용한다. Source는 current Ready authority를 resolve하고
local outbound admission으로 submit을 완료한다. Cache hit도 같은 public 의미를 유지하므로 cache 상태에 따라
동기 submit을 제공하거나 caller에게 owner node와 generation을 요구하지 않는다. Message call은 Missing
object의 creation intent를 기본적으로 만들지 않는다. Spot 전용 fluent call에서 Instance intent를 명시한
경우에만 실행 중인 Instance Spot이 없을 때 새 Spot을 만들고 최초 message를 처리할
수 있게 준비한다. 이 과정을 `cold activation`이라 한다. 시작 method는 계속 global
[Spot ID](01-glossary.ko.md#spot-id)만 받으며 optional [stable type](01-glossary.ko.md#stable-type)과 initial Mesh는 fluent call의
[cold activation](01-glossary.ko.md#cold-activation) option이다.

유효한 one-way call은 source-local admission이 성공하면 결과값 없이 완료한다. Send timeout까지 capacity를
확보하지 못하면 `DeadlineExceeded`, target·route 부재와 runtime shutdown은 operation-specific exception으로
완료한다. 잘못된 argument·handle·state와 중복 submit도 local exceptional completion이다. Cancellation은
언어별 cancelled awaitable로 표현한다. 어느 terminal 완료 뒤에도 Framework가 operation을 자동으로 다시
제출하지 않는다.

`request`는 선택한 송신 경로에 reply correlation을 만들고 terminal 결과를 정확히 한 번 전달한다. request timeout은 reply를
기다리는 시간이다. 전송 단계의 backpressure는 send timeout이 담당한다. route 오류나 timeout으로 끝난
request를 Framework가 자동 재전송하지 않는다. 언어별 transport 오류는 이 문서의 닫힌 Framework 결과 가운데 하나로 변환하며
transport 전용 결과를 public call에 노출하지 않는다.

`Send`와 `Request`가 반환하는 공통 kind, timeout과 cancellation의 정확한 의미는
[Framework 오류 모델](32-framework-error-model.ko.md)이 정의한다.

다른 RouteMesh 또는 ClientServer Channel로 보낸 request도 같은 단일 terminal completion 규칙을 따른다.
Spot에서 시작한 경우 Framework는 원래 Spot activation과 generation을 completion record에 보존하고, reply를
새 application message로 다시 dispatch하지 않는다.

같은 origin이 같은 destination pipe에 성공적으로 submit한 message는 FIFO다. 서로 다른 destination,
origin 또는 session 사이의 전역 순서는 보장하지 않는다.

## 5. Spot Logical Multicast

Logical Multicast publish는 target ChannelName, [topic](01-glossary.ko.md#topic)과 typed payload를 받는다. publish 시점에 remote
[MeshNode](01-glossary.ko.md#meshnode)와 local Spot match를 snapshot한다.

- remote MeshNode마다 routed message를 한 번 submit한다.
- 수신 MeshNode가 `(ChannelName, topic filter)`의 local subscription을 검사한다.
- 같은 node의 일치하는 Spot queue는 immutable payload storage의 reference를 공유한다.
- 다른 MeshNode로 relay하거나 과거 event를 replay하지 않는다.

Framework service runtime은 bounded I/O executor에 publish transaction을 제출한다. Send timeout까지
worker slot을 확보하지 못하면 transaction을 시작하지 않고 `DeadlineExceeded`로 실패한다. Handoff에 성공해
transaction이 시작되면 public terminal은 반환 데이터 없이 정상 완료하고, runtime은 각 remote target과
local Spot queue의 제출을 내부에서 계속한다.
Transaction 시작이 [snapshot](01-glossary.ko.md#snapshot) operation의 commit point이므로 cancellation이나 shutdown으로 남은 target 제출을
중단하지 않는다.
앞에서 수락된 remote target과 local Spot queue는 뒤 target의 실패 때문에 취소되지 않는다.

Snapshot target이 모두 0이어도 정상 완료한다. Transaction이 시작된 뒤 발생한 remote 연결 불가, outbound
capacity 부족과 local Spot queue drop은 이미 수락된 target을 rollback하거나 전체 publish를 retry하지 않는다.
Target별 수락·실패 결과는 public 결과로 반환하거나 publish 전용 monitoring 값으로 집계하지 않는다.

Publish 정상 완료는 transaction을 시작했다는 뜻이다. 고정한 snapshot의 target 제출, Spot handler 실행,
subscriber 수신 또는 remote ROUTER가 수락한 뒤 수신 MeshNode의 local Spot queue 수락을 보장하지 않는다.

## 6. Classic fanout

[Classic fanout](01-glossary.ko.md#classic-fanout)은 MeshNode와 독립된 publisher/subscriber channel이다. 현재 연결과 [subscription](01-glossary.ko.md#subscription) 준비가
완료된 subscriber에게만 새 event를 전달한다. publisher는 연결 전 또는 연결 단절 중 event를 저장하지
않고, 다시 연결된 뒤 replay하지 않는다.

Publisher call은 publisher socket send timeout까지 local admission을 기다리는 비동기 terminator 하나만
제공한다. Subscriber가 0이어도 local publisher queue가 event를 수락하면 결과값 없이 정상 완료한다. 이
완료는 subscriber 수신이나 handler 완료를 뜻하지 않는다.

Publish의 공통 입력은 ChannelName, topic과 typed event다. Typed event의 packet name을 topic으로
사용하는 편의 호출도 같은 operation을 만든다. 두 호출은 같은 publisher transport, timeout과
비동기 완료 규칙을 사용하며 subscriber dispatch는 [packet name](01-glossary.ko.md#packet-name)으로 handler를 선택하고 topic을 handler
context에 보존한다.

Publisher는 전용 location descriptor에 ChannelName과 실제 endpoint를 게시한다. Automatic subscriber는
같은 ChannelName의 live publisher를 모두 연결하고 다른 ChannelName이나 다른 [descriptor](01-glossary.ko.md#descriptor) kind는 연결하지
않는다. Manual subscriber는 명시한 endpoint만 연결한다.

Logical Multicast와 classic fanout은 모두 publish/subscribe 사용 경험을 제공하지만 전달 대상과 보장이
다르므로 별도 기능으로 등록한다.

## 7. Spot과 Actor

Spot은 MeshNode가 소유하는 logical mailbox다. Entry Spot과 Instance Spot의 direct message, Logical
Multicast, timer와 lifecycle callback은 각 Spot의 execution gate에서 직렬로 처리한다. User Spot은 기본
`SpotWide` mode에서 Spot·member Actor·timer·lifecycle callback 전체가 공통 gate를 사용하고, optional
`PerActor` mode에서는 Actor별, Spot lane별, timer별 gate를 사용한다. Node callback이 Spot queue를 대신
읽지 않는다.

[Instance Spot](01-glossary.ko.md#entry-spot-user-spot과-instance-spot)은 Actor membership이 없는 Spot kind다. Missing Instance 생성은 [Spot direct](01-glossary.ko.md#spot-direct) fluent call의
명시적인 [Instance intent](01-glossary.ko.md#instance-intent)만 시작한다. [Location Store](01-glossary.ko.md#location-store) reservation이 정한 owner 하나가 factory를 실행하고
durable activation inbox first record를 확정한 뒤 recovery root·cursor를 포함한 location `Ready`를 commit한다.
Framework는 first record를 local queue head로 복원한 뒤 activation barrier를 연다. Creating 경쟁자는 같은
attempt의 terminal 결과에 합류하며 별도 [factory](01-glossary.ko.md#factory)나 message를 시작하지 않는다.

`ActorRef`와 `SpotRef`는 global ID, ObjectGeneration, 조회 시점의 MeshName과 NodeRid를 담은 immutable location
snapshot이다. Endpoint, 내부 frame과 runtime resource는 포함하지 않는다. Bound session의 `Ref`/`ref()`
accessor는 Actor relocation의 route switch가 완료되면 같은 ActorId·ObjectGeneration과 target MeshName·NodeRid를
담은 새 immutable snapshot을 반환한다. 기존에 반환한 ref 값은 변경하지 않는다. 일반 message는 ref가 아니라
global ID를 사용하며 Framework가 current authority를 resolve한다.

Actor message는 global Actor ID의 current authority를 resolve한 뒤 Actor mailbox에 직접 추가한다. Actor
payload는 Spot message queue를 경유하지 않는다. Entry Spot Actor와 `PerActor` User Spot의 Actor는 Actor별
gate를 사용하고, `SpotWide` User Spot의 member Actor는 User Spot 공통 gate를 사용한다. Spot 소유 상태를
읽거나 바꿔야 하면 명시적인 Spot send/request를 제출하고 해당 Spot turn에서 처리한다.

Node, Spot과 Actor의 completion과 send-ready는 application handler가 대기 중이어도 진행할 수 있는
infrastructure 실행 영역에서 처리한다.

## 8. STREAM session

STREAM session은 연결 lifecycle과 packet 순서를 소유한다. Framework 내부 recv loop는 packet을
관리 queue에 넣은 뒤 session callback을 실행한다. 같은 session의 packet과 lifecycle callback은
직렬로 실행하며 서로 다른 session 사이의 전역 순서는 보장하지 않는다.

Session과 Actor가 bind되면 session ingress는 Actor mailbox로 complete message를 submit한다. Actor에서
client로 보내는 message는 현재 binding의 session FIFO를 사용한다. Actor 이동 중에는 session barrier가
old epoch와 new epoch의 순서를 구분한다.

Server package의 bound session send, session Actor relay와 명시적인 STREAM send·reply도 같은 async-only one-way admission
결과를 반환한다. 별도 stream connector package의 send builder는 connector package 계약을 따른다. STREAM
reply는 해당 STREAM socket의 send timeout을 사용하며 caller request timeout을 reply admission deadline으로
사용하지 않는다. Reply sequence 또는 one-shot token이 유효하지 않거나 같은 reply call을 두 번 제출하면
local exceptional completion으로 끝난다. 유효한 첫 reply terminator는 transport admission 전에 token을 원자적으로
소비한다. 이 terminator가 backpressure, timeout 또는 cancellation으로 완료되어도 token은 재사용하지
않는다. 같은 token에서 만든 두 call이 경쟁하면 하나만 transport admission을 시작한다.

## 9. 대표 public 호출 예제

다음 코드는 앞 절의 상호작용이 target을 어떻게 지정하고 어떤 terminal method로 끝나는지 비교한다.
`routes`, `spots`, `actors`, manager와 publisher는 DI로 받은 public client이고, RID와 ID는 application이
이미 가지고 있다고 가정한다. 업무 message type은 설명을 위한 예시다.

### 9.1 Node direct와 Channel select-one

```csharp
// Node direct: application이 "world" Mesh의 exact node RID를 지정한다.
await routes
    .SendToNode("world", targetNodeRid, new ReloadConfig())
    .Async(cancellationToken);

// Channel select-one: Framework가 "game" Channel의 ready server 하나를 선택한다.
MatchFound match = await routes
    .RequestToChannel("game", new FindMatch(playerId))
    .Timeout(TimeSpan.FromSeconds(2))
    .Async<MatchFound>(cancellationToken);
```

첫 번째 호출은 지정한 RID가 실패해도 다른 node를 선택하지 않는다. 두 번째 호출은 한 target이 operation을
수락하기 전까지만 같은 ChannelName의 다른 eligible target을 선택할 수 있다.

### 9.2 Spot·Actor message와 생성

```csharp
// Existing Spot: Framework가 Spot ID의 current Ready owner를 찾아 request를 보낸다.
RoomState room = await spots
    .RequestToSpot(roomId, new GetRoomState())
    .Timeout(TimeSpan.FromSeconds(1))
    .Async<RoomState>(cancellationToken);

// Missing Instance Spot: 명시한 intent가 있을 때만 Spot을 준비하고 같은 첫 request를 처리한다.
ShardState shard = await spots
    .RequestToSpot(shardRid, new LoadShard())
    .InstanceSpot("world-shard")
    .InMesh("world")
    .Async<ShardState>(cancellationToken);

// User Spot은 manager call로 명시적으로 만들거나 기존 creation attempt에 합류한다.
ZLinkSpotCreateResult createdSpot = await spotManager
    .GetOrCreate(roomId, "room")
    .InMesh("world")
    .Async(cancellationToken);

// Actor message는 member Spot의 message queue를 거치지 않고 Actor queue로 직접 들어간다.
PlayerState player = await actors
    .RequestToActor("player-42", new GetPlayerState())
    .Async<PlayerState>(cancellationToken);

// Actor 생성은 global Actor ID와 stable Actor type을 함께 지정한다.
// 결과는 기존 Actor 조회, 새 생성 승인, application 거절을 구분한다.
ZLinkActorCreateResult actorCreation = await actorManager
    .GetOrCreate("player-42", "player")
    .InMesh("world")
    .Async(cancellationToken);
```

일반 Spot·Actor message는 target node나 endpoint를 받지 않는다. `SpotRef`와 `ActorRef`에 NodeRid가 있어도
일반 message의 주소로 사용하지 않으며 Framework가 global ID의 current authority를 다시 확인한다.

### 9.3 Logical Multicast와 Classic fanout

```csharp
// Logical Multicast: 조건에 맞는 remote node와 local Spot subscription에 함께 게시한다.
await spotPublisher
    .Publish("world-events", "zone.7", new WeatherChanged("rain"))
    .Async(cancellationToken);

// Target별 제출 결과는 반환하거나 publish 전용 monitoring으로 집계하지 않는다.

// Classic fanout: 독립 publisher transport에 현재 연결된 subscriber를 대상으로 한다.
await fanout
    .Publish("telemetry", "server.health", new HealthSample(cpu, memory))
    .Async(cancellationToken);
```

두 publish 모두 handler 완료를 기다리지 않는다. Target별 제출 결과는 public 결과로 반환하거나
publish 전용 monitoring으로 집계하지 않는다.

### 9.4 STREAM session

```csharp
// 현재 session FIFO에 server-initiated one-way packet을 제출한다.
await sessionClient
    .Send(new ServerNotice("maintenance"))
    .Async(cancellationToken);

// STREAM request handler에서만 현재 request의 reply capability를 정확히 한 번 소비한다.
await sessionClient
    .Reply(new LoginAccepted(playerId))
    .Async(cancellationToken);

// Binding된 Actor relay는 현재 session binding을 사용해 Actor mailbox에 제출한다.
await sessionActor
    .RelayAsync(
        ZLinkMessage.From(new ClientInput(sequence, command)),
        cancellationToken);
```

`Reply(...)`는 임의의 server-initiated message를 보내는 API가 아니다. 현재 handler가 받은 STREAM request의
reply capability를 소비한다. 일반 server-initiated packet은 `Send(...)`를 사용한다.

## 10. Handler 실패

reply route를 복원할 수 있는 request는 구조화된 error reply로 완료한다. reply route를 복원할 수 없는
message와 one-way message는 drop하고 원인에 맞는 log, metric과 observer event를 남긴다. application
handler 예외는 one-way 경로에서도 error로 기록한다. observer 실패는 원래 reply 또는 drop 결과를
바꾸지 않는다.

## 11. 종료

`Relocate`가 `Relocating` intent를 게시하거나 `Shutdown`이 admission seal을 시작하면 새 channel 선택, Logical Multicast
target과 새 상태 배정을 제한한다. `Relocating`에서 permit을 얻지 못한 relocation unit은 기존 message와 timer를 계속
처리하며 queue turn 경계에서 permit을 얻은 뒤에만 seal한다. `Draining` 뒤에는 이미 admission한 message, request
completion, Actor relocation과 [STREAM session](01-glossary.ko.md#stream-session) barrier만 설정된 [deadline](01-glossary.ko.md#deadline)까지 진행한다. Deadline 뒤 남은 operation은
owner별 terminal [shutdown](01-glossary.ko.md#shutdown) 결과로 완료한다.

Draining MeshNode는 새 Instance placement 후보에서 제외된다. `Shutdown`은 기존 Instance Spot을 다른 node로
이동시키지 않고 수락된 turn을 deadline까지 처리한 뒤 정리한다. `Relocate`는 type별 maintenance policy와
authority transaction이 허용한 기존 owner만 target에 materialize한다. 두 operation 모두 Framework admission
seal과 current location authority를 검증하며 stale owner가 `Closing`이나 release를 적용하지 못하게 한다.
