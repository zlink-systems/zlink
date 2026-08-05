# Framework Common Sample Scenarios

이 디렉토리는 모든 framework 언어가 공유하는 샘플 시나리오를 정의한다.
언어별 샘플은 구현 방식과 문법은 달라도 이 문서의 서버 역할, 메시지 흐름,
메시지 필드, 검증 기준에 맞춘다.

공통 sample은 실제 업무 흐름의 정본이지만 새 public API 계약의 근거는 아니다.
공개 동작과 제약은 [공통 spec](../README.ko.md)이 소유하고, 이 문서는 그 계약을
실행 가능한 업무 흐름으로 보여 준다. 언어별 sample은 한 언어 구현을 기준으로
복사하지 않고 이 공통 시나리오와 해당 언어 spec을 함께 따른다.

Bingo, TicTacToe, SupportChat, DeliveryDispatch, ShoppingMall과 GameQuest는 다섯 framework 언어
(.NET, Java, Kotlin, Node.js, C++)가 공통으로 제공한다. ZoneWorld는 .NET과 Node.js가 제공하며
TypeScript 브라우저 client를 공유한다. 지원 언어는 같은 역할 분리, request/response/notify 이름,
상태 필드와 smoke 검증 순서를 따른다. 언어별 API 표현은 달라도 같은 framework 기능을 같은 순서로
확인할 수 있어야 한다.

언어별 guide는 공통 sample의 목적, 서버 구성, 메시지 계약, 상태 전이, 검증 순서를 다시
정의하지 않는다. 이런 내용을 언어별로 복사하면 공통 문서와 서로 다른 구현 기준이 생길 수
있기 때문이다. 언어별 DTO 표현을 따로 적어야 하는지는
[언어별 표현 기준](languages/README.ko.md)에 따라 판단한다. 실제 표현 차이가 없으면 언어별
샘플 문서를 만들지 않는다.

## 샘플 목록

| 샘플 | 목적 | 서버 구성 | 연결 방식 | Handler 등록 방식 | 기본 payload codec |
|------|------|-----------|-----------|-------------------|--------------------|
| [Bingo](bingo/README.ko.md) | 레벨별 matchmaking Instance Spot, session gateway, actor binding, room User Spot, timer와 bound push를 한 흐름으로 보여 준다. | `Session`, `Api`, `Matchmaking`, `Play` 분리 | location store 기반 자동 연결 | **자동 등록** | Protobuf |
| [TicTacToe](tictactoe/README.ko.md) | 2개 API와 2개 Play로 수동 endpoint scale-out, 공식 Redis Location Store 기반 room routing과 실시간 게임 흐름을 보여 준다. | `Api` 2개, `Play` 2개, 별도 `Session` 서버 없이 `Play`가 stream session을 함께 소유 | **수동 endpoint 연결** | **자동 등록** | JSON |
| [SupportChat](supportchat/README.ko.md) | 고객과 상담원이 같은 conversation Spot에서 대화하고, reconnect, idle timer, close, bound push를 확인한다. | `Session`, `Api`, `Support` 분리 | location store 기반 자동 연결 | **자동 등록** | JSON |
| [DeliveryDispatch](deliverydispatch/README.ko.md) | 배송 배차, timeout 재배정, 상태 push, 고객 stream push를 확인한다. | `Dispatch`, `CourierSession`, `CourierMeshNode` 2개, `Tracking`, `CustomerGateway` 분리 | location store 기반 자동 연결 | **자동 등록** | JSON |
| [ShoppingMall](event/shoppingmall.ko.md) | `CommerceApi`(HTTP edge)와 `OrderWorkflow`(주문 owner)를 분리해 event-sourced 주문 처리와 조회 모델을 구성한다. | `CommerceApi`, `OrderWorkflow` 분리 | location store 기반 자동 연결 | **자동 등록** | JSON |
| [GameQuest](event/gamequest.ko.md) | gameplay event를 player별 owner spot에 모아 event sourced quest aggregate와 조회 모델을 갱신한다. | `Session Server`, `PlayerQuestSpot` owner를 MeshNode로 분산 | location store 기반 자동 연결 | **자동 등록** | JSON |
| [ZoneWorld](zoneworld/README.ko.md) | zone 분할 MMORPG의 경계 이동(actor relocation)·경계 동기화·봇(bound session 없는 actor)과, 그것을 운영하는 관제 콘솔(runtime event·fanout 공지·노드 지정)을 브라우저 UI로 보여 준다. | `Gateway`, `ZoneNode` 2개, `Ops` 분리 | location store 기반 자동 연결 | **자동 등록** | JSON |

> ZoneWorld는 브라우저 UI로 zone 이동과 노드 관제를 확인한다. .NET과 Node.js server는 wire 계약이
> 같은 TypeScript client 하나를 공유한다. 실제 Chromium에서 `ws`·`wss`, request/reply, push,
> reconnect와 명시적 flow 전달을 검증한다.

## Channel 역할과 물리 topology 기준

Channel send/request는 ChannelName 하나로 대상을 지정한다. 샘플은 MeshName을 숨기는 application helper를
추가하지 않고 각 언어의 정식 Channel client를 직접 사용한다. RouteMesh와 ClientServer 선택은 개별 호출
하나가 아니라 두 process 역할 사이의 전체 업무 방향과 상태 주소 메시징 필요 여부로 결정한다.

- Spot·Actor 직접 메시징, actor relocation 또는 Logical Multicast가 필요한 역할은 RouteMesh를
  사용한다.
- 두 역할이 서로 독립적인 업무 send/request를 시작하면 하나의 RouteMesh peer 연결을 양방향으로 사용한다.
  호출 방향마다 ClientServer Channel을 만들어 연결을 중복하지 않는다.
- 한쪽만 업무 호출을 시작하고 두 역할이 공유할 RouteMesh가 없을 때 ClientServer Channel을 사용한다.
- `Client()`는 송신 경로만 등록하고 `Server()`만 handler와 weight를 제공한다. `SetWeight(0)`으로 client
  역할을 표현하거나 가짜 ChannelName membership을 추가하지 않는다.
- Object 역할을 `Client()`로만 등록한 MeshNode끼리는 연결하지 않는다. Manual endpoint를 등록해도
  이 제한을 우회하지 않는다. 서로 호출할 이유가 없는 client끼리 연결하면 설정 오류를 감추고
  불필요한 socket만 유지하기 때문이다.
- Object Client process가 channel Server도 제공해야 하면 그 channel은 독립 ClientServer topology로
  등록한다. Object Client RouteMesh에 channel Server 역할을 섞지 않는다.
- 로컬 sample runner는 기본 BindHost `127.0.0.1`과 automatic port 0을 사용한다. Container·Kubernetes
  배포는 `ConfigureNetwork()`에 Pod 또는 Service에서 remote peer가 연결할 AdvertiseHost를
  명시한다. Wildcard BindHost를 descriptor의 advertised endpoint로 기록하지 않는다.
- Sample은 특정 MeshNode의 `NodeRid`를 설정값, message field나 업무 상수로 미리 공유하지 않는다.
  Actor·Spot 생성과 direct messaging은 global `ActorId`·`SpotId`와 Location Store 기반 배치를
  사용하며, caller가 owner node를 선택하거나 owner `NodeRid`를 전달하지 않는다. 수동 topology도
  peer endpoint만 구성하고 상대 `NodeRid`를 application route로 사용하지 않는다.
- Node direct messaging은 Admin·Ops 관리 기능에서만 사용한다. 이 경우에도 runtime descriptor에서
  현재 발견한 `NodeRid`만 그 시점의 관리 target 또는 표시 값으로 사용할 수 있다. 이를 저장해 다음
  실행의 고정 target으로 사용하거나 Actor·Spot 위치를 추론해서는 안 된다. 업무상 `NodeId`가 필요한
  sample은 transport `NodeRid`와 다른 domain identifier로 정의하고 framework routing에 사용하지 않는다.
- STREAM과 classic Pub/Sub은 Channel egress가 아니므로 독립 listener를 유지한다.

언어별 sample topology regression은 다음 공통 fixture를 읽어야 한다. 같은 기대값을 언어별 source에
복사해 두지 않는다.

```text
framework/doc/framework/common/sample/fixtures/
`-- channel-topology.json
```

Fixture의 `channelKinds`는 각 `channels` entry가 물리 RouteMesh connection을 사용하는지,
별도 ClientServer socket을 사용하는지 구분한다. Object Client pair의 연결 필요성은
`RouteMesh` Server membership만 사용해 판단한다. `ClientServer` Server는 이 판정에
포함하지 않는다.

### Sample별 물리 연결

| sample | RouteMesh 범위 | ClientServer 범위 | 별도 연결 |
|---|---|---|---|
| Bingo | Api·Matchmaking이 `bingo.matchmaking`, Session·Api·Play가 `bingo.play`를 사용한다. 두 Mesh는 object provider와 배치 pool을 공유하지 않는다. RouteMesh Channel Server가 없는 Object Client끼리는 연결하지 않는다. | `bingo.api`: Session·Play Client → Api Server | Session STREAM, Redis matchmaking state |
| TicTacToe | Api Object Client와 두 Play Object Server가 `tictactoe`를 수동 연결한다. 두 Play는 milestone multicast Server membership도 제공한다. | `tictactoe.api`: Play Client → Api Server | Play STREAM, Redis location store |
| SupportChat | Session·Api는 Object Client, Support는 Object Server로 `supportchat`을 공유한다. 두 Object Client에는 RouteMesh Channel Server가 없으므로 서로 연결하지 않는다. | `supportchat.api`: Session·Support Client → Api Server | Session STREAM |
| DeliveryDispatch | Dispatch·CourierSession은 Object Client, CourierActorNode는 Object Server로 `deliverydispatch.courier`를 공유한다. Tracking은 Object Client, CustomerGateway는 Object Server로 `deliverydispatch.customer`를 공유한다. | `deliverydispatch.dispatch`: CourierActorNode Client → Dispatch Server; `deliverydispatch.tracking`: Dispatch Client → Tracking Server | Courier STREAM, Customer STREAM |
| ShoppingMall | CommerceApi·OrderWorkflow가 `shoppingmall.workflow` 하나를 공유하고 OrderWorkflow만 Instance factory 제공 | 없음 | Commerce HTTP, shared event·projection store |
| GameQuest | GameApi·QuestMission이 `gamequest` 하나를 공유 | 없음 | GameApi STREAM, shared state store |
| ZoneWorld | Gateway·ZoneNode·Ops가 `zoneworld.mesh` 하나를 공유 | 없음 | Gateway STREAM, Ops STREAM, `zoneworld.broadcast` classic fanout |

### Channel 역할

| sample | Client 역할 | Server 역할 |
|---|---|---|
| Bingo | 독립 ClientServer의 Session·Play: `bingo.api` | 독립 ClientServer의 Api: `bingo.api`; `bingo.play` RouteMesh의 Play: `bingo.room` Logical Multicast membership |
| TicTacToe | 독립 ClientServer의 Play: `tictactoe.api` | 독립 ClientServer의 Api: `tictactoe.api`; RouteMesh의 두 Play: `tictactoe.player.milestone.channel` Logical Multicast membership |
| SupportChat | 독립 ClientServer의 Session·Support: `supportchat.api` | 독립 ClientServer의 Api: `supportchat.api` |
| DeliveryDispatch | 독립 ClientServer의 CourierActorNode: `deliverydispatch.dispatch`; Dispatch: `deliverydispatch.tracking` | 독립 ClientServer의 Dispatch: `deliverydispatch.dispatch`; Tracking: `deliverydispatch.tracking` |
| ShoppingMall | 없음. CommerceApi는 Order ID의 global SpotId로 direct Spot call을 시작하고 `InstanceSpot("shoppingmall.order-workflow")` marker를 명시한다 | 없음. OrderWorkflow는 `OrderWorkflowSpot` Instance factory만 제공 |
| GameQuest | 없음. GameApi는 Player ID의 global SpotId로 direct Spot call을 시작하고 `InstanceSpot("gamequest.player-quest")` marker를 명시한다 | 없음. QuestMission은 `PlayerQuestSpot` Instance factory만 제공 |
| ZoneWorld | Gateway: actors; ZoneNode: report | actor 생성 ZoneNode: actors; Ops: report; zone multicast 대상은 ZoneNode만 등록 |

TicTacToe의 수동 RouteMesh initiator는 API-A→Play-A/Play-B, API-B→Play-A/Play-B,
Play-A→Play-B로 고정한다. Object Client인 API-A와 API-B는 서로 연결하지 않는다.
Play→API request는 별도 ClientServer `tictactoe.api` 연결을 사용한다. RouteMesh 연결을 역방향
업무 channel로 재사용하지 않는다.

## Actor·Spot 생성과 matchmaking

샘플은 생성 요청을 channel handler로 중계하지 않는다. 생성하려는 process가 public manager를
직접 호출하고 Framework가 Location Store의 후보와 weight를 사용해 owner node를 선택하게 한다.

| 목적 | 호출 | 식별자 소유자 |
|---|---|---|
| application이 정한 ActorId로 새 Actor만 생성 | Actor manager의 `Create` | application |
| application이 정한 ActorId로 기존 Actor를 얻거나 한 번만 생성 | Actor manager의 `GetOrCreate` | application |
| 새 User Spot을 만들고 Framework가 ID를 발급 | Spot manager의 `Create` | Framework |
| room code·zone code·match reservation처럼 이미 정한 SpotId를 한 번만 생성 | Spot manager의 `GetOrCreate` | application |
| Instance Spot에 첫 메시지를 보내 필요할 때 생성 | Spot send/request의 `InstanceSpot` marker | caller가 지정한 SpotId |

`Create`와 `GetOrCreate`는 필요할 때 `InMesh`로 최초 배치 Mesh를 지정할 수 있다. 이미 존재하는
Actor·User Spot에 보내는 direct message에는 `InMesh`를 붙이지 않는다. Framework가 global ID로
현재 owner를 찾기 때문이다. Missing Instance Spot을 처음 활성화하는 send/request에는
`InstanceSpot`과 함께 `InMesh`를 사용할 수 있다.

TicTacToe의 `CreateGame`은 새 방을 만드는 요청이다. API는 `SpotManager.Create`가 반환한 SpotId를
RoomId로 사용한다. 특정 Play endpoint나 NodeRid를 owner로 선택하지 않는다.

Bingo는 matchmaking과 실제 게임 실행의 resource·lifecycle·배치 pool이 다르므로 두 RouteMesh를
사용한다. API process는 두 Mesh에 각각 Object Client MeshNode를 등록한다.

| MeshName | Object Server | 제공 기능 |
|---|---|---|
| `bingo.matchmaking` | Matchmaking | 레벨 bucket별 `bingo.matchmaker` Instance Spot |
| `bingo.play` | Play | Bingo room User Spot과 player Actor |

Sample runner는 Matchmaking process 하나를 시작하지만 singleton 계약을 만들지 않는다. 운영자는
Matchmaking node를 여러 개 둘 수 있으며 Framework가 레벨별 Instance Spot을 eligible node에
분산한다. Matchmaking node는 room User Spot이나 player Actor type을 등록하지 않고, Play node는
matchmaker Instance Spot type을 등록하지 않는다.

API는 player level을 bounded bucket으로 바꾸고
`bingo-matchmaker-level-<bucket>` SpotId에 request를 보낸다. Missing Spot이면
`InstanceSpot("bingo.matchmaker")`와 `InMesh("bingo.matchmaking")` intent로 자동 생성한다.
Instance Spot은 같은 bucket 요청을 turn 단위로 처리하고 Redis의 open room reservation을
선택하거나 새로 만든다. 실제 state는 Redis가 소유하며 Instance Spot은 `RecreateOnRelocation` policy를
사용한다. Idle timer가 만료되면 Spot 내부에서 `Context.CloseAsync()`를 호출한다.

Redis match reservation은 stable RoomId와 같은 RoomSettings를 소유한다. 첫 요청과 동시에 들어온
요청은 모두 같은 reservation을 받고, API는 모두 `SpotManager.GetOrCreate`를
`InMesh("bingo.play")`와 함께 호출해 room Spot이 Ready가 될 때까지 기다린다. 첫 요청만 생성
완료를 기다리고 나머지 요청이 RoomId를 바로 반환해서는 안 된다. 생성이 실패하면 성공하지 않은
RoomId를 참가자에게 반환하지 않는다. 참가자 수가 채워질 때까지 기다리는 책임은 matchmaking
request가 아니라 생성된 room User Spot이 맡는다.

## Relocation과 Message Follow 검증

Relocation을 시연하는 샘플은 host 전체 소요 시간이 아니라 application이 사용하는 단위 하나의
서비스 중단 시간을 측정한다. 측정 구간은 source가 새 작업을 막은 시점부터 target이 새 작업을
받기 시작했다는 ACK를 보낸 시점까지다. Actor 하나, Instance Spot 하나와 SpotWide User Spot
aggregate 하나는 각각 기본 1초 이내를 목표로 한다. 1초를 넘겨도 relocation을 취소하거나
rollback하지 않는다. 자세한 기준은
[graceful drain과 handoff](../spec/28-graceful-drain-handoff.ko.md#71-relocation-unit별-서비스-중단-시간-목표)를
따른다.

SpotWide User Spot은 Spot state와 member Actor state를 하나의 relocation unit으로 옮긴다.
여러 Actor payload는 Relocation Store에 순차로 저장하거나 읽지 않고 설정된 I/O concurrency와
in-flight byte 제한 안에서 병렬 처리한다. Queue, accepted journal과 logical timer는 Framework가
저장하고 target에서 순서를 유지해 복원한다.

Entry Spot과 PerActor User Spot은 Spot application state를 옮기지 않는다. Framework는 target에
같은 SpotId의 stateless shell을 준비하고 Actor를 하나씩 옮긴다. Application은 Actor adapter의
state만 관리하며 Spot에 ActorRef 목록을 복원하지 않는다.

SpotWide factory가 `ApplicationSignaled` boundary를 선택한 샘플은 안전한 업무 turn에서
`RelocationReady().Defer()`를 등록한다. Framework는 실제 relocation 여부와 관계없이
`OnRelocationReadyCompletedAsync`를 호출한다. Application은 이 callback이 끝난 뒤 다음 round를
시작한다. 기본 `AnyTurnBoundary`에서 `Defer()`를 호출하는 예제는 만들지 않는다.

[Message Follow](../spec/21-location-runtime.ko.md#63-이전-owner로-도착한-message를-새-owner에게-전달한다)는
owner 변경 직후 이전 node에 도착한 Actor·Spot message를 current owner에게 전달하는 기능이다.
샘플 검증은 다음 경우를 각각 확인한다.

- Relocation 도중 시작한 one-way send와 request/reply가 target owner에 도달한다. 업무 효과의
  중복 여부는 보존된 operation ID를 사용하는 handler의 idempotency 검증으로 확인한다.
- Reply route, operation ID, payload와 ObjectGeneration이 relay 뒤에도 유지된다.
- Source는 relocation 완료 때 기록한 target만 사용하며 Message Follow 중 Location Store를 다시
  조회하지 않는다.
- `MessageFollowDuration` 기본값은 30초이고 0이면 relay를 사용하지 않는다. Sample이 더 짧은
  값을 설정하면 실제 설정값과 만료 시각을 evidence에 남긴다.
- Relay는 최대 8번, 이동 하나당 message 1,024개와 16 MiB 이내에서만 동작한다. Route missing·기간
  만료·loop·hop 초과는 `Unavailable`, generation mismatch는 `InvalidOperation`, message·byte 한도 초과는
  `CapacityExceeded`인지 각각 확인한다.
- Relay 실패나 target admission 뒤 실행 여부가 불명확한 failure를 fresh owner에게 자동
  재제출하지 않는다. 실패한 operation은 terminal로 끝나며 다음 call만 fresh resolve한다.
- Message Follow 기간이 끝난 뒤 시작한 새 call은 Location Store에서 current owner를 찾으며
  이전 source relay에 의존하지 않는다.

## 메시지 이름 원칙

샘플 메시지 이름은 도메인 사건 이름보다 framework 호출 방식이 먼저 드러나야 한다. 같은 업무
흐름이라도 request/reply인지, 단방향 send인지, client push인지에 따라 호출자가 기다리는 값과
handler 계약이 달라지기 때문이다. 언어별 샘플과 e2e는 아래 접미어를 같은 뜻으로 사용한다.

| 호출 방식 | 접미어 | 기준 |
|-----------|--------|------|
| request/reply | `Req` / `Res` | `Request(...)`, `RequestToChannel(...)`, route request, stream request, HTTP request처럼 응답을 기다리는 호출 |
| send | `Msg` | `Send(...)`처럼 응답 없이 전달하는 단방향 메시지 |
| client push | `Notify` | server가 stream/session으로 client에 밀어 주고 client가 기다려 받는 알림 |
| publish (pub/sub · fanout) | `Event` | Spot context의 `Publish(...)`나 classic fanout publish처럼 **발행자가 수신자를 모르는** 메시지. Logical Multicast와 channel fanout이 여기에 해당한다 |

request로 호출하는 메시지는 업무 이름이 `Changed`, `Accepted`, `Created`처럼 보여도 `Req`와
`Res` 쌍으로 명명한다. 예를 들어 상태 변경을 요청하고 ack를 기다리는 흐름은
`DeliveryStatusChangedReq`와 `DeliveryStatusChangedRes`가 맞다. 반대로 server가 고객 client에
상태 변경을 밀어 주는 흐름은 `DeliveryStatusNotify`처럼 `Notify`를 사용한다.

`Command`, `Result`, `Ack` 같은 접미어는 샘플의 wire message 이름으로 새로 늘리지
않는다. 이런 이름은 내부 도메인 event, 업무 명령, 처리 결과, transport 응답을 서로 섞어 보이게
할 수 있다. 이미 존재하는 샘플 메시지를 손볼 때도 호출 방식 기준으로 위 표의 접미어 중 하나로
정리한다. `Event`는 **publish 호출에만** 쓴다 — request나 send로 보내는 메시지에 `Event`를
붙이지 않는다.

이 규칙은 stream, channel, actor, Spot 경계를 실제로 넘나드는 ZLink wire message에
적용한다. 아래 두 경우는 wire message가 아니므로 예외로 둔다.

- **도메인 event stream(SoR) 레코드**: event sourcing 샘플(ShoppingMall, GameQuest)의
  `OrderStartedEvent`, `QuestProgressed`처럼 event store에 append되는 도메인 이벤트는 이 규칙의
  대상이 아니다. 이 이름은 그 자체로 전송되는 packet이 아니라 durable store 안에 쌓이는
  기록이며, event sourcing 어휘가 곧 도메인 표현이라 `Event` 접미어를 강제하지도, 금지하지도
  않는다 — 도메인이 자연스러운 이름(`OrderStartedEvent`, `QuestProgressed`)을 정한다.
- **in-process 도메인/application port 계약**: 같은 서버 프로세스 안에서 도메인 module을
  호출하는 port DTO(예: `ReserveInventoryCommand`)는 ZLink로 dispatch되지 않는 언어 중립
  계약이므로 `Command`/`Result` 접미어를 유지할 수 있다.

반대로 entry-spot에서 owner spot으로 실제 `SendToSpot`/`RequestToSpot`으로 전달되는
내부 메시지는 예외가 아니다. 이런 메시지는 호출 방식에 맞춰 `Msg`(one-way send) 또는
`Req`/`Res`(request/reply)로 명명한다.

## Spot 실행 turn과 terminator 샘플 기준

**모든 샘플은 세 terminator를 같은 기준으로 고른다**([04 §1.1](../spec/05-async-execution-policy.ko.md)).

| terminator | 실행 줄 | 언제 |
|---|---|---|
| `submit` | 그대로 진행(one-way) | 응답을 쓰지 않는다 |
| **`async`**(기본) | **turn을 유지한다** | 대기 결과로 **이 spot의 상태를 판단·변경**한다. handler = 하나의 turn |
| **`yield`**(opt-in) | **turn을 반납한다** | 대기가 **이 spot의 공유 상태와 무관**하다. 그 대기로 spot 전체가 멈추면 안 된다 |

기준은 하나다 — **그 대기가 이 spot의 공유 상태와 관련이 있는가.** `yield`는 편의가 아니라
**직렬 실행의 이점을 지키면서 무관한 대기만 빼내는** 도구다. 그래서 기본은 `async`이고 `yield`는
근거가 있을 때만 쓴다.

`yield`를 쓰는 자리는 **`yield` 앞뒤로 같은 mutable state를 이어서 판단하지 않는다.** 양보 중에
다른 메시지가 먼저 처리될 수 있으므로, 재개 후에는 필요한 상태를 다시 확인한다.

샘플의 기준 사용처는 아래와 같다.

| 샘플 | 지점 | terminator |
|------|------|-----------|
| [Bingo](bingo/README.ko.md) §7.1 | room Spot의 actor join/leave가 Api 서버에서 player 전적을 조회·기록한다 | **`yield`** |
| [Bingo](bingo/README.ko.md) §3.1 | Matchmaker Instance Spot이 Redis reservation 결과로 같은 bucket의 다음 상태를 결정한다 | `async` |
| [DeliveryDispatch](deliverydispatch/README.ko.md) §6.1 | Entry Spot이 전달받은 새 Actor의 application 상태를 초기화한다 | `async` |
| TicTacToe | game join이 게임 상태 흐름으로 바로 이어진다 | `async` |

**worker와 HTTP client도 같은 축이다**([04 §1.2](../spec/05-async-execution-policy.ko.md),
[12 §3](../spec/http-client/12-http-client.ko.md)). 외부 HTTP·레거시 API는 HTTP client의 terminator를 직접 쓰고,
DB 드라이버·외부 SDK처럼 자체 terminator가 없는 비동기 대기는 `RunIoWorker(...)`로 감싼다. CPU
작업은 `RunCpuWorker(...)`로 넘긴다.

HTTP 응답에서 decoded body만 사용하는 sample은 언어별 `Fetch`/`fetch` terminal로
DTO를 직접 받는다. Status나 header를 검증해야 할 때만 typed response envelope를
반환하는 `Async`/`submit`/`await` terminal을 사용한다. DTO를 받기 위해 response의
`.Body`/`.body()`를 즉시 꺼내는 코드는 sample에 두지 않는다.

## 샘플 포팅 기준

Bingo와 TicTacToe는 각자 맡은 기능을 보여 주는 예외 샘플이다. Bingo는 Protobuf
payload, 두 RouteMesh로 분리한 matchmaking·gameplay 배치 pool과 location store 기반
gateway를 보여 주고, TicTacToe는 수동 endpoint와 공식 Location Store를 함께 쓰는
scale-out 흐름을 보여 준다.

그 밖의 정본 샘플(SupportChat, DeliveryDispatch, ShoppingMall, GameQuest)은
아래 기준을 따른다.

- payload codec은 JSON을 기본으로 사용한다. 샘플끼리 payload를 비교하기 쉽고, event
  sourcing과 projection state를 사람이 읽기 쉬워야 하기 때문이다.
- Protobuf나 MessagePack이 필요한 샘플은 framework codec extension package를 설치하고
  구성 단계에서 extension을 등록한다. 샘플의 DTO, handler, client 호출 모양은 codec 때문에
  바꾸지 않는다.
- 서버 간 연결은 공유 location store 기반 자동 연결로 구성한다. 샘플 코드가 endpoint
  연결 순서나 route warmup을 직접 관리하지 않게 하기 위해서다.
- 한 process는 특별한 물리 격리 요구가 없으면 RouteMesh를 하나만 등록하고, 업무별 route는 그
  MeshNode의 여러 ChannelName membership으로 나눈다. Bingo API는 예외다. Matchmaking Instance
  Spot과 gameplay User Spot·Actor를 서로 다른 provider·resource pool에 배치해야 하므로
  `bingo.matchmaking`과 `bingo.play` Object Client를 함께 등록한다. Classic pub/sub과 STREAM
  node는 RouteMesh의 ChannelName이 아니므로 각각 독립 등록을 유지한다.
  샘플에서 RouteMesh를 추가하려면 먼저 해당 샘플 문서에 물리 mesh를 분리해야 하는 이유와 사용자가
  체감하는 연결 경계를 기록한다.
- **절대 규칙: TicTacToe만 수동 연결을 사용할 수 있다.** TicTacToe를 제외한 모든 샘플은
  어떤 이유로도 수동 연결을 추가하거나 유지하면 안 된다. 빌드·실행 성공, 일시적인 자동 연결
  실패, 디버깅 편의, 언어별 구현 차이는 예외 사유가 아니다. 그 밖의 샘플은 ChannelName client에 상대
  endpoint를 직접 넘기거나, Spot router/pub-sub peer를 직접 연결하거나, 서버 간 호출을
  고정 HTTP endpoint로 우회하면 안 된다. 즉 application code가 manual peer 연결, Spot 전용
  router/pub-sub peer 연결 또는 서버 코드의 peer 대상 `ZLinkHttpClient.Create(...)`를 사용하지 않는다.
  자동 연결이 실패하면 샘플에 수동 연결을 추가하지 말고 location store 등록·조회·연결
  lifecycle이 끊긴 framework 구현을 수정한다. 이 금지는 언어별 sample 전체에 적용하며,
  위반이 하나라도 있으면 해당 샘플 변경은 완료된 것으로 판단하지 않는다.
- **자동 등록이 기본이다.** framework가 handler를 스캔하고 등록할 수 있는 언어에서는 별도 등록
  호출 없이 handler를 자동 등록한다([05 §8](../spec/06-framework-api.ko.md#8-handler-등록과-dispatch)). 샘플마다 handler
  목록을 반복해서 적으면 public 사용 예시가 장황해지고, handler 추가 누락을 client 시나리오가
  늦게 발견하게 된다.
- **수동 topology와 handler 등록은 별개다.** TicTacToe도 handler를
  annotation·attribute·decorator로 선언하고 assembly·module scan으로 자동 등록한다. 수동 endpoint
  연결을 보여 주기 위해 handler 목록까지 구성 코드에 반복해서 적지 않는다.
- C++은 runtime reflection scanner를 사용하지 않으므로 compile-time 타입으로 handler를 명시
  등록한다. 정확한 표면은 [C++ handler 공개 계약](../spec/server/languages/cpp/interfaces/03-channel-messaging.ko.md)을
  따른다. 등록 방법만 다르며 메시지·역할·codec·검증 기준은 바꾸지 않는다.

## Dispatch 오류 로그 기준

모든 언어별 샘플은 framework message dispatch 오류를 샘플 로그에 남겨야 한다.
등록되지 않은 request, payload decode 실패, handler 예외처럼 dispatch 단계에서
처리할 수 없는 메시지는 샘플 실행 중 바로 확인할 수 있어야 하기 때문이다.

샘플마다 `AddZLinkFramework(...)`에서 Framework diagnostics를 설정하고 자기 logger로 기록한다. Bingo,
TicTacToe, SupportChat 같은 서로 다른 샘플이 logging 설정 helper를 공유하지 않는다. 샘플은 독립적으로
읽고 옮길 수 있어야 하며, logging 설정이 다른 샘플의 디렉토리에 의존하면 그 기준이 깨진다.

로그 출력은 새 logging 체계를 만들지 않고 각 샘플이 이미 쓰는 logger를 따른다.
파일 로그를 이미 직접 쓰는 샘플은 그 파일 logger에 기록하고, 실행 스크립트가
stdout/stderr를 `logs/*.log`로 저장하는 샘플은 그 샘플의 console logger에 기록하면 된다.
Trace attribute는 [Message flow tracing](../spec/26-message-flow-tracing.ko.md)의 정확한 snake_case 이름과
포함 조건을 따른다. `surface`, `message_kind`, `outcome`은 message-flow 기록에 포함하고, 원인이 있을 때
`reason`, dispatch error에는 `action`을 포함한다. Typed packet name이 있을 때만 `packet_name`,
request와 terminal reply를 연결할 때만 `correlation_id`를 기록한다. Channel 경로는 `channel_name`, Spot
경로는 `spot_id`, Actor 경로는 `actor_id`처럼 실제 논리 target이 있는 식별자만 남긴다.

Structured log로 제공하는 구현은 같은 spec의 fallback key인 `event`, `kind`, `channel`, `packet`, `spot`,
`actor`, `corr` 등을 그대로 사용한다. 두 schema를 camelCase 이름으로 바꾸거나 값이 없는 field를 빈
문자열로 강제하지 않는다.

샘플은 각 서버 프로세스의 `AddZLinkFramework(...)` 설정에서 diagnostics level과 sampling을
설정한다. Framework가 표준 tracing과 structured logging provider에 기록을 전달하며 application이
별도 observer·event DTO나 Framework 전용 log file을 구현하지 않는다. `run_sample.sh`와
`run_sample.ps1`은 프로세스 출력을 `logs/*.log`에 저장하므로 message-flow error도 같은
샘플 로그에서 확인한다.

샘플 handler는 framework가 처리하는 dispatch 오류를 handler 안에서 다시 잘게
처리하지 않는다. request, actor request, session packet handler 안에서 예외를 잡아
로그만 남긴 뒤 다시 던지거나, domain 예외를 임의의 `error` 필드 응답으로 바꾸지
않는다. 그런 예외는 Framework dispatch 경계가 error reply, drop과 표준 diagnostics 기록으로
처리하게 둔다. 샘플 handler는 성공 경로와 도메인 동작을
보여 주는 데 집중해야 하며, 실패를 정상 업무 응답으로 바꾸는 코드는 해당 메시지
계약이 명시적으로 그런 실패 상태를 정의할 때만 둔다.

## 샘플 설정 전달 기준

모든 언어의 sample은
[Sample/E2E 설정 정책](../sample-e2e-configuration-policy.ko.md)을 필수로 따른다. Runner는 실행별
role 설정 파일을 만들고 framework host에는 설정 파일 경로만 전달한다. Framework host가 아닌
standalone client는 직접 연결하는 endpoint, 요청 timeout과 scenario selector를 명시적인 CLI
option으로 받되 시작할 때 한 번 검증한다.
Endpoint, Redis, routing id, timeout과 로그 경로를 환경 변수나 JVM system property로 전달하지
않으며, server와 client 애플리케이션 코드에서 직접 사용할 수 있는 환경 변수는 0개다.

환경 변수 interface를 호환 경로로 함께 제공하지 않는다. Framework host는 설정 파일과 typed
binding을 사용하고, standalone client는 검증된 CLI 입력 또는 필요한 경우 typed 설정 파일을
사용한다.

## 샘플 실행 스크립트와 Redis 격리 기준

모든 언어의 sample runner는 같은 사용 의미와 같은 Redis 구동 방식을 가져야 한다.
언어별 구현은 shell, PowerShell, npm, Gradle, dotnet, CMake처럼 도구가 달라도 아래 실행
계약을 맞춘다.

**필수 격리 규칙:** Redis가 필요한 각 sample 실행은 그 실행만 사용하는 전용 Docker Redis
container를 새로 만들어야 한다. 이미 실행 중인 container, host Redis, 다른 sample이나 E2E가 만든
Redis endpoint를 공유하거나 fallback으로 사용하면 안 된다. key prefix만 다르게 지정하는 것도
인스턴스 공유를 허용하지 않는다. cleanup 시점과 저장 데이터가 다른 실행에 영향을 주지 않게 하는
것이 이 규칙의 목적이다.

기준 템플릿은 이 디렉토리의 `runner-templates/` 아래에 둔다.

- `runner-templates/redis-common.template.sh`: Redis helper 기준
- `runner-templates/run_sample.template.sh`: 개별 sample runner 기준
- `runner-templates/run_samples.template.sh`: 통합 sample runner 기준

- 개별 `run_sample.*`는 build → 로그 디렉토리 생성 → 필요한 Redis 준비 → 서버 시작
  → readiness 확인 → client self-check 실행 → 서버와 Redis 정리 순서를 책임진다.
- 각 언어는 sample runner들이 공유하는 Redis helper를 둔다. helper는 실행별 Redis container
  시작과 그 실행이 만든 container id 정리를 공통 함수로 제공하고, 개별 sample script가 Docker
  명령을 직접 조합하지 않게 한다.
- Redis가 필요한 sample은 runner가 실행마다 전용 Docker Redis container를 직접 시작한다.
  이미 떠 있는 Redis container나 host Redis endpoint를 재사용하면 안 된다. Redis key prefix가
  달라도 cleanup, 장애 주입, latency injection, sample 간 데이터 정리 시점이 섞이면 테스트
  간섭이 발생할 수 있기 때문이다. 샘플 애플리케이션 코드가 Docker를 호출하거나 Redis container
  생명주기를 소유하면 안 된다.
- Docker Redis를 만들지 못하면 runner는 즉시 실패한다. host Redis나 다른 실행의 endpoint로
  자동 전환해서 성공 처리하면 안 된다.
- Redis container 시작은 모든 언어에서 같은 순서를 쓴다.
  `docker create --name <scoped-name> --tmpfs /data -p 127.0.0.1::6379 <pinned-redis-image>`로 container를
  만들고, `docker start <container-id>`로 시작한 뒤, `docker inspect`로 실행 상태와 배정된
  host port를 읽는다. `docker run -d` 출력에 의존해 container id와 port를 동시에 처리하는
  방식은 쓰지 않는다.
- sample Redis 데이터는 실행 중에만 필요하므로 Docker volume을 만들지 않는다. Redis 이미지가
  선언한 `/data` volume은 `--tmpfs /data`로 덮어쓰고, container 정리에는 `docker rm -fv`를
  사용한다. 이렇게 해야 반복 실행 후 anonymous volume이 남지 않는다.
- 개별 `run_sample.*`가 Redis를 시작할 때는 container 이름에 언어와 sample 실행 범위를
  드러내는 prefix를 추가한다. 예를 들어 Java sample은 `zlink-redis-java-sample...`,
  Kotlin sample은 `zlink-redis-kotlin-sample...`처럼 같은 언어·sample 범위를 한눈에
  알 수 있어야 한다.
- 개별 `run_sample.*`와 통합 sample runner는 시작 시 같은 prefix의 다른 container를 지우지
  않는다. 같은 언어 runner도 동시에 실행될 수 있으므로 prefix cleanup은 다른 실행의 전용 Redis를
  제거할 수 있다.
- 개별 `run_sample.*`는 정상 종료와 실패 종료 모두에서 자신이 만든 Redis container id만
  정리한다. prefix로 넓게 지우는 cleanup을 개별 script의 exit trap에 넣지 않는다.
- 통합 sample runner는 다른 실행의 Redis를 정리하지 않고 각 개별 `run_sample.*`를 순차 호출한다.
  이 runner도 한 실행 안에서는 sample을 병렬 실행하지 않는다.
- 통합 sample runner는 특정 sample 리스트만 실행할 수 있어야 한다. 인자가 없으면 모든 sample을
  실행하고, 인자가 있으면 지정한 sample runner만 순차 실행한다. 예:
  `./run_samples.sh Bingo SupportChat` 또는 언어별 경로를 구분해야 하는 runner에서는
  `./run_samples.sh java/Bingo kotlin/SupportChat`처럼 쓴다. 통합 runner는 sample 내부 절차를
  재구현하지 않고 선택한 개별 `run_sample.*`만 호출한다.
- 통합 sample runner는 sample별 내부 동작을 다시 구현하지 않는다. 선택한 개별 `run_sample.*`를
  한 번 호출하고 최종 결과만 관리한다. Redis endpoint 생성,
  readiness, 로그 위치, self-check 세부 절차는 개별 script와 공통 helper가 맡는다.
- Redis host port는 고정값을 쓰지 않고 Docker가 비어 있는 loopback port를 배정하게 한다.
  runner는 배정된 port를 inspect로 읽어 애플리케이션 설정에 전달한다. Redis key prefix도
  실행마다 고유하게 만든다.
- 같은 host에서 다른 sample/e2e가 Redis를 사용 중이어도 그 endpoint를 빌려 쓰지 않는다. 새
  Docker Redis container를 만들고 Docker가 할당한 다른 loopback port를 사용해야 테스트 간섭을
  막을 수 있다.
- Redis container 생성은 오래 걸릴 수 있으므로 Docker 명령 자체에는 짧은 timeout을 두고,
  실제 Redis readiness는 별도의 port/readiness 대기 함수로 확인한다. Docker 명령이
  응답하지 않는 문제와 Redis가 아직 준비되지 않은 문제를 같은 sleep으로 숨기지 않는다.
- Redis helper가 실패하면 개별 runner도 즉시 실패해야 한다. shell runner에서는
  `read ... < <(redis_start_function)`처럼 process substitution 결과를 읽는 방식으로 container id를
  받지 않는다. 이 방식은 helper가 실패해도 `read` 자체는 성공할 수 있어 Redis 없이 서버를 시작하는
  잘못된 실행으로 이어진다. helper는 `zlink_redis_start_scoped_assign`처럼 호출부 변수에 값을
  대입하는 함수로 제공하고, 함수 실패가 그대로 runner 실패가 되게 한다.
- 통합 sample runner는 bind 실패를 포함한 개별 sample 실패를 재시도하지 않는다. 실행별 port를
  미리 확보했는데도 bind가 실패했다면 같은 실행을 반복해 숨기지 않고 즉시 실패해야 원인을 확인할 수 있다.
- 실패 시 runner는 `log_dir=...` 또는 sample별 로그 위치를 출력하고, 각 프로세스의
  stdout/stderr와 framework log를 `logs/*.log`에 남긴다.

## 공통 작성 원칙

- 샘플은 framework가 어떤 일을 대신해 주는지 보여 주어야 한다.
- 도메인 규칙은 작게 유지하고, session, actor, Spot, channel, timer, push 흐름이
  코드에서 잘 보이게 둔다.
- 샘플 애플리케이션 코드는 각 언어 framework가 공개한 package entrypoint, DI token,
  builder, client interface만 사용한다. `internal`, `runtime`, `dist/runtime`처럼
  유지보수용 구현 위치를 직접 import하거나 reflection으로 접근하지 않는다. 필요한 기능이
  공개 계약에 없으면 샘플에서 우회하지 않고 framework의 public contract를 먼저 보완한다.
- 이 문서에서 `Spot`은 독립적인 생명주기를 가지는 stateful coordination point를 뜻한다.
  Spot은 room, conversation, workflow instance, player quest처럼 상태와 이벤트가 모이는
  단위를 표현한다. Spot은 actor 참여를 받을 수 있지만 actor가 필수는 아니다. Spot은
  directed request를 처리하거나, event를 publish하거나, timer를 실행하거나, pub/sub event에
  반응할 수 있다.
- 실시간 상태를 소유하는 서버는 `Domain`, `Application`, `Infrastructure` 책임을 나누어 구현한다.
  아래 이름은 권장 구조이며, 디렉터리 이름은 언어 관용에 맞게 바꿀 수 있다.
  다만 같은 책임 분리와 의존 방향은 유지해야 한다.
  - `Domain`은 순수 도메인 규칙, 상태 전이, 결과 판정, 도메인 event 생성을 맡는다.
    framework 타입, socket, stream, handler, logger, DI container에 의존하지 않는다.
  - `Application`은 room 생성, room 배정처럼 domain을 사용하는 use case를 맡는다.
    framework adapter가 호출할 수 있는 작고 명확한 진입점을 제공한다.
  - `Infrastructure`는 framework와 외부 연결을 맡는다. ZLink actor, session, Spot,
    handler, notification publisher, channel request handler는 이 레이어에 둔다.
- 언어별 샘플은 같은 역할과 메시지 이름을 사용한다. 언어 관용구 때문에 이름을
  바꿔야 하면 공통 문서에 차이를 먼저 기록한다.
- 클라이언트에서 실제 서버에 접속해 request, push, final state를 확인하는 흐름은
  `ClientScenario` 이름으로 둔다. 예를 들어 `BingoClientScenario`처럼 샘플 이름과
  client scenario 역할이 함께 드러나야 한다. `TestScenario`는 별도 테스트 fixture로
  오해될 수 있으므로 샘플 client 실행 흐름의 이름으로 쓰지 않는다.
- 공통 문서의 메시지 계약은 언어 중립 schema로 읽는다. 언어별 샘플은 record,
  class, struct, interface, type alias처럼 자기 언어에 맞는 표현으로 같은 필드와
  의미를 구현한다.
- **enum·상태 값은 wire에서 이름 있는 문자열로 직렬화한다. 정수 ordinal로 보내지 않는다.**
  이름 있는 값(`Assigned`, `Delivered`, `Won`, `Draw` 등)을 정의한 필드는 그 이름 그대로
  wire를 탄다. 언어의 enum 타입이 기본적으로 정수로 직렬화되면(예: C# enum, 문자열
  컨버터 미등록) **그 언어에서 명시적으로 문자열 인코딩을 등록해야 한다.** 정수 ordinal은
  값 하나가 중간에 추가되면 순서가 밀려 **다른 언어 소비자를 조용히 깨뜨리고**, 로그에서
  읽을 수도 없다. 이 규칙은 모든 샘플·모든 언어에 적용한다.
- **nullable 필드는 값이 없으면 wire에서 키를 생략하거나 `null`을 싣되, 그 처리를 언어 간에
  맞춘다.** 한 언어가 `null`을 싣는데 다른 언어의 디코더가 "키 없음"만 처리하면 교차 언어에서
  터진다. 문서가 `Field?`로 표시한 것만 nullable이고, 표시 없는 필드는 항상 실린다.
- channel, route, stream, actor, Spot 경계를 넘는 wire message는 이름 있는 계약으로
  둔다. Python `dict` 나 Node.js object literal 처럼 동적 객체를 쉽게 만들 수 있는
  언어에서도 호출 지점에 `{ ... }` 를 바로 쓰거나 packet name 문자열을 흩어 놓지
  않는다. 요청, 응답, 알림 payload는 `Shared/Contracts` 같은 공용 계약 위치에
  message type 또는 schema로 두고, client와 server는 그 객체의 public interface만
  사용해야 한다.
- codec별 편의 wrapper나 샘플 전용 helper로 message 객체의 계약을 감추면 안 된다.
  JSON, MessagePack, Protobuf 중 어떤 codec을 쓰더라도 샘플 코드는 connector와
  message 객체가 제공하는 public interface를 직접 사용해야 한다. connector 전용 codec
  package나 bindings codec package를 샘플의 표준 사용법으로 안내하지 않는다.
- inline object literal은 한 함수 안에서만 쓰는 local state, 테스트 보조 값, 파싱 결과처럼
  wire 계약이 아닌 값에만 사용한다. 샘플은 짧은 데모보다 여러 언어에서 같은 메시지
  흐름을 비교할 수 있는 가시성을 우선한다.
- Bingo와 TicTacToe는 같은 기능을 반복해서 보여 주지 않는다. Bingo는 두 RouteMesh로 분리한
  matchmaking·gameplay 배치 pool과 공유 Location Store를 이용한 gateway 구조를, TicTacToe는
  수동 endpoint와 공식 Location Store를 함께 쓰는 scale-out 구조를 맡는다.
- codec 선택은 샘플의 역할을 방해하지 않도록 단순하게 둔다. Bingo는 여러 언어가 공유하는
  schema가 분명한 Protobuf payload를 맡고, TicTacToe와 나머지 샘플은 읽고 비교하기 쉬운
  JSON payload를 기본으로 둔다. Bingo의 Protobuf 사용도 업무 API 차이가 아니라 dependency와
  framework codec extension 등록 차이로만 드러나야 한다.
- 도메인 식별자는 그 의미가 드러나게 명명한다. 예를 들어 TicTacToe에서 client가 받는
  값은 Node RID가 아니라 Framework가 발급한 문자열 `RoomId`다. RoomId·ConversationId처럼
  도메인 식별자가 곧 Spot 주소이면 그 UTF-8 문자열을 SpotId로 그대로 사용한다. SpotId를
  `RoutingId`로 변환하거나 Node RID의 hex 표현을 도메인 식별자로 사용하지 않는다.
- MeshNode의 transport RID는 도메인 식별자와 구분되는 infrastructure identity다. Bingo와
  ZoneWorld처럼 routing id allocation을 검증하는 샘플은 location store가 RID를 자동 할당하며,
  애플리케이션이 고정 RID를 설정하지 않는다. 자동 할당된 RID는 public allocation 결과나 runtime
  관측 결과로 확인한다. Node direct target으로 사용하는 범위는 Admin·Ops 관리 operation으로
  제한한다. `ActorRef`에 들어 있는 NodeRid는 현재 route snapshot일 뿐 application identity나
  다음 호출의 target이 아니다. 이 값은 `RoomId`, `ConversationId`, `OrderId` 같은 도메인
  식별자를 대신하지 않으며 client에 도메인 식별자로 노출하지 않는다.

## Client self-check 기준

Bingo와 TicTacToe client는 아래의 공통 검증 흐름을 따른다. 샘플 client는 성공 로그를 출력하는
데서 끝나면 안 된다. request로 보낸 값이 response와 push payload에
같은 의미로 돌아오는지 직접 확인해야 한다.

언어별 client는 아래 항목을 반드시 검증한다.

- 인증 요청에 사용한 token 또는 actor id가 인증 응답의 actor id와 일치한다.
- Room 생성이나 matching 요청이 반환한 RoomId와 state status가 요청 시나리오와 일치한다.
  TicTacToe가 반환하는 endpoint 목록은 client STREAM 접속 후보이며 Room owner 정보가 아님을
  함께 확인한다. `GameName`처럼 특정 샘플에만 있는 필드는 해당 샘플에서만 확인한다.
- 첫 번째 참가자는 waiting 상태를 받고, 두 번째 참가자는 running 또는 in-progress 상태를
  만든다.
- 자기 자신에게 보내면 안 되는 join notify는 받지 않았음을 확인한다.
- 상대 참가자의 join notify는 actor id, room id, state status를 확인한다. TicTacToe의
  `Mark`처럼 특정 샘플에만 있는 필드는 해당 샘플에서 추가로 확인한다.
- game start, move, draw, ended notify는 단순 수신 개수가 아니라 payload 안의 board,
  turn, draw sequence, winner, player list 같은 의미 값을 확인한다.
- deterministic sample은 마지막 winner와 최종 state를 고정값으로 확인한다.

push message 대기는 sample-local polling 함수가 아니라 stream connector 객체가 제공하는 public
wait API를 직접 호출한다. codec별 JSON, MessagePack,
Protobuf wrapper나 샘플 전용 함수 뒤에 대기 흐름을 숨기면 안 된다. sample은 connector가
반환한 message 객체의 public interface로 payload를 읽고 바로 검증한다. notification
수집용 inbox나 로그 queue는 결과 출력과 추가 검증을 위해 둘 수 있지만, push 도착을
기다리는 기준 경로가 되어서는 안 된다.

## 상태 소유 서버 공통 디렉토리 구조

언어별 문법과 build system은 달라도 상태를 소유하는 서버 소스는 아래 구조를 기준으로 맞춘다.

```text
Server/<StateOwner>/
  Domain/
    <DomainName>/
      ... pure domain rules ...
  Application/
    <UseCase>/
      ... use case services ...
  Infrastructure/
    ZLink/
      Actors/
      Handlers/
      Sessions/
      Spots/
        EntrySpot/
          Handlers/
        <DomainSpot>/
          Handlers/
          Notifications/
```

필요 없는 디렉토리는 생략할 수 있다. Entry Spot이 없으면 `EntrySpot/`를 두지 않아도 되고,
별도 notification mapper가 필요 없으면 `<DomainSpot>/Notifications/`를 두지 않아도 된다.
반대로 Bingo처럼 bound session push와 domain event 변환이 필요하면 해당 domain Spot 아래에
`Notifications/`를 둔다.

중요한 기준은 이름 자체가 아니라 의존 방향이다. `Domain`은 `Application`이나
`Infrastructure`를 알면 안 된다. `Application`은 domain을 사용하지만 framework transport
세부 구현에 기대지 않는다. `Infrastructure`는 framework 객체와 message codec, logging,
DI 등록을 다루며 domain state를 직접 조작하지 않고 domain 객체의 method를 호출한다.
