# ZoneWorld Sample Scenario

[샘플 목록](../README.ko.md)

> ZoneWorld는 zone 경계를 이동하는 player Actor와 운영 console을 한 sample에 묶어,
> Framework가 global Spot routing, cross-node relocation, bound session, Logical Multicast와
> classic fanout을 제공하고 Application이 월드 규칙과 desired state에 집중할 수 있음을 보여 준다.

## 1. 목적과 범위

이 sample은 네 개의 논리 zone으로 나뉜 월드에서 player가 경계를 이동하고, 인접 zone에
경계 snapshot을 전달하며, Ops console이 node 상태와 점검 모드를 관리하는 흐름을 다룬다.
Gateway는 게임 browser STREAM을 종단하고 player Actor를 현재 session에 bind한다. ZoneNode는
zone Spot과 Player Actor를 제공한다. Ops는 관제 STREAM, runtime event 관찰과 전 node fanout을
제공한다.

Framework가 맡는 책임은 global ZoneId Spot routing, Actor membership과 cross-node relocation,
Message Follow, bound session route, Logical Multicast subscription, classic fanout transport와
runtime event 전달이다. Application은 좌표·zone 규칙, border snapshot 만료, bot 경로,
NodeId desired state와 UI 정책을 소유한다.

.NET과 Node.js server는 같은 wire contract를 사용하고 TypeScript browser client를 공유한다.
headless runner는 server별 self-check를 실행하며, 브라우저 client는 실제 WS/WSS transport와
화면 흐름을 확인한다.

시작할 때 다음 조건을 가정한다.

- 좌표 범위와 네 zone의 ID가 고정되어 있다.
- Gateway, ZoneNode 두 개와 Ops의 readiness가 완료되어 있다.
- Location Store, Relocation Store와 maintenance store가 실행별로 준비되어 있다.
- X 순찰 bot 네 마리와 Y 순찰 bot 네 마리가 deterministic seed로 생성된다.

범위는 입장, 이동, 경계 동기화, relocation, bot, node 관찰, 전 node 공지와 점검 변경까지다.
다음은 제외한다.

- 전투, 아이템, 경제와 여러 zone을 가로지르는 게임 집계
- client가 local state를 권위로 변경하는 방식
- Transport RID를 player, zone 또는 node 주소로 사용하는 방식
- Ready owner 장애 뒤 자동 crash failover
- browser UI의 제품 수준 인증과 접근 제어

## 2. 요구사항

### 2.1 월드 기능 요구사항

| 항목 | 기준 |
|---|---|
| 좌표 | 0 <= X < 100, 0 <= Y < 100 정수 |
| zone | zone-nw, zone-ne, zone-sw, zone-se 네 사분면 |
| 인접 | 변을 공유하는 zone만 인접이며 대각선은 제외 |
| 경계 band | 경계에서 10 이내인 player를 인접 zone snapshot에 포함 |
| tick | zone Spot 생성 시 0, 이후 100ms마다 1 증가 |
| 이동 | 한 MoveMsg의 각 축 이동 최대 5 |
| 시작 | JoinWorldRes는 (25, 25), zone-nw를 반환 |
| player state | Actor가 X, Y, ZoneId의 권위 상태를 소유하고 Zone Spot은 사본을 보관 |
| bot | zone당 사람 bot 두 마리, 총 8마리, bound session 없음 |

### 2.2 운영·품질 요구사항

- Move rejection 순서는 OutOfRange → TooFar → DiagonalCrossing → ZoneMaintenance로 고정한다.
- ZoneStateNotify.Players는 자기 zone 값을 우선하고 PlayerId UTF-8 byte 오름차순으로 정렬한다.
- 인접 zone snapshot은 FromZoneId별 최신 Tick으로 교체하며 3 tick 동안 새 snapshot이 없으면
  제거한다.
- cross-node zone 이동은 같은 PlayerId와 ObjectGeneration을 유지하고 owner generation만
  변경한다. client WebSocket connection은 session route 갱신이 seal timeout 안에 적용되는
  정상 경로에서 유지된다. timeout이 지나면 Framework가 physical connection을 닫으며, client는
  관측 가능한 disconnect 뒤 재연결한다(§7.5).
- border sync와 announce는 publish이며 target handler 완료를 성공 기준으로 사용하지 않는다.
- 점검 모드는 desired state를 store에 기록하고 fanout으로 전 node에 알린다. target Spot의
  `OnActorJoin` admission이 **유일한** 최종 판정자다. source/Entry의 maintenance cache는
  관측·최적화 용도로만 쓰며 그 값으로 client-facing terminal 결과를 만들지 않는다. 점검 중
  허용 범위는 같은 zone 내부 이동뿐이다(같은 NodeId의 다른 zone 이동도 거부한다).
- runtime node 상태는 polling이 아니라 runtime event와 explicit report로 관찰한다.
  Registered는 ZoneNode explicit report 기반이며, 마지막 report 후 15초(report 주기 5초의
  3배)가 지나면 false로 관찰한다. crash된 node는 false report를 보낼 수 없으므로 이 TTL이
  유일한 false 전환 규칙이다.
- Ready owner 장애는 자동 replacement가 아니며 해당 operation은 Unavailable로 끝난다.

### 2.3 표면 선택 기준

| 업무 | Framework 표면 | 이유 |
|---|---|---|
| node 등록·연결 변화 관찰 | runtime event | 대상 node에 request를 보내지 않아도 변화가 전달된다. |
| 전 node 공지 | classic fanout | publisher가 subscriber 목록을 관리하지 않는다. |
| 특정 NodeId 점검 | desired state + fanout | NodeId는 application label이고 transport RID가 아니다. |
| 인접 zone snapshot | Logical Multicast | topic별 인접 zone 구독자에게 publish한다. |
| 특정 player push | bound session | Actor의 현재 binding이 connection 위치를 해결한다. |

## 3. 시스템 구성과 topology

기본 topology는 Client와 server component의 배치와 연결만 표현한다. Redis와 maintenance
store는 resource 표에서 설명하며 이동·publish 시간 순서는 §7 sequence diagram에서 설명한다.

<iframe class="zlink-diagram" src="/common/diagrams/sample-zoneworld-topology.html" title="ZoneWorld topology — Client · Server 배치와 연결" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-zoneworld-topology.html" target="_blank">↗ 크게 보기</a></p>

- Gateway만 player-facing game STREAM을 제공하고 Ops만 control STREAM을 제공한다.
- ZoneNode A/B는 같은 executable capability로 실행하며 Zone Spot factory(stable type
  `zoneworld.zone`, ZoneId별 네 Spot instance), Player Actor factory(stable type
  `zoneworld.player`), zone Channel과 report channel을 등록한다. 모든 언어가 이 canonical
  stable type 문자열을 동일하게 등록한다(actorJoin은 stable type을 wire에 싣지 않고 Location
  Store authority row로 해석하므로 이름이 갈리면 cross-language join이 실패한다).
- 각 ZoneNode는 Zone Spot capacity를 2로 선언한다. 네 zone은 capacity에 의해 두 node에
  2/2로 분산되며, 2x2 격자의 어떤 2/2 분할도 서로 다른 owner의 인접 zone pair를 보장한다.
  runner는 zone→NodeId를 가정하지 않고 Ops probe로 실제 owner 배치를 발견해 cross-owner
  경계를 선택한다. fixture나 test가 특정 zone을 특정 NodeId에 고정 배치하는 것은 금지한다.
  각 ZoneNode의 bootstrap은 첫 zone을 claim한 뒤 두 번째 claim에서 **그 인접 zone을 우선**
  한다(연속영역 선호). 이로써 2/2 분할이 항상 두 연속 영역이 되어 cross-owner 인접쌍과
  same-owner 인접쌍(ZW-E4의 전제)이 모두 결정적으로 존재한다 — 대각 분할({nw,se}/{ne,sw})은
  same-owner 인접쌍이 없어 E4를 불충족으로 만들므로 배제한다. 선호는 claim 시도 순서일 뿐
  owner 계산이 아니며, placement 판정은 여전히 Framework capacity가 소유한다.
- **ready 신호 문자열을 고정한다.** ZoneNode는 준비를 마치면 표준 출력에
  `topology=ready node=<NodeId> zones=<쉼표로 이은 ZoneId>`를 정확히 한 줄 낸다. zone이
  없으면 `zones=` 뒤를 비운다. runner가 이 줄을 기다려 다음 단계로 넘어가므로 언어마다
  문자열이 다르면 같은 runner 절차를 쓸 수 없다. 이 줄에 다른 field를 덧붙이지 않는다.
- **Bootstrap은 zone 2개를 확보한 뒤에 ready를 알린다.** ZoneNode는 startup에서 claim을
  시도하고, 자기 census가 zone 2개가 될 때까지 반복한다. 확보하기 전에 ready를 알리면
  `ZW-C1`이 "두 ZoneNode의 Registered·Connected 각각 정확"을 단언할 때 zone을 갖지 않은
  node도 통과시켜 단언의 뜻이 달라진다. factory만 등록하고 첫 요청에서 zone Spot을 만드는
  구현은 이 조건을 만족하지 않는다.
- **claim 재시도는 `250 ms` 간격으로 최대 `120`회다.** 다른 ZoneNode가 아직 뜨지 않아
  capacity가 남아 있을 수 있으므로 즉시 실패하지 않고 반복한다. 120회를 모두 쓰고도 zone
  2개를 확보하지 못하면 startup 실패로 끝낸다 — 조용히 zone 없이 ready를 알리지 않는다.
  간격과 횟수를 언어마다 다르게 두면 같은 시나리오가 언어별로 다른 시점에 실패해 판정이
  갈리므로 값을 고정한다.
- **crash 교체 process는 zone을 확보하지 않고 ready를 알린다.** §7.5가 crash replacement를
  "새 object를 수용할 수 있게 되는 것"으로 정의하고 이전 owner object의 자동 복원을 금지하기
  때문이다. 이 경우에만 zone 0개로 ready이며, runner가 그 의도를 명시적으로 켠다. 설정
  이름은 `allowEmptyZoneSet`으로 고정한다(각 언어의 이름 규칙을 따라 표기만 바꾼다 —
  `allow_empty_zone_set`, `allowsEmptyZoneSet`). 이 경로에서는 `attempt`가 `8`에 이르고
  census가 비어 있으면 재시도를 멈추고 ready를 알린다. 일반 startup에서는 이 경로를 쓰지
  않는다.
- zoneworld.mesh는 ChannelName, Spot·Actor direct message와 Logical Multicast를 운반한다.
- zoneworld.broadcast는 mesh와 독립된 classic fanout publisher/subscriber 연결이다.
- Zone Spot·Player Actor 같은 object의 owner는 Location Store placement가 선택한다.
  NodeId와 transport RID는 별도 domain이다.
- Client에는 Gateway와 Ops endpoint만 제공하며 ZoneNode endpoint를 노출하지 않는다.

| Resource | 책임 | 준비 |
|---|---|---|
| Location Store | peer descriptor, ZoneId Spot authority와 Actor location | 실행별 공유 Redis |
| Relocation Store | Player Actor relocation의 operation recovery record(relocation 뒤 pending request terminal 기록) | Location Store와 분리한 provider·key prefix를 사용하는 실행별 Redis keyspace |
| Maintenance store | NodeId별 desired state | 실행별 공유 Redis keyspace |
| Zone state | actor 좌표 사본, border snapshot과 tick | Zone Spot |
| Player actor state | 좌표, zone, bot 방향 | Player Actor relocation adapter |
| Runtime evidence | node status, alert와 relocation probe | Ops runner |

## 4. 역할과 책임

| 역할 | 수 | 책임 | 분리 이유와 소유 상태 |
|---|---:|---|---|
| Game Browser | 1 이상 | JoinWorld, Move, state notify와 announce 확인 | server push만으로 화면 state를 갱신한다. |
| Ops Browser | 1 | node watch, announce, maintenance와 diagnose | 게임 domain state와 운영 desired state를 분리한다. |
| Gateway | 1 | game STREAM, Player Actor binding, relay와 push | browser connection 수명을 zone owner와 분리한다. |
| ZoneNode | 2 | Entry Spot, 네 Zone Spot, Player Actor, bot timer와 local report | zone object를 여러 owner 후보에 분산한다. |
| Ops | 1 | Ops STREAM, runtime event 수집, fanout publish와 maintenance store | node 목록을 publisher code에 하드코딩하지 않는다. |
| Zone Spot | ZoneId별 1 | player 사본, border snapshot, tick과 admission | zone view state의 단일 소유자다. |
| Player Actor | PlayerId별 1 | X, Y, ZoneId 권위와 이동·relocation | client 입력의 권위 판정을 한 곳에 둔다. |

NodeId는 zone owner 계산에 사용하지 않는 application label이다. MeshNode RID는 prefix와 UUID로
Framework가 자동 발급하며 고정 RID를 설정하지 않는다.

## 5. 사용하는 Framework 요소와 선택 이유

| 필요한 동작 | 선택한 요소 | 선택 이유와 계약 근거 |
|---|---|---|
| ZoneId로 현재 zone owner를 찾는다. | global Spot message | global SpotId authority를 Framework가 resolve한다. [상호작용 모델 §2](../../spec/server/00-foundation/04-interaction-model.ko.md) |
| PlayerId로 actor를 찾는다. | global Actor message | Actor location과 current owner를 application route로 노출하지 않는다. [Actor model](../../spec/server/03-spot-actor/04-actor-model.ko.md) |
| zone join을 cross-node 이동으로 사용한다. | Actor Join + relocation | target owner가 다르면 Framework relocation unit이 actor를 이동시킨다. relocation 전체 순서(owner 전환·relay·target queue·CAS)의 단일 권위는 [Relocation flow](../../spec/server/05-location-relocation/04-relocation-flow.ko.md)이고, target admission·membership·lifecycle은 [Spot·Actor membership §4.2](../../spec/server/03-spot-actor/05-spot-actor-membership.ko.md#42-다른-node의-spot으로-actor를-join하는-순서)가 소유한다. |
| 이동 중 이전 owner message를 전달한다. | Message Follow | committed target route를 사용하며 실패한 operation을 다른 owner에 재제출하지 않는다. [Object routing §2.4](../../spec/server/03-spot-actor/08-routing.ko.md#25-이전-owner-route에-도착한-message) |
| 인접 zone에 snapshot을 전달한다. | Logical Multicast | topic과 target subscription으로 경계를 표현한다. [상호작용 모델 §5](../../spec/server/00-foundation/04-interaction-model.ko.md#5-spot-logical-multicast) |
| 전 node 공지·점검을 보낸다. | classic fanout | publisher가 node 목록을 관리하지 않는다. [상호작용 모델 §6](../../spec/server/00-foundation/04-interaction-model.ko.md#6-classic-fanout) |
| node 상태를 관찰한다. | runtime monitoring event | 상태 변화와 local report를 Ops에서 수집한다. [Runtime monitoring](../../spec/server/06-observability/01-runtime-monitoring.ko.md) |
| actor 연결을 유지한다. | bound STREAM session | relocation 중 같은 connection을 유지하고 binding 위치만 갱신한다. [Failure policy §6](../../spec/server/05-location-relocation/06-failure-failover-policy.ko.md#6-session과-binding) |
| RID 충돌을 피한다. | SetRoutingIdPrefix zn | application NodeId나 ZoneId와 transport identity를 분리한다. [MeshNode spec](../../spec/server/03-spot-actor/03-mesh-node.ko.md) |

Player Actor factory는 `PreserveStateWith` relocation adapter를 등록한다. Capture/Restore payload는
좌표, ZoneId, bot 방향과 마지막 적용 movement ID처럼 Application이 소유하는 state만 보존한다.
Framework가 보존하는 queue, accepted journal, logical timer, membership과, owner가 바뀔 때마다 Framework가 전진시키는 owner fence는 포함하지 않는다.
이동하지 않는 Zone Spot factory는 `DisableRelocation`을 선택한다.

## 6. Message 계약

ZoneWorld는 typed JSON codec을 사용한다. 아래 declaration은 .NET, Node.js와 shared TypeScript
browser가 유지할 JSON wire 이름과 optional·null 의미다.

Player-facing wire는 **logical-only**다: NodeId, transport RID, relocation 발생 여부
(`transferred` 류 flag), owner 정보를 game message에 싣지 않는다. 이런 physical 관측이
필요한 화면(HUD·데모)은 Ops contract(WatchNodes/Diagnostics)에서 공급받는다. shared browser를
포함한 어떤 client도 이 declaration에 physical field를 추가하지 않는다.

업무 실패는 다음 typed 매핑으로만 관측한다(자유 문자열로 언어별 예외 텍스트를 노출하지
않는다):

| 실패 | client 관측 |
|---|---|
| 이동 거부(OutOfRange/TooFar/DiagonalCrossing/ZoneMaintenance) | `MoveRejectedNotify.reason`의 해당 코드 |
| JoinWorld의 zone admission 거부(점검 등) | `JoinWorldRes.error`의 typed 코드(예: `ZoneMaintenance`) |
| 그 외 Framework Join/request 실패 | 해당 `error` field에 [Spot·Actor membership §4](../../spec/server/03-spot-actor/05-spot-actor-membership.ko.md)의 public failure kind 이름을 **그대로**(예: `NotFound`, `CapacityExceeded`, `InternalFailure`, `DataLost`, `InvalidOperation`, `ShuttingDown`) — 이 폐쇄 집합 밖 문자열 금지 |
| target owner crash로 인한 operation 종료 | `JoinWorldRes.error`/해당 request의 `error` = `Unavailable` |
| request deadline 초과 | 해당 request의 `error` = `DeadlineExceeded` |
| session route 갱신 timeout | WebSocket close(별도 message 없음, §7.5) |

### 6.1 Game STREAM message

```text
message PlayerView {
  playerId: string
  x: int32
  y: int32
  zoneId: string
  isBot: bool
}

message JoinWorldReq {
  playerId: string
}

message JoinWorldRes {
  playerId: string
  zoneId: string
  x: int32
  y: int32
  error?: string | null
}

message MoveMsg {
  x: int32
  y: int32
}

message ZoneStateNotify {
  zoneId: string
  tick: int64
  players: PlayerView[]
}

message ZoneChangedNotify {
  playerId: string
  zoneId: string
}

message WorldAnnounceNotify {
  announcementId: string
  text: string
}

message MoveRejectedNotify {
  reason: string
  x: int32
  y: int32
}
```

MoveMsg는 response가 없는 one-way send다. ZoneChangedNotify는 논리 ZoneId 변경만 알리고
physical relocation 여부나 owner RID를 노출하지 않는다.

### 6.2 Ops STREAM message

```text
message NodeView {
  nodeId: string
  registered: bool
  connected: bool
  maintenance: bool
  zones: string[]
  playerCount: int32
}

message WatchNodesReq {}

message WatchNodesRes {
  nodes: NodeView[]
}

message NodeStatusNotify {
  nodeId: string
  registered: bool
  connected: bool
  maintenance: bool
  zones: string[]
  playerCount: int32
}

message NodeAlertNotify {
  nodeId: string
  kind: string
  detail: string
  occurredAt: string
}

message AnnounceWorldReq {
  text: string
}

message AnnounceWorldRes {
  announcementId: string
}

message SetMaintenanceReq {
  nodeId: string
  enabled: bool
}

message SetMaintenanceRes {
  nodeId: string
  enabled: bool
  zones: string[]
  error?: string | null
}

message NodeDiagnosticsReq {
  nodeId: string
}

message NodeDiagnosticsRes {
  nodeId: string
  zones: string[]
  playerCount: int32
  maintenance: bool
  error?: string | null
}
```

NodeId는 Ops가 표시하는 application identifier다. NodeDiagnosticsReq와 SetMaintenanceReq는
current node report에 나타난 NodeId를 사용하며 RID를 입력으로 받지 않는다.

### 6.3 내부 routing, border와 probe message

```text
message WorldAnnounceEvent {
  announcementId: string
  text: string
}

message NodeMaintenanceChangedEvent {
  nodeId: string
  enabled: bool
}

message DeliverAnnounceMsg {
  announcementId: string
  text: string
}

message BotTickMsg {}

message EnterWorldReq {
  x: int32
  y: int32
  isBot: bool
  dirX?: int32
  dirY?: int32
}

message EnterWorldRes {
  zoneId: string
  x: int32
  y: int32
  error?: string | null
}

message ReportSpotEventMsg {
  nodeId: string
  kind: string
  detail: string
  occurredAt: string
}

message ReportNodeStatusMsg {
  nodeId: string
  zones: string[]
  playerCount: int32
  maintenance: bool
}

message ZoneBorderEvent {
  fromZoneId: string
  toZoneId: string
  tick: int64
  players: PlayerView[]
}

message EnterZoneReq {
  playerId: string
  x: int32
  y: int32
  isBot: bool
  initialEntry: bool
}

message EnterZoneRes {
  zoneId: string
  error?: string | null
}

message UpdatePositionMsg {
  playerId: string
  x: int32
  y: int32
  isBot: bool
}

message DeliverZoneStateMsg {
  zoneId: string
  tick: int64
  players: PlayerView[]
}

message DeliverWorldAnnounceMsg {
  announcementId: string
  text: string
}
```

WorldAnnounceEvent와 NodeMaintenanceChangedEvent는 classic fanout publish payload다.
ZoneBorderEvent는 Logical Multicast publish payload다. ReportSpotEventMsg와
ReportNodeStatusMsg는 ZoneNode가 Ops channel에 보내는 one-way message다. MessageFollowProbe
와 ActorLocationProbe는 runner-only evidence이며 browser application contract에 넣지 않는다.

## 7. 업무 흐름

### 7.1 입장과 같은 zone 이동

시작 상태는 Gateway, 두 ZoneNode와 Ops가 readiness를 완료하고 browser가 Gateway에 STREAM
연결한 상태다. JoinWorld는 (25,25)의 zone-nw로 시작한다. actor가 같은 zone 안에서 이동하면
Actor가 좌표를 갱신하고 Zone Spot에 UpdatePositionMsg를 보내 사본을 갱신한다.

Zone join은 Framework Actor Join이므로 handler 안에서 동기적으로 완료되지 않는다. Actor는
join을 `Defer()`로 등록하고 현재 handler를 정상 종료하며, join 결과는 completion callback으로
도착한다([Spot·Actor membership §3](../../spec/server/03-spot-actor/05-spot-actor-membership.ko.md)). 따라서
`JoinWorldRes`는 join completion callback에서 발신한다 — **JoinWorldRes 성공 = target zone
admission까지 완료**가 이 시나리오의 규범 의미이며, admission 이전 상태(cache 등)로
JoinWorldRes terminal을 만드는 구현은 비적합이다.

<iframe class="zlink-diagram" src="/common/diagrams/sample-zoneworld-join-move.html" title="입장과 같은 zone 이동 — JoinWorld · Move" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-zoneworld-join-move.html" target="_blank">↗ 크게 보기</a></p>

### 7.2 경계 이동과 relocation

target zone owner가 같으면 membership만 바뀌고, 다르면 같은 Player Actor가 target owner에서
materialize되는 relocation이 발생한다. Application은 두 경우를 NodeId로 구분하지 않는다.
두 경우 모두 request/reply인 `EnterZoneReq`와 `EnterZoneRes`를 사용한다.

<iframe class="zlink-diagram" src="/common/diagrams/sample-zoneworld-relocation.html" title="경계 이동과 relocation" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-zoneworld-relocation.html" target="_blank">↗ 크게 보기</a></p>

Relocation은 ActorId와 ObjectGeneration을 유지하고 owner generation만 바꾼다. relocation 중
이전 owner에 도착한 one-way 또는 request message는 committed target으로 Follow한다. source는
Follow 중 Location Store를 다시 조회하거나 다른 owner에 같은 operation을 자동 제출하지 않는다.

### 7.3 Border snapshot과 bot

각 Zone Spot은 tick마다 자기 zone과 인접 snapshot으로 ZoneStateNotify를 만들고, 인접 zone별
topic에 ZoneBorderEvent를 publish한다. 수신자는 FromZoneId별 최신 Tick만 보관하고 3 tick 동안
갱신이 없으면 제거한다. 같은 PlayerId가 자기 zone과 border snapshot에 동시에 있으면 자기 zone
값을 사용한다.

Bot은 사람과 같은 Player Actor type이며 bound session이 없다. 네 zone에 X 방향 bot 하나와
Y 방향 bot 하나를 두고, 500ms BotTickMsg마다 3칸 이동한다. 이동이 거부되면 방향을 반전한다.
초기 좌표와 방향은 다음과 같이 고정한다.

| PlayerId | 시작 좌표 | 방향 | 경계 효과 |
|---|---|---|---|
| bot-nw-x | (10,15) | (+1,0) | X 경계 cross-node relocation |
| bot-nw-y | (15,10) | (0,+1) | X 경계 없음 |
| bot-ne-x | (90,15) | (-1,0) | X 경계 cross-node relocation |
| bot-ne-y | (85,10) | (0,+1) | X 경계 없음 |
| bot-sw-x | (10,85) | (+1,0) | X 경계 cross-node relocation |
| bot-sw-y | (15,90) | (0,-1) | X 경계 없음 |
| bot-se-x | (90,85) | (-1,0) | X 경계 cross-node relocation |
| bot-se-y | (85,90) | (0,-1) | X 경계 없음 |

### 7.4 Ops 관찰, announce와 maintenance

Ops는 runtime event와 ZoneNode의 explicit report를 NodeStatusNotify와 NodeAlertNotify로
변환한다. 여기서 runtime event는 [Runtime monitoring](../../spec/server/06-observability/01-runtime-monitoring.ko.md)의
현재 상태 조회·변화 관찰 surface를 뜻하며, 각 항목은 부분 event가 아니라 완전한 status다.
WatchNodesRes의 Registered와 Connected는 서로 다른 관측값이다. Connected는 runtime status
관찰의 peer state에서 얻고, Registered는 Framework topology status가 등록 신호를 노출하지
않으므로 ZoneNode의 explicit report에서 얻는다.

<iframe class="zlink-diagram" src="/common/diagrams/sample-zoneworld-ops.html" title="Ops 관찰, announce와 maintenance" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-zoneworld-ops.html" target="_blank">↗ 크게 보기</a></p>

Target zone owner가 maintenance=true이면 target Zone Spot의 OnActorJoin admission이
ZoneMaintenance로 거부한다. 허용 범위는 같은 zone 내부 이동뿐이다(같은 NodeId의 다른 zone
이동도 신규 admission이므로 거부한다). fanout cache가 stale해도 target admission이 유일한
최종 판정자이며 source/Entry cache는 terminal을 만들지 않는다. Ops는 desired state를
maintenance store에 기록하므로 ZoneNode 재시작 뒤 같은 NodeId의 maintenance state를 복원한다.

이 maintenance는 application admission desired state이며 [Host relocation flow]
(../../spec/server/05-location-relocation/05-host-relocation-flow.ko.md)의 `Relocate(PlannedMaintenance)`를 호출하지
않는다 — ZW-E는 Spec 30 host relocation의 검증 대상이 아니다(그 커버리지는 별도 harness가
소유한다).

### 7.5 Failure와 failover 경계

Ready ZoneNode owner process가 종료되면 현재 Actor·Spot operation은 Unavailable로 끝난다.
Framework는 다른 ZoneNode에서 새 Actor incarnation을 자동으로 만들지 않는다. planned
relocation은 target-only Location Store CAS commit 규칙을 따르는 별도 operation이며 crash
failover가 아니다.

relocation 중 bound session route 갱신이 seal timeout 안에 적용되지 않으면 Framework는
physical connection을 닫는다. client의 관측 결과는 WebSocket close이며, client는 재연결 후
JoinWorld를 다시 수행한다(같은 PlayerId로 기존 Actor에 재바인딩). 이 실패 경로는 §9.1의
self-check 항목으로 관측한다.

"crash replacement"는 같은 NodeId로 **새 process(새 transport RID)** 를 시작해 새 object를
수용할 수 있게 되는 것을 뜻한다. 이전 Ready owner가 소유하던 object의 자동 복원·재생성이
아니며, 그 object들의 미완 operation은 Unavailable 경계로 끝난 상태가 유지된다.

## 8. 구현 구조

.NET과 Node.js server는 `Client`, `Shared`, `Server`를 같은 순서로 두고 아래 logical component를
같은 책임으로 유지한다. headless scenario와 browser client의 파일 위치는 달라도 Gateway, ZoneNode와
Ops의 경계, zone state owner와 relocation adapter의 위치는 바꾸지 않는다.

<iframe class="zlink-diagram" src="/common/diagrams/sample-zoneworld-structure.html" title="구현 구조 — Client · Shared · Server" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-zoneworld-structure.html" target="_blank">↗ 크게 보기</a></p>

| Logical component | 모든 언어에서 유지할 책임 | 의존 방향과 금지 경계 |
|---|---|---|
| `Client/Program` | headless runner와 browser adapter의 설정·실행 진입점을 구성한다. | ZoneNode owner와 runtime RID를 선택하지 않는다. |
| `Client/HeadlessScenario` | join, move, border, bot, announce, maintenance와 §9 assertion을 실행한다. | ZoneNode owner와 transport RID를 직접 선택하지 않는다. |
| `Client/BrowserGame`·`BrowserOps` | 같은 wire contract로 game·ops 화면의 response와 push를 확인한다. | browser별 별도 message codec을 만들지 않는다. |
| `Shared/Configuration` | role, Mesh·fanout, zone fixture와 runner marker를 고정한다. | NodeId와 MeshNode RID를 같은 값으로 취급하지 않는다. |
| `Shared/JSON Contracts` | game, ops, internal routing과 runtime event의 wire 의미를 소유한다. | .NET/Node 전용 field를 공통 계약에 추가하지 않는다. |
| `Shared/WorldRules` | coordinate, zone, rejection order와 border policy를 계산한다. | Framework type과 transport를 참조하지 않는다. |
| `Server/Gateway/Application` | Player binding, relay와 client-facing result mapping을 조정한다. | zone state를 직접 변경하지 않는다. |
| `Server/Gateway/Infrastructure` | WebSocket·STREAM, handler와 push adapter를 연결한다. | frame codec과 owner route를 application에 노출하지 않는다. |
| `Server/ZoneNode/Domain` | zone snapshot, player state view와 border 규칙을 계산한다. | ActorRef를 state에 cache하지 않는다. |
| `Server/ZoneNode/Application` | movement, admission, bot tick과 relocation 전후 결과를 조정한다. | NodeId를 Framework routing identity로 사용하지 않는다. |
| `Server/ZoneNode/Infrastructure` | Entry Spot, Zone Spot, Player Actor, relocation, border와 local report를 연결한다. | raw frame과 private runtime API를 사용하지 않는다. |
| `Server/Ops/Application` | node watch, announcement, maintenance와 diagnostics 업무 결과를 조정한다. | game domain state를 직접 변경하지 않는다. |
| `Server/Ops/Infrastructure` | Ops stream, runtime event, fanout과 maintenance store를 연결한다. | node 목록을 publisher code에 하드코딩하지 않는다. |

WorldRules는 coordinate, zone, rejection order와 border policy를 소유한다. Gateway는 stream
transport와 session binding만 소유한다. ZoneSpot은 actor coordinate의 사본, tick과 border
snapshot을 소유하며 ActorRef를 cache하지 않는다. PlayerActor는 coordinate와 ZoneId의 권위 상태,
relocation adapter와 bound push를 소유한다. Ops는 NodeId desired state와 runtime evidence를
소유한다. browser transport는 platform WebSocket을 stream connector로 연결하며 application이
frame codec을 다시 구현하지 않는다.

언어별 구현은 Gateway·ZoneNode·Ops를 하나의 server module로 합치거나, Zone state를 Gateway 또는
Ops에 복제하지 않는다. .NET과 Node.js가 내부 type을 다르게 표현할 수는 있지만 같은 logical
component와 wire declaration을 찾을 수 있어야 한다. 언어별로 달라질 수 있는 것은 host·browser
adapter, async 표현과 runtime event wrapper이며, relocation, border, bot, fanout과 self-check 순서는
공통 문서와 같아야 한다.

.NET의 attribute, Java·Kotlin의 annotation과 Node.js의 decorator는 선언형 metadata scan으로
handler를 자동 등록한다. C++은 runtime reflection scanner가 없으므로 compile-time type과 public
builder로 같은 handler 집합을 명시 등록한다. 이 차이는 등록 방법에만 적용하며 message와 처리
책임을 바꾸지 않는다.

## 9. Client self-check

### 9.1 Game browser

1. JoinWorldRes가 zone-nw, (25,25)를 반환하는지 확인한다.
2. 같은 zone MoveMsg 뒤 ZoneStateNotify에서 좌표와 ZoneId를 확인한다.
3. 범위 초과·이동량 초과·대각선 crossing·maintenance 거부가 정의한 순서와 이유를 반환하는지
   확인한다.
4. 서로 다른 player 두 명이 같은 ZoneStateNotify에 있고 PlayerId UTF-8 byte 순서가 맞는지
   확인한다.
5. 인접 zone에만 border snapshot이 도착하고 대각선 zone에는 도착하지 않는지 확인한다.
6. border snapshot이 tick 역순을 무시하고 3 tick 동안 갱신되지 않으면 제거되는지 확인한다.
7. 서로 다른 owner의 인접 zone pair를 runner가 선택한 뒤 cross-node 이동을 수행한다. 같은
   ActorId와 ObjectGeneration, 유지된 WebSocket binding과 ZoneChangedNotify를 확인한다. pair가
   없으면 release gate를 통과시키지 않는다.
8. relocation 직후 이전 owner route로 one-way와 request probe를 보내 operation id, generation,
   payload와 reply route가 보존되는지 확인한다. Follow 중 source가 Store를 다시 조회하거나
   hidden retry를 하면 실패다.
9. X 순찰 bot과 Y 순찰 bot의 deterministic 이동, 거부 뒤 방향 반전을 확인한다. bot에는 client
   push가 없어야 한다.

### 9.2 Ops browser

1. WatchNodesRes에서 Registered와 Connected를 각각 확인한다.
2. NodeStatusNotify와 NodeAlertNotify가 runtime event와 local report를 반영하는지 확인한다.
3. AnnounceWorldReq 뒤 AnnouncementId가 중복 없이 game client에 도착하는지 확인한다. fanout
   publish 완료나 subscriber handler 완료를 client 성공 조건으로 삼지 않는다.
4. SetMaintenanceReq는 선택한 NodeId만 변경하고 desired state가 store에 기록되는지 확인한다.
5. 점검 중 target zone 신규 join은 ZoneMaintenance로 거부하고 같은 zone 이동은 허용하는지
   확인한다.
6. NodeDiagnosticsReq가 최신 zone 목록, player count와 maintenance를 반환하는지 확인한다.
7. ZoneNode를 정상 종료하고 재시작한 뒤 새 transport RID와 같은 NodeId report를 확인한다.
8. ZoneNode crash 뒤 replacement가 이전 owner의 자동 failover가 아니라는 것을 Unavailable
   경계로 확인한다.

### 9.3 routing ID gate

- MeshNode RID는 zn-<lowercase-canonical-uuid-v4> 형식이며 고정 RID 설정과 SetRoutingId 호출이 없다.
- ChannelName, Spot과 Actor가 별도 transport RID를 만들지 않는다.
- process 시작 순서, 정상 교체와 crash 교체에서 global ZoneId routing은 NodeId와 독립적으로
  동작한다. 여기서 "동작"은 새 process가 새 RID로 같은 NodeId report를 내고 이후의 **새**
  object 생성·routing이 정상이라는 뜻이다. crash 이전 owner object의 자동 복구를 뜻하지
  않는다(§7.5).
- 관측용 OwnerNodeRid는 probe evidence에만 사용하고 application message 또는 placement
  input으로 전달하지 않는다.

## 10. Smoke 실행

1. 실행별 Location Store, Relocation Store와 maintenance store를 준비한다. 두 Framework store는 같은
   Redis deployment를 사용할 수 있지만 provider와 key prefix를 분리한다.
2. Ops를 시작하고 control STREAM readiness를 확인한다.
3. ZoneNode A/B를 시작하고 zone capability, mesh peer와 fanout readiness를 확인한다.
4. Gateway를 시작하고 game STREAM readiness를 확인한다.
5. headless self-check와 Chromium browser scenario를 실행한다.
6. 정상 replacement, crash replacement와 cross-owner pair probe를 별도 실행으로 확인한다.
7. runtime evidence와 completion marker를 확인한다.
8. 성공·실패 모두에서 이번 실행이 만든 resource와 process를 정리한다.

```text
zoneworld=completed
```

runner는 completion marker와 함께 relocation, border sync, Ops observe·announce·maintenance
evidence를 검사한다. 단계별 marker는 언어별 runner가 실제로 출력하는 경우에만 사용하며,
공통 문서의 message 계약이나 topology 이름으로 취급하지 않는다.

## 11. 완료 기준

- .NET, Node.js와 shared TypeScript browser가 같은 JSON declaration과 업무 의미를 사용한다.
- 기본 topology가 Client와 server component, STREAM, RouteMesh와 fanout 연결만 표현한다.
- Actor가 좌표 권위를 소유하고 Zone Spot은 사본과 border snapshot만 소유한다.
- 이동 rejection order, zone geometry, tick, sort와 expiry 규칙이 모든 언어에서 같다.
- cross-node join이 같은 ActorId와 ObjectGeneration, 유지된 session binding을 보존한다.
- 반복 relocation으로 Actor가 이전에 방문한 node로 돌아와도 같은 identity와 bound session을
  보존한다(A→B→A round trip, ZW-B7).
- Message Follow가 명시한 제한과 terminal error를 self-check가 확인한다.
- border sync는 인접 topic만 사용하고 대각선 zone에는 publish하지 않는다.
- announce와 maintenance는 classic fanout, node status는 runtime event와 explicit report를 사용한다.
- NodeId와 transport RID를 구분하고 자동 routing ID gate를 통과한다.
- bot은 bound session 없이 같은 PlayerActor 규칙으로 이동하며 client push를 만들지 않는다.
- Framework public API와 typed JSON codec만 사용하고 raw frame, private route와 custom owner
  selection을 추가하지 않는다.
- Ready owner 장애를 crash failover로 표시하지 않고 Unavailable 경계를 유지한다.
- runner가 build, readiness, browser/headless self-check, evidence와 cleanup을 수행한다.

### 11.1 시나리오 ID 계열

self-check 시나리오 ID(`ZW-*`)는 의도별 계열로 묶인다. 각 계열은 위 완료 기준의 한 축을
검증하며, runner의 evidence는 개별 ID를 명시한다.

| 계열 | 의도 |
| --- | --- |
| ZW-A | 이동 기본: 입장, zone 내 이동, rejection order, 가시성, 정렬 |
| ZW-B | relocation과 session: border sync, cross-node relocation, identity·binding 연속성 — 이전에 방문한 node로 되돌아가는 A→B→A round trip인 ZW-B7 포함 |
| ZW-C | Ops 관찰: node status, shutdown, disconnect, spot event report |
| ZW-D | fanout announce: 한 번의 publish가 모든 node의 subscriber와 zone spot에 도달 |
| ZW-E | maintenance: 대상 지정 enable/disable, 입장 거부, 재시작 지속성, diagnostics |
| ZW-F | bot: client 없는 이동, population, client push 부재, rejection 시 방향 반전 |
| ZW-G | node identity와 교체: NodeId와 transport RID 구분, routing ID gate, replacement |

### 11.2 개별 시나리오 정의 (canonical)

모든 언어 runner는 아래 개별 정의를 구현하고, `zoneworld=completed`는 구현한 전체 ID의
판정 AND로만 출력한다. 전제(P)는 명시하지 않으면 "§10 순서로 전 구성 요소 ready + browser
또는 headless client가 Gateway에 연결"이다.

| ID | 전제 | 행동 | 단언 |
| --- | --- | --- | --- |
| ZW-A1 | 기본 | JoinWorldReq | JoinWorldRes = zone-nw,(25,25); §7.1 규범대로 admission 완료 후 응답 |
| ZW-A2 | A1 | 같은 zone MoveMsg | ZoneStateNotify에 갱신 좌표·ZoneId |
| ZW-A3 | A1 | 범위 밖·6칸 초과·대각선·점검 이동 각 1회 | MoveRejectedNotify reason이 OutOfRange→TooFar→DiagonalCrossing→ZoneMaintenance 고정 순서 |
| ZW-A4 | player 2명 같은 zone | 각자 Move | 동일 ZoneStateNotify에 두 player 존재 |
| ZW-A5 | A4 | ZoneStateNotify 수신 | Players가 PlayerId UTF-8 byte 오름차순, 자기 zone 값 우선 |
| ZW-B1 | 경계 band 내 player | tick 대기 | 인접 zone에만 border snapshot 도착, 대각선 zone 미도착 |
| ZW-B2 | cross-owner 인접 pair(§3 capacity 분산, probe로 발견) | 경계 넘는 MoveMsg | relocation 완료, ZoneChangedNotify, 같은 WebSocket으로 후속 notify |
| ZW-B3 | B2 직후 | ActorLocationProbe | 같은 ActorId·ObjectGeneration, owner generation만 전진 |
| ZW-B4 | B1 상태에서 publish 중단 | 3 tick 경과 | 해당 FromZoneId snapshot 제거(expiry) |
| ZW-B5 | B2 | 이전 owner route로 one-way probe | committed target에서 정확히 1회 처리(Follow), 재제출 없음 |
| ZW-B6 | B2 | 이전 owner route로 request probe | operation id·generation·payload·reply route 보존, source Store 재조회·hidden retry 없음 |
| ZW-B7 | B2 | 역방향 이동으로 원래 owner 복귀(A→B→A) | 동일 identity·binding 유지 |
| ZW-B8 | B2 가능 상태 | runner가 session route 갱신 전달을 seal timeout 이상 지연/차단(주입) 후 경계 이동 | Framework가 physical connection을 닫음(WebSocket close 관측), client 재연결·JoinWorld 재수행으로 같은 PlayerId의 기존 Actor에 재바인딩(§7.5) |
| ZW-C1 | 기본 | Ops WatchNodesReq | 두 ZoneNode의 Registered·Connected 각각 정확 |
| ZW-C2 | C1 | ZoneNode 정상 종료 | Connected=false 관측(runtime event, polling 아님) |
| ZW-C3 | C2 | report TTL 15초 경과 | Registered=false 관측(§2.2 TTL 규칙) |
| ZW-C4 | 기본 | zone tick timer 실패 주입(runner) | spot event report로 실패 관측, zone 정지 없음 |
| ZW-D1 | 기본 | AnnounceWorldReq | 모든 node·zone의 game client에 AnnouncementId 중복 없이 1회 도달 |
| ZW-D2 | D1 + 제3 subscriber 추가 | 재announce | 새 subscriber 포함 전원 수신(publisher 목록 하드코딩 없음 증명) |
| ZW-E1 | 기본 | SetMaintenanceReq(node,true) | 해당 NodeId만 desired state 변경, store 기록 |
| ZW-E2 | E1 | 점검 node의 zone으로 신규 join | target OnActorJoin이 ZoneMaintenance 거부(§7.4 단독 판정) |
| ZW-E3 | E1 | 점검 zone 내부 이동 | 허용 |
| ZW-E4 | E1 | 점검 node의 다른 zone으로 이동 | ZoneMaintenance 거부(same-zone만 허용) |
| ZW-E5 | E1 | ZoneNode 재시작 | 같은 NodeId의 maintenance state 복원 |
| ZW-E6 | 기본 | NodeDiagnosticsReq | 최신 zone 목록·player count·maintenance 반환 |
| ZW-F1 | 기본 | bot 관찰 | 8 bot이 §7.3 고정 초기값·궤적으로 이동 |
| ZW-F2 | F1 | X-bot 경계 도달 | cross-owner bot relocation 완주(binding 없음) |
| ZW-F3 | F1 | bot 이동 거부 유도 | 방향 반전 |
| ZW-F4 | F1 | client push 관찰 | bot 대상 push 부재(음성 증거) |
| ZW-G1 | 기본 | RID 관측(probe) | `zn-<lowercase-uuid-v4>` 형식 실검사(문자열 marker 출력만으로는 불충분), 노드 간 상이 |
| ZW-G2 | 기본 | 시작 순서 변형 실행 | readiness·routing 정상 |
| ZW-G3 | 기본 | 정상 교체(stop→start) | 새 RID·같은 NodeId report, 새 object 정상 |
| ZW-G4 | 기본 | crash 교체(kill→start) | §7.5 의미의 교체: 이전 operation Unavailable 경계 유지 + 새 process 정상 |
| ZW-G5 | G3/G4 | routing ID gate | §9.3 전 항목 |

언어별 runner가 일부 ID를 runner-driven으로 구현할 수는 있으나(예: C4 fault 주입), ID의
전제·행동·단언 의미는 바꾸지 않는다. 이 표에 없는 ID를 새로 만들 때는 이 문서에 먼저
추가한다.

#### ZoneNode를 멈추고 다시 띄우는 시나리오의 고정값

여러 시나리오가 ZoneNode를 멈춘다. **어떤 방식으로 멈추는지와 다시 띄울 때 어떤 zone 집합을
갖는지는 언어별 재량이 아니다.** 이 값이 비어 있어서 네 구현이 서로 다른 신호를 썼고, 같은 ID가
언어마다 다른 것을 시험했다.

| ID | 멈추는 방식 | 근거 |
| --- | --- | --- |
| ZW-B4 | 급정지(abrupt) | publish가 즉시 끊겨야 3 tick expiry를 관측한다 |
| ZW-C2 | 정상 종료(graceful) | 표의 전제가 "정상 종료"다. drain 경로의 disconnect event를 본다 |
| ZW-C3 | 급정지(abrupt) | §2.2 — "crash된 node는 false report를 보낼 수 없으므로 이 TTL이 유일한 false 전환 규칙이다" |
| ZW-E5 | 급정지(abrupt) | maintenance desired state가 process 밖 store에 있음을 보인다 |
| ZW-G3 | 정상 종료(graceful) | 표의 전제가 "정상 교체(stop→start)"다 |
| ZW-G4 | 급정지(abrupt) | 표의 전제가 "crash 교체(kill→start)"다 |

**다시 띄운 ZoneNode는 zone을 되찾지 않는다.** §2.2가 "Ready owner 장애는 자동 replacement가
아니다"라고 정하므로, 재기동한 process는 **zone 0개로 ready가 되는 replacement 구성**으로
띄운다. 멈춘 방식이 정상 종료든 급정지든 같다. zone을 claim하는 것은 최초 cold start뿐이다.

이 값을 비워 두면 재기동한 node가 zone 2개를 요구하며 claim을 반복하다 예산을 소진한다 —
실제로 cpp 구현이 그 상태였다.

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=Math.max(d.body?d.body.scrollHeight:0,d.documentElement?d.documentElement.scrollHeight:0);if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
