<!-- framework-adapter-nav:start -->
[E2E 목차](README.ko.md) | [이전: Location messaging](config-1-location-messaging.ko.md) | [다음: Pub/Sub](config-3-pubsub.ko.md)
<!-- framework-adapter-nav:end -->

# Config 2 — Spot, Actor와 Session을 함께 사용하는 서비스

이 config는 상태를 가진 User Spot, 그 Spot에 존재하는 Actor와 Actor에 bind된 Stream Session을 실제 여러
process에 배치한다. Application은 global SpotId와 ActorId를 사용하며 owner RID와 endpoint를 계산하지 않는다.
Framework는 public object manager, direct messaging, binding과 Stream API로 request·send·push를 연결한다.

E2E client는 역할 server의 application endpoint와 Stream endpoint만 호출한다. Location Store row, private
mailbox, raw frame과 internal creation barrier는 사용하지 않는다. Factory·handler·lifecycle callback이 필요한
evidence는 Application state로 기록한다.

## 1. 확인 범위

- Entry·User Spot 생성, 상태, timer와 global Spot routing
- Actor create, local·remote Join, lifecycle과 direct messaging
- Channel·Spot·Logical Multicast 방향별 message 흐름
- Session bind·rebind, relay, push, disconnect와 Stream lifecycle
- 같은 MeshNode transport에서 Channel·Node·Spot route 분리
- Play node crash, scale-out, lifecycle 경합과 placement weight

## 2. 배포 구성

| 역할 | 수 | 하는 일과 분리 이유 |
|---|---:|---|
| Location Store | 1 | Global Spot·Actor current location과 automatic topology를 제공한다. |
| Relocation Store | 1 | Cross-node Actor Join의 `PreserveStateWith` payload를 보존한다. |
| Play node | 2 | Object Server다. Entry Spot, `SpotWide`·`PerActor` User Spot과 Actor factory·handler를 제공한다. |
| Session gateway | 2 | Object Client다. Stream Session, Actor binding과 relay를 제공하며 Actor·Spot factory는 제공하지 않는다. |
| E2E client | scenario별 | 역할 server의 public endpoint와 Stream connector를 사용한다. |

Runner는 scenario마다 object IDs, Session과 evidence marker를 새로 만든다. 역할 health, public topology
status와 object readiness를 확인한 뒤 operation을 시작한다. 순서 제어가 필요하면 Application factory나
handler가 public signal에서 대기한다. Fixed sleep과 handler 선택 확률로 상태를 추정하지 않는다.

## 3. Scenario

### Track A — Spot을 만들고 global SpotId로 사용

#### SM-A1 Entry Spot request로 User Spot을 만든다

우선순위: `P0`

Entry Spot은 Host가 시작할 때 준비되는 application 진입점이다. Join request가 User Spot을 만들면 caller는
생성된 global SpotId를 받아 이후 direct messaging에 사용한다.

**검증 질문:** Entry Spot request가 Ready User Spot의 ID를 반환하고 그 Spot이 request를 처리하는가.

- 시작 조건: Play node의 public startup evidence에서 Entry Spot ID를 얻고 Entry Spot이 ready다.
- 절차: Caller가 Entry Spot에 Join request를 보내고 reply의 SpotId로 state request를 한 번 보낸다.
- 검증: Reply의 SpotId를 public manager `Find`가 Ready ref로 반환하고 state handler가 한 번 실행된다.
- 세부 동작: [Spot actor §2](../spec/15-spot-actor.ko.md)을 검증한다.

#### SM-A2 User Spot state를 serial하게 변경한다

우선순위: `P0`

같은 Spot의 callback은 하나의 execution lane에서 처리되어 shared state 변경 순서를 보존한다.

**검증 질문:** Counter increment requests N개가 reply 순서와 관계없이 최종 state N으로 수렴하는가.

- 시작 조건: Counter 0인 User Spot이 ready다.
- 절차: 고유 operation ID의 increment requests N개를 bounded concurrency로 보낸다.
- 검증: 모든 requests가 reply 하나를 받고 final state는 N이다. Handler active count는 1을 넘지 않는다.
- 세부 동작: [Spot messaging §7](../spec/12-spot-messaging.ko.md)을 검증한다.

#### SM-A3 Global SpotId가 정확한 Spot에 도달한다

우선순위: `P1`

같은 stable type의 Spot이 여러 node에 있어도 direct request는 지정한 global SpotId의 current owner만
처리해야 한다.

**검증 질문:** Spot A ID의 request가 A handler에만 기록되는가.

- 시작 조건: 서로 다른 nodes에 Spot A와 B가 ready다.
- 절차: Spot A ID로 marker request를 한 번 보낸다.
- 검증: A evidence에 marker가 한 번 있고 B evidence에는 없다.
- 세부 동작: [Spot messaging §3](../spec/12-spot-messaging.ko.md)을 검증한다.

#### SM-A4 Owner를 입력하지 않고 current Spot을 호출한다

우선순위: `P0`

Application은 domain key에서 SpotId만 정하고 current owner는 Framework가 찾는다.

**검증 질문:** 같은 SpotId request가 owner relocation 전후의 current node에서 처리되는가.

- 시작 조건: Spot이 play-a에 ready이고 caller endpoint는 SpotId만 입력받는다.
- 절차: Request를 한 번 보내고 Host Relocate로 Spot owner를 play-b로 바꾼 뒤 같은 ID로 다시 요청한다.
- 검증: 첫 marker는 A, 두 번째 marker는 B가 처리한다. Caller 입력에는 MeshName, RID와 endpoint가 없다.
- 세부 동작: [Spot messaging §3](../spec/12-spot-messaging.ko.md)을 검증한다.

#### SM-A5 Application Stage wrapper가 Spot 계약을 바꾸지 않는다

우선순위: `P2`

Stage는 Application이 Spot·Actor·timer API를 묶어 쓰는 wrapper이며 별도 scheduler나 routing layer가 아니다.

**검증 질문:** SpotWide와 PerActor Stage variants가 각 execution mode의 public ordering을 유지하는가.

- 시작 조건: 같은 domain behavior를 두 User Spot execution mode로 구성한다.
- 절차: Spot request, member Actor request와 timer를 application wrapper를 통해 실행한다.
- 검증: SpotWide는 shared gate 순서를, PerActor는 Actor별·timer별 lane 순서를 유지한다. Public replies와
  state는 wrapper를 쓰지 않은 대조 flow와 같다.
- 세부 동작: [비동기 실행 정책](../spec/05-async-execution-policy.ko.md)을 검증한다.

#### SM-A6 User Spot initialize와 close lifecycle을 실행한다

우선순위: `P1`

Member Actor가 있는 User Spot은 임의로 닫지 않으며, member가 모두 떠난 뒤 exact ref close가
`OnClosing(ExplicitClose)`를 한 번 호출한다.

**검증 질문:** Actor가 있을 때 close는 false이고 leave 뒤 close만 성공하는가.

- 시작 조건: Initialize callback이 한 번 끝난 User Spot에 Actor가 join되어 있다.
- 절차: Current SpotRef로 close하고, Actor를 leave한 뒤 같은 current ref로 다시 close한다.
- 검증: First close는 false이고 callback·membership이 유지된다. Second는 true이며 closing callback이 한
  번 실행된다.
- 세부 동작: [Spot actor §7](../spec/15-spot-actor.ko.md)을 검증한다.

#### SM-A7 같은 SpotId의 stable type 충돌을 거부한다

우선순위: `P1`

Global SpotId 하나는 current incarnation에서 한 object kind와 stable type만 가질 수 있다.

**검증 질문:** Type A SpotId를 Type B로 GetOrCreate하면 `TypeMismatch`인가.

- 시작 조건: Stable type A의 Spot이 ready이고 state marker를 가진다.
- 절차: 같은 ID에 stable type B의 `GetOrCreate`를 호출한다.
- 검증: Call은 `TypeMismatch`이고 original Spot state와 handler availability가 유지된다.
- 세부 동작: [Spot actor §2](../spec/15-spot-actor.ko.md)을 검증한다.

#### SM-A8 CPU worker 결과를 Spot state에 반영한다

우선순위: `P2`

CPU 계산은 bounded worker pool에서 실행하고 continuation은 Spot execution context로 돌아와 state를 바꾼다.

**검증 질문:** CPU worker Yield 중 probe가 진행되고 계산 결과가 이후 Spot state에 한 번 반영되는가.

- 시작 조건: Worker completion을 application signal로 보류할 수 있는 Spot이 ready다.
- 절차: CPU worker call을 Yield로 기다리고 같은 Spot에 probe request를 보낸 뒤 worker를 해제한다.
- 검증: Probe가 continuation 전에 완료되고 final state는 worker result를 정확히 한 번 반영한다.
- 세부 동작: [비동기 실행 정책 §6](../spec/05-async-execution-policy.ko.md)을 검증한다.

#### SM-A9 User Spot은 initialize 완료 뒤 Ready로 공개한다

우선순위: `P0`

Factory가 instance를 만들고 initialize하는 중에는 remote caller가 incomplete Spot을 existing object로 사용하면
안 된다.

**검증 질문:** Initialize-held 중 Find·request가 Spot을 사용하지 못하고 release 뒤 성공하는가.

- 시작 조건: User Spot factory initialize가 application signal에서 대기한다.
- 절차: Create를 시작하여 initialize-held를 확인하고 다른 process에서 Find와 request를 시도한다. Gate를
  해제한 뒤 다시 호출한다.
- 검증: Held 구간에 Find는 Ready ref를 반환하지 않고 handler evidence가 없다. Create success 뒤 Find와
  request가 같은 current ref로 성공한다.
- 세부 동작: [Spot actor §2](../spec/15-spot-actor.ko.md)의 publication boundary를
  검증한다.

#### SM-A10 Entry Spot ID는 MeshNode RID와 독립된 lifecycle identity다

우선순위: `P0`

Entry Spot ID를 Node RID에서 문자열 조합으로 계산하면 restart와 identity 충돌을 안전하게 구분할 수 없다.

**검증 질문:** 같은 Host lifecycle에서는 Entry Spot ID가 유지되고 replacement에서는 RID와 별도로
바뀌는가.

- 시작 조건: Diagnostic prefix `play`의 Object Server가 ready다.
- 절차: Public startup evidence에서 Node RID와 Entry Spot ID를 읽고 정상 request를 보낸다. Host를
  replacement lifecycle로 재시작해 두 IDs를 다시 읽는다.
- 검증: 두 IDs는 서로 다른 valid identities이고 same lifecycle에서 안정적이다. Replacement에서는 old
  Entry ID가 current로 남지 않고 새 Entry request가 성공한다.
- 세부 동작: [Network listener identity §7.3](../spec/10-network-listener-identity.ko.md)를
  검증한다.

#### SM-A11 Entry Spot 예약 형식을 User·Instance ID로 거부한다

우선순위: `P0`

Framework가 발급하는 Entry Spot namespace를 Application object ID로 사용하면 current Entry identity와
충돌한다.

**검증 질문:** Reserved Entry-style ID의 User create와 Instance intent가 `InvalidOperation`인가.

- 시작 조건: 유효한 reserved-format string을 준비한다.
- 절차: 같은 ID로 User Spot GetOrCreate와 Instance Spot request를 각각 시도한다.
- 검증: 두 calls는 `InvalidOperation`이고 factory callback과 application handler evidence가 없다.
- 세부 동작: [Glossary의 Entry Spot](../spec/01-glossary.ko.md)을 검증한다.

#### SM-A12 Automatic User Spot IDs가 concurrent creates에서 서로 다르다

우선순위: `P0`

Automatic create는 Application이 ID를 제공하지 않을 때 global unique Spot identity를 반환해야 한다. 내부
UUID generator 충돌 주입은 public E2E가 아니라 contract test 책임이다.

**검증 질문:** Concurrent automatic creates가 모두 다른 SpotIds와 독립 state를 반환하는가.

- 시작 조건: 같은 stable type factory와 충분한 capacity가 ready다.
- 절차: 서로 다른 callers가 automatic Create 200개를 동시에 실행한다.
- 검증: Successful refs의 SpotIds는 모두 다르고 각 ID의 marker request가 자기 Spot에서 한 번 처리된다.
- 세부 동작: [Spot actor §2](../spec/15-spot-actor.ko.md)을 검증한다.

#### SM-A13 SpotId UTF-8 길이와 exact equality를 지킨다

우선순위: `P0`

SpotId는 1~255 UTF-8 bytes의 case-sensitive exact string이다. Public E2E는 invalid raw binary frame을 만들지
않고 public string boundary만 검증한다.

**검증 질문:** 1·255-byte IDs는 성공하고 256-byte ID는 side effect 없이 거부되는가.

- 시작 조건: Byte length가 정확한 public strings와 `Room/room`, NFC/NFD variants를 준비한다.
- 절차: 각 valid ID를 create·find·request하고 256-byte ID create를 시도한다.
- 검증: Valid IDs는 exact values로 서로 다른 objects를 가리킨다. 256-byte call은 local validation error이고
  factory evidence가 없다.
- 세부 동작: [Actor model §2.1](../spec/14-actor-model.ko.md)의 동일 global ID
  규칙과 Spot ID 계약을 검증한다.

### Track B — Actor 생성과 Spot membership 변경

#### SM-B0 Explicit type create와 existing-only Find를 구분한다

우선순위: `P0`

Find는 Missing Actor를 만들지 않고 Create·GetOrCreate만 factory를 실행한다.

**검증 질문:** Missing Find는 empty이고 concurrent create calls는 current Actor 하나로 수렴하는가.

- 시작 조건: 두 Play nodes가 같은 stable Actor type과 capacity를 제공한다.
- 절차: Missing ID를 Find한 뒤 같은 ID·type의 Create와 GetOrCreate를 동시에 호출하고 다시 Find한다.
- 검증: First Find는 empty이며 factory evidence가 없다. Creation results는 current Actor 하나를 가리키고
  final Find가 같은 generation ref를 반환한다.
- 세부 동작: [Actor model §3](../spec/14-actor-model.ko.md)을 검증한다.

#### SM-B0A Actor creation accept와 reject를 operation별로 반환한다

우선순위: `P0`

Creation callback이 reject한 operation의 reply를 다음 caller와 공유해서는 안 된다.

**검증 질문:** First rejected call과 second accepted call이 각자의 request·terminal을 받는가.

- 시작 조건: Creation callback이 first marker는 reject하고 second marker는 accept하도록 구성한다.
- 절차: 같은 ActorId의 two GetOrCreate calls를 순서가 제어된 concurrent flow로 실행하고 final Find를
  호출한다.
- 검증: First는 typed Rejected와 자기 payload를, second는 Created와 current ActorRef를 받는다. Final Find는
  accepted Actor만 반환하고 rejected operation의 handler·destroy evidence는 없다.
- 세부 동작: [Actor model §3](../spec/14-actor-model.ko.md)을 검증한다.

#### SM-B1 같은 node의 User Spot으로 Join한다

우선순위: `P0`

Same-node Join은 Actor state relocation 없이 membership callbacks로 current Spot을 바꾼다.

**검증 질문:** Local Join 뒤 Actor current Spot과 후속 handler가 target User Spot을 가리키는가.

- 시작 조건: Actor는 play-a Entry Spot, target User Spot도 play-a에 ready다.
- 절차: Actor handler에서 target SpotId로 Join을 시작하고 completion을 기다린 뒤 request를 보낸다.
- 검증: Target `OnActorJoin`, `OnJoinedActor`, source `OnLeaveActor`가 한 번씩 실행되고 current Spot은 target이다.
  Follow-up Actor request도 play-a에서 한 번 처리된다.
- 세부 동작: [Spot actor §4](../spec/15-spot-actor.ko.md)를 검증한다.

#### SM-B2 다른 node의 User Spot으로 Join한다

우선순위: `P0`

Cross-node Join은 같은 ActorId·ObjectGeneration과 application state를 target Actor instance에 복원한다.

**검증 질문:** Remote Join 뒤 state와 generation을 유지한 Actor가 target에서 request를 처리하는가.

- 시작 조건: Actor는 play-a Entry Spot, target User Spot은 play-b에 있다.
- 절차: Counter state를 변경한 뒤 target SpotId로 Join하고 completion 후 current ref와 state를 조회한다.
- 검증: Join is Accepted, current location은 play-b, generation과 counter는 이전과 같다. Public lifecycle·adapter
  callbacks가 정식 순서로 한 번씩 실행되고 follow-up request는 target에서 처리된다.
- 세부 동작: [Spot actor §5](../spec/15-spot-actor.ko.md)을 검증한다.

#### SM-B3 Typed Actor request payload를 보존한다

우선순위: `P0`

Nested object, collection과 nullable field가 있는 typed payload는 process 경계를 지나도 application 값이
같아야 한다.

**검증 질문:** Complex request와 reply의 모든 application fields가 입력과 일치하는가.

- 시작 조건: Actor handler가 받은 DTO를 reply와 evidence에 그대로 반영한다.
- 절차: Nested object, ordered tags와 nullable values가 있는 request를 remote Actor에 보낸다.
- 검증: Handler evidence와 reply의 field values·collection order가 입력과 같다.
- 세부 동작: [Message model](../spec/04-message-model.ko.md)을 검증한다.

#### SM-B4 Remote Actor request를 current owner로 보낸다

우선순위: `P1`

Caller와 Actor owner가 다른 process여도 global ActorId request는 target mailbox와 reply route를 연결한다.

**검증 질문:** Remote Actor가 request를 한 번 처리하고 caller가 reply를 받는가.

- 시작 조건: Actor는 play-b, caller server는 play-a에 ready다.
- 절차: Caller가 ActorId만 사용하여 request를 한 번 보낸다.
- 검증: Play-b handler만 marker를 한 번 기록하고 caller가 matching reply를 받는다.
- 세부 동작: [Actor model §5](../spec/14-actor-model.ko.md)을 검증한다.

#### SM-B5 Handler 없는 Actor request를 관찰한다

우선순위: `P0`

Actor는 존재하지만 packet handler가 없으면 target missing과 다른 dispatch failure다.

**검증 질문:** Missing handler request가 public error와 `no_handler/reply_error` flow evidence를 남기는가.

- 시작 조건: Actor는 ready이고 public message-flow observer가 등록되어 있다.
- 절차: 등록하지 않은 packet name의 request를 보낸 뒤 normal request를 보낸다.
- 검증: First는 정식 error terminal이고 observer evidence가 한 번 있다. Normal request는 성공한다.
- 세부 동작: [Message flow tracing §2.2](../spec/26-message-flow-tracing.ko.md)을
  검증한다.

#### SM-B6 Explicit leave와 Session disconnect callback을 구분한다

우선순위: `P0`

Spot membership leave와 physical Session disconnect는 다른 lifecycle 사건이다.

**검증 질문:** Leave는 leave callback만, disconnect는 disconnect callback만 실행하는가.

- 시작 조건: Fresh Actors를 각각 User Spot과 Session에 배치·bind한다.
- 절차: Variant A는 public leave를 호출하고 B는 Stream connection을 비정상 종료한다.
- 검증: A는 `OnLeaveActor`만 한 번 실행하고 membership이 바뀐다. B는 `OnDisconnectActor`만 current binding에
  한 번 실행하며 Actor와 Spot membership은 유지된다.
- 세부 동작: [Session Actor dispatch §6](../spec/20-session-actor-dispatch.ko.md)를
  검증한다.

#### SM-B7 Membership callback 뒤 Actor packet을 dispatch한다

우선순위: `P1`

Actor가 Ready이거나 Join commit이 끝나기 전에 packet handler가 시작하면 incomplete state를 관찰할 수 있다.

**검증 질문:** Join completion 뒤에 보낸 packets가 target Actor에서 FIFO로 처리되는가.

- 시작 조건: Actor와 target Spot이 ready다.
- 절차: Join completion callback 뒤 sequence 1~20 requests를 보낸다.
- 검증: Public callback evidence가 Join terminal 전에 끝나고 target handler sequence는 1~20이며 active
  count는 1이다.
- 세부 동작: [Spot actor §4](../spec/15-spot-actor.ko.md)를 검증한다.

#### SM-B8 Exact ActorRef로 current incarnation을 destroy한다

우선순위: `P1`

Destroy는 exact ActorRef의 incarnation만 종료한다.

**검증 질문:** Current ref destroy는 true, 반복은 false, recreate 뒤 old ref는 `InvalidOperation`인가.

- 시작 조건: Current ActorRef를 저장한다.
- 절차: Same ref로 destroy를 두 번 호출하고 같은 ActorId를 recreate한 뒤 old ref로 다시 destroy한다.
- 검증: Results는 true, false, `InvalidOperation` 순서이며 recreated Actor는 request를 처리한다.
- 세부 동작: [Failover policy §4.1](../spec/31-failure-failover-policy.ko.md)을
  검증한다.

#### SM-B9 Target Spot의 Join accept와 reject를 구분한다

우선순위: `P1`

Target `OnActorJoin`은 existing Actor의 membership proposal을 승인하거나 거절한다. Reject는 source를
변경하지 않는다.

**검증 질문:** Accept는 target membership으로 바꾸고 reject는 source membership을 유지하는가.

- 시작 조건: Accept target과 reject target을 별도 User Spots로 준비한다.
- 절차: Fresh Actors로 local·remote accept와 reject variants를 실행한다.
- 검증: Accept는 completion Accepted와 target current Spot을 반환한다. Reject는 typed Rejected이며 target
  joined·source leave callbacks가 없고 source follow-up request가 성공한다.
- 세부 동작: [Spot actor §4](../spec/15-spot-actor.ko.md)를 검증한다.

#### SM-B10 Object role과 Location Store prerequisite를 검증한다

우선순위: `P0`

Object Client·Server와 Actor dispatch에는 Location Store가 필요하다. Role None manual Host는 Node·Channel만
제공한다.

**검증 질문:** Missing Store object hosts는 startup 실패하고 role None manual Channel은 동작하는가.

- 시작 조건: Object role without Store, Actor dispatch without Store와 role None manual configurations를
  각각 만든다.
- 절차: Negative hosts와 manual host를 시작하고 manual Node·Channel request를 보낸다.
- 검증: Negative hosts는 listener ready 전에 configuration error다. Manual request는 성공하고 object
  managers·factory operation은 제공되지 않는다.
- 세부 동작: [MeshNode §4](../spec/13-mesh-node.ko.md)을 검증한다.

#### SM-B11 Actor는 initial membership 완료 뒤 Ready로 공개한다

우선순위: `P0`

Factory와 initial Entry membership 중인 Actor를 remote caller가 existing Actor로 사용해서는 안 된다.

**검증 질문:** Factory-held 중 Find·request는 Actor를 사용하지 못하고 release 뒤 성공하는가.

- 시작 조건: Actor factory가 application signal에서 대기한다.
- 절차: Create를 시작해 factory-held를 확인하고 다른 process에서 Find와 request를 시도한다. Gate를 해제한
  뒤 다시 호출한다.
- 검증: Held 구간에는 Ready ref와 handler evidence가 없다. Create completion 뒤 Find와 request가 current
  Actor로 성공한다.
- 세부 동작: [Actor model §3](../spec/14-actor-model.ko.md)을 검증한다.

### Track C — Channel과 Spot 사이 message 방향을 확인

#### SM-C1 Channel handler에서 Spot request를 보낸다

우선순위: `P0`

Channel request를 처리하는 역할 server는 global SpotId로 stateful Spot을 호출하고 그 reply를 원래 Channel
reply에 포함할 수 있다.

**검증 질문:** Channel caller가 최종 Spot state가 포함된 reply를 한 번 받는가.

- 시작 조건: Channel handler와 target Spot이 ready다.
- 절차: Caller가 operation ID를 Channel request로 보내고 handler가 같은 ID로 Spot request를 실행한다.
- 검증: Spot handler가 한 번 실행되고 caller는 Spot 결과가 포함된 reply 하나를 받는다.
- 세부 동작: [Spot messaging §3](../spec/12-spot-messaging.ko.md)을 검증한다.

#### SM-C2 Spot handler에서 Channel request를 보낸다

우선순위: `P0`

Spot은 자기 callback에서 ChannelName request를 기다리고 결과를 Spot state에 반영할 수 있다.

**검증 질문:** Downstream Channel reply가 원래 Spot request와 state에 한 번 반영되는가.

- 시작 조건: Spot과 remote Channel handler가 ready다.
- 절차: Spot request가 Channel request를 Async 또는 allowed Yield로 기다린다.
- 검증: Channel handler와 Spot handler가 operation ID를 한 번씩 기록하고 final reply·state가 downstream
  result와 일치한다.
- 세부 동작: [Channel messaging §3.2](../spec/08-channel-messaging.ko.md)을
  검증한다.

#### SM-C3 Spot에서 다른 Spot으로 request를 보낸다

우선순위: `P1`

Source Spot은 target SpotId만 사용하여 remote stateful service를 호출할 수 있다.

**검증 질문:** Source Spot request가 target Spot reply를 받아 자기 state에 반영하는가.

- 시작 조건: Source와 target User Spots가 서로 다른 nodes에 ready다.
- 절차: Source handler가 target SpotId로 request를 한 번 보낸다.
- 검증: Target marker와 source final state가 matching operation ID를 가지며 caller reply가 한 번 도착한다.
- 세부 동작: [Spot messaging §3](../spec/12-spot-messaging.ko.md)을 검증한다.

#### SM-C4 Local Spot이 없는 MeshNode가 Logical Multicast를 publish한다

우선순위: `P1`

Logical Multicast origin은 local Spot을 호스팅할 필요가 없다. Object Client node도 ChannelName과 topic으로
remote subscription에 publish할 수 있다.

**검증 질문:** Spot 없는 origin의 marker를 matching remote Spots만 받는가.

- 시작 조건: Origin node에는 local Spot이 없고 two remote nodes에 matching·nonmatching subscriptions가 있다.
- 절차: Origin application endpoint가 Logical Multicast를 한 번 publish한다.
- 검증: Matching Spots가 marker를 한 번씩 받고 nonmatching Spot은 받지 않는다.
- 세부 동작: [Spot messaging §4](../spec/12-spot-messaging.ko.md)을
  검증한다.

#### SM-C5 Logical Multicast remote delivery를 subscriber evidence로 판정한다

우선순위: `P0`

Publish terminal은 remote 수신 확인이 아니다. E2E는 target handler evidence를 별도로 확인해야 한다.

**검증 질문:** Positive-weight remote nodes의 subscribed Spots가 같은 marker를 한 번씩 받는가.

- 시작 조건: Remote node weights 1, 10000과 0 variants가 있고 positive nodes에 matching Spots가 있다.
- 절차: Source Spot이 unique marker를 publish한다.
- 검증: Positive nodes의 Spots가 한 번씩 받고 weight 0 node는 신규 target에서 제외된다. Publish terminal만으로
  통과하지 않는다.
- 세부 동작: [Spot messaging §4](../spec/12-spot-messaging.ko.md)을
  검증한다.

#### SM-C6 Logical Multicast partial backpressure를 다른 target과 격리한다

우선순위: `P0`

한 target이 message를 수락하지 못해도 이미 수락 가능한 target delivery를 rollback하거나 같은 publish를
자동 재실행하지 않는다.

**검증 질문:** Blocked target과 ready target이 함께 있을 때 ready target만 marker를 한 번 처리하는가.

- 시작 조건: One remote target은 handler gate와 public HWM보다 큰 deterministic payload를 준비한다. 먼저
  blocker payload를 보내 handler 진입과 Application receive paused 상태를 확인한다. 다른 target은 ready다.
- 절차: Marker를 한 번 publish한다.
- 검증: Public terminal은 target별 result 없이 정식 의미로 끝나고 ready target은 marker를 한 번 처리한다.
  Private snapshot·attempt count는 읽지 않는다.
- 세부 동작: [Spot messaging §4](../spec/12-spot-messaging.ko.md)을
  검증한다.

### Track D — Session binding, relay와 Stream lifecycle을 확인

#### SM-D1 Local Actor를 Session에 bind하고 relay한다

우선순위: `P0`

Session gateway와 Actor owner route가 준비되면 client request는 bound Actor로 relay되고 Actor push는 같은
client로 돌아온다.

**검증 질문:** Local-owner Actor request reply와 push가 bound Stream client에 도착하는가.

- 시작 조건: Session과 Actor가 ready이고 exact ActorRef를 bind했다.
- 절차: Client가 Actor ID metadata로 request를 보내고 Actor가 push를 한 번 보낸다.
- 검증: Actor handler가 request를 한 번 처리하고 client는 matching reply와 push를 한 번씩 받는다.
- 세부 동작: [Session Actor dispatch §5](../spec/20-session-actor-dispatch.ko.md)를
  검증한다.

#### SM-D2 Remote Actor를 Session에 bind하고 relay한다

우선순위: `P0`

Actor owner가 gateway와 다른 process여도 binding route가 request와 push를 연결한다.

**검증 질문:** Remote Actor relay와 push가 gateway를 거쳐 같은 client에 도착하는가.

- 시작 조건: Actor는 play-b, Session은 session-a에 있고 exact ref bind가 완료됐다.
- 절차: SM-D1 request와 push를 반복한다.
- 검증: Play-b handler가 request를 처리하고 client가 reply·push를 받는다. Caller는 RID와 endpoint를
  제공하지 않는다.
- 세부 동작: [Session Actor dispatch §5](../spec/20-session-actor-dispatch.ko.md)를
  검증한다.

#### SM-D3 Entry·User Spot Actor binding 의미가 같다

우선순위: `P1`

Session binding은 Actor의 current Spot kind와 독립적이다.

**검증 질문:** Entry Actor와 User Spot Actor가 같은 bind·relay·push 결과를 만드는가.

- 시작 조건: Fresh Actors를 Entry와 User Spot에 각각 준비한다.
- 절차: Separate Sessions에 bind하고 request·push를 한 번씩 실행한다.
- 검증: 두 variants 모두 matching reply·push를 한 번씩 제공하고 membership은 바뀌지 않는다.
- 세부 동작: [Actor model §2.3](../spec/14-actor-model.ko.md)을
  검증한다.

#### SM-D4 한 Session에 여러 Actors를 bind한다

우선순위: `P0`

Session 하나는 여러 Actor bindings를 가질 수 있고 Application은 inbound metadata로 target binding을
선택한다.

**검증 질문:** Actor ID가 있는 packets와 pushes가 지정한 Actor에만 전달되는가.

- 시작 조건: Actor X와 Y를 같은 Session에 bind한다.
- 절차: X·Y metadata requests와 Actor별 pushes를 보낸다. Missing metadata request도 한 번 보낸다.
- 검증: 각 Actor가 자기 marker만 처리하고 client가 구분된 replies·pushes를 받는다. Missing target은 public
  dispatch error이며 어느 Actor도 처리하지 않는다.
- 세부 동작: [Session Actor dispatch §3](../spec/20-session-actor-dispatch.ko.md)를
  검증한다.

#### SM-D4A Rebind 뒤 stale Session을 격리한다

우선순위: `P0`

Actor를 Session B에 rebind하면 Session A의 old binding identity는 더 이상 current가 아니다.

**검증 질문:** Session A의 late relay·disconnect가 Session B binding과 Actor state를 바꾸지 않는가.

- 시작 조건: Actor X를 A에 bind한 뒤 B에 explicit rebind한다.
- 절차: A network gate에 보류한 relay와 disconnect를 B bind 완료 뒤 전달하고 B에서 normal relay·push를
  실행한다.
- 검증: Old operations는 stale result이고 handler evidence가 없다. B relay·push가 한 번씩 성공하며 current
  binding은 B다.
- 세부 동작: [Session Actor dispatch §4](../spec/20-session-actor-dispatch.ko.md)를
  검증한다.

#### SM-D4B Relocation 뒤 stored binding route의 Message Follow를 사용한다

우선순위: `P0`

Session binding은 validated route를 저장한다. Actor relocation 뒤 active Message Follow가 있으면 old route의
relay를 current owner로 한 번 전달하고, mapping이 없으면 `Unavailable`이다.

**검증 질문:** Active follow route variant는 성공하고 expired variant는 `Unavailable`인가.

- 시작 조건: Actor를 bind한 뒤 remote owner로 relocate한다.
- 절차: Active follow window에서 relay를 보내고, fresh fixture에서 window expiry 뒤 relay를 보낸다.
- 검증: Active marker는 target Actor에서 한 번 처리된다. Expired request는 `Unavailable`이고 handler
  evidence가 없다. Application이 rebind하지 않은 same-incarnation relocation만 대상이다.
- 세부 동작: [Session Actor dispatch §5](../spec/20-session-actor-dispatch.ko.md)를
  검증한다.

#### SM-D5 Physical disconnect를 current bindings 전체에 통지한다

우선순위: `P0`

Stream connection이 끊기면 Framework가 current binding snapshot의 각 Actor에 disconnect callback을 자동
제출한다.

**검증 질문:** Multiple bound Actors가 disconnect callback을 최대 한 번씩 받고 membership을 유지하는가.

- 시작 조건: Local·remote Actors 여러 개가 한 Session에 bind되어 있다.
- 절차: Stream connection을 비정상 종료한다. 한 Actor callback은 application error를 반환한다.
- 검증: 모든 current Actors의 callbacks가 각각 한 번 시도되고 한 failure가 나머지를 막지 않는다. Public
  current Spot과 ObjectGeneration은 유지된다.
- 세부 동작: [Session Actor dispatch §6](../spec/20-session-actor-dispatch.ko.md)를
  검증한다.

#### SM-D5A 선택한 Actor에 logical disconnect를 통지한다

우선순위: `P0`

Application logical disconnect는 physical connection 전체가 아니라 선택한 current binding 하나에만
적용한다.

**검증 질문:** 선택 Actor callback만 실행되고 다른 bindings와 connection은 유지되는가.

- 시작 조건: Actor X와 Y가 같은 active Session에 bind되어 있다.
- 절차: Public logical disconnect operation을 X에 호출하고 Y relay를 보낸다.
- 검증: X callback만 한 번 실행되고 Y relay는 성공한다. Connection과 두 Actors의 membership은 유지된다.
- 세부 동작: [Session Actor dispatch §6](../spec/20-session-actor-dispatch.ko.md)를
  검증한다.

#### SM-D6 Push는 current bound Session만 받는다

우선순위: `P0`

Actor push는 current binding 하나를 target으로 하며 unbound clients에 broadcast하지 않는다.

**검증 질문:** Bound client만 state-change push를 받는가.

- 시작 조건: Client A가 Actor에 bind되어 있고 B는 연결만 되어 있다.
- 절차: Backend request로 Actor state를 바꾸어 push를 발생시킨다.
- 검증: A가 marker를 한 번 받고 B는 받지 않는다.
- 세부 동작: [Session Actor dispatch §5](../spec/20-session-actor-dispatch.ko.md)를
  검증한다.

#### SM-D7 Stream auth 뒤 packet dispatch를 허용한다

우선순위: `P0`

Unauthenticated connection은 업무 packet을 dispatch하지 않고 auth 성공 뒤에만 Session handler를 실행한다.

**검증 질문:** Valid auth 뒤 request가 성공하고 invalid auth connection은 정식 close/error인가.

- 시작 조건: Valid와 invalid credentials가 준비되어 있다.
- 절차: Separate connectors로 auth하고 같은 packet을 보낸다.
- 검증: Valid connector만 reply를 받고 handler evidence가 있다. Invalid connector는 public auth error 또는
  close reason을 받고 handler가 실행되지 않는다.
- 세부 동작: [Stream session §3](../spec/19-stream-session.ko.md)을 검증한다.

#### SM-D8 Stream reconnect는 새 auth·bind를 요구한다

우선순위: `P1`

Reconnect는 새 physical Session이므로 이전 pending request와 binding을 자동 복원하지 않는다.

**검증 질문:** Disconnect pending은 실패하고 reconnect 뒤 explicit auth·rebind 후 새 request가 성공하는가.

- 시작 조건: Authenticated bound Session과 slow pending request가 있다.
- 절차: Connection을 끊고 pending terminal을 확인한다. Reconnect하여 auth·rebind하고 새 request를 보낸다.
- 검증: Old request는 disconnected failure이며 replay되지 않는다. New request만 Actor에서 한 번 처리된다.
- 세부 동작: [Failover policy §6](../spec/31-failure-failover-policy.ko.md)을 검증한다.

#### SM-D9 Public inbound observer가 Stream packet을 기록한다

우선순위: `P1`

Inbound observability는 packet kind·name·sequence를 정식 field로 제공하며 payload를 성공 조건으로 복제하지
않는다.

**검증 질문:** Inbound observer evidence와 handler evidence가 같은 packet identity를 가지는가.

- 시작 조건: Public inbound observer와 handler가 등록되어 있다.
- 절차: Request와 one-way packet을 각각 한 번 보낸다.
- 검증: Observer가 두 identities를 한 번씩 기록하고 handler results와 일치한다.
- 세부 동작: [Stream session §8](../spec/19-stream-session.ko.md)을 검증한다.

#### SM-D10 Stream backpressure를 Session별로 격리한다

우선순위: `P1`

한 Session의 slow consumer가 다른 Session의 send·reply를 막아서는 안 된다.

**검증 질문:** Session A가 public HWM에서 pending이어도 B request와 push가 완료되는가.

- 시작 조건: A와 B를 서로 다른 Session gateway process에 배치한다. A gateway만 small public
  `ApplicationHwmBytes`와 application receive gate를 사용하고, B gateway는 별도 HWM 경계에서 정상
  동작하게 구성한다. A client receive를 gate로 막은 뒤 A의 public status에서 receive paused를 확인한다.
- 절차: A에 sends를 시작해 source awaitable pending을 확인하고 B request·push를 실행한다. A gate를
  해제한다.
- 검증: B results는 A gate 해제 전에 완료한다. A operations는 success 또는 deadline terminal 하나씩을
  가지며 Session state가 손상되지 않는다.
- 세부 동작: [Stream session §7](../spec/19-stream-session.ko.md)을 검증한다.

#### SM-D11 Stream과 Channel requests를 같은 client에서 분리한다

우선순위: `P1`

같은 Application이 두 transport surfaces를 함께 사용해도 reply correlation은 각 operation에 유지된다.

**검증 질문:** Interleaved Stream·Channel requests가 자기 payload reply만 받는가.

- 시작 조건: Stream Session과 Channel target이 ready다.
- 절차: 서로 다른 markers의 requests를 각 surface에서 50개씩 interleave한다.
- 검증: 100 replies가 operation ID와 input surface에 정확히 대응하며 cross-delivery가 없다.
- 세부 동작: [Interaction model](../spec/03-interaction-model.ko.md)을 검증한다.

#### SM-D12 다른 gateway reconnect 뒤 Actor state를 rebind한다

우선순위: `P0`

Session owner process와 Actor owner는 분리되어 있으므로 gateway를 바꿔도 Actor state는 유지된다. Binding은
새 Session에서 다시 만든다.

**검증 질문:** Session-b auth·rebind 뒤 같은 Actor state로 messaging을 계속하는가.

- 시작 조건: Session-a에 Actor가 bind되어 state counter가 10이다.
- 절차: Connection을 끊고 session-b에 reconnect·auth한 뒤 current ActorRef로 bind하고 state request를 보낸다.
- 검증: Counter는 10에서 계속 증가하고 reply·push는 session-b에 도착한다. Old binding은 재사용하지 않는다.
- 세부 동작: [Failover policy §6](../spec/31-failure-failover-policy.ko.md)을 검증한다.

#### SM-D13 Stream heartbeat loss를 disconnect로 처리한다

우선순위: `P1`

Heartbeat가 정상인 connection은 유지하고 heartbeat가 중단된 Session은 configured deadline 뒤 disconnected로
처리한다.

**검증 질문:** Heartbeat blackhole 뒤 connector disconnect와 bound Actor callbacks가 관찰되는가.

- 시작 조건: Bound Session이 heartbeat를 정상 교환하고 있다.
- 절차: Runner가 heartbeat direction을 차단하고 public deadline·tolerance로 disconnect를 기다린다.
- 검증: Connector는 Disconnected이고 current bound Actors가 disconnect callback을 최대 한 번씩 받는다.
- 세부 동작: [Stream session §6](../spec/19-stream-session.ko.md)을 검증한다.

#### SM-D14 TLS Stream에서 auth·relay·push를 수행한다

우선순위: `P2`

TLS는 transport security를 바꾸지만 Session·binding의 application 의미는 바꾸지 않는다.

**검증 질문:** Valid certificate는 SM-D2 flow를 성공시키고 invalid certificate는 auth 전에 거부되는가.

- 시작 조건: Valid trust chain과 invalid certificate server variants가 있다.
- 절차: TLS connectors로 두 endpoints에 연결한다.
- 검증: Valid connection은 auth·bind·relay·push를 완료하고 invalid connection은 public TLS error로 끝나며
  Session handler가 실행되지 않는다.
- 세부 동작: [Stream session §10](../spec/19-stream-session.ko.md)을 검증한다.

#### SM-D15 Channel→Actor→bound Session push 사슬을 완료한다

우선순위: `P0`

다른 backend role이 시작한 state change가 Channel, Actor direct와 bound push를 거쳐 최종 Stream client까지
도달해야 한다.

**검증 질문:** One operation marker의 push를 bound client가 실제로 받는가.

- 시작 조건: Backend Channel, bound Actor와 Stream client가 ready다.
- 절차: Backend request가 Actor send를 시작하고 Actor handler가 bound push를 보낸다.
- 검증: Client가 marker push를 한 번 받는다. Public flow trace가 각 hop을 같은 flow로 연결한다.
- 세부 동작: [Flow correlation §5](../spec/27-flow-correlation.ko.md)을 검증한다.

### Track E — Negative dispatch와 timer를 확인

#### SM-E1 Handler 없는 Spot request를 관찰한다

우선순위: `P0`

Ready Spot에 packet handler가 없으면 dispatch error를 caller와 observer가 확인할 수 있어야 한다.

**검증 질문:** Missing Spot handler가 error reply와 `no_handler/reply_error` evidence를 만드는가.

- 시작 조건: Spot과 public message-flow observer가 ready다.
- 절차: Missing packet request와 normal packet request를 보낸다.
- 검증: First는 정식 error와 observer evidence, second는 normal reply를 한 번 반환한다.
- 세부 동작: [Message flow tracing §2.2](../spec/26-message-flow-tracing.ko.md)을
  검증한다.

#### SM-E2 Spot one-shot timer가 state를 변경한다

우선순위: `P1`

Timer callback은 Spot execution lane에서 application state를 바꾸고 public evidence를 남긴다.

**검증 질문:** One-shot timer가 한 번 실행되어 counter와 push를 한 번 변경하는가.

- 시작 조건: Counter 0인 Spot과 bound notification target이 있다.
- 절차: Public Spot context로 one-shot timer를 등록하고 callback evidence를 bounded polling한다.
- 검증: Callback count와 counter delta는 1이고 client push도 한 번이다.
- 세부 동작: [Spot messaging §6](../spec/12-spot-messaging.ko.md)을 검증한다.

#### SM-E3 Idle timer가 explicit close를 시작한다

우선순위: `P1`

Framework가 inactivity를 추측해 자동 close하지 않는다. Application timer가 last activity와 membership을
확인하고 explicit close를 호출한다.

**검증 질문:** Idle·empty Spot만 close되고 active 또는 member-containing Spot은 유지되는가.

- 시작 조건: Idle empty, active empty와 idle member-containing Spots를 준비한다.
- 절차: Timer callbacks가 각 상태를 확인하도록 하고 public Find·closing evidence를 수집한다.
- 검증: Idle empty Spot만 close되고 callback reason은 ExplicitClose다. 다른 two Spots는 request를 계속
  처리한다.
- 세부 동작: [Spot actor §7](../spec/15-spot-actor.ko.md)을 검증한다.

#### SM-E4 Timer overrun policy별 observable sequence를 확인한다

우선순위: `P1`

Handler가 interval보다 오래 걸릴 때 `SkipLateTicks`, `CatchUpBounded`와 `DelayNextTick`은 서로 다른 callback
sequence를 만든다.

**검증 질문:** Application gate로 만든 overrun 뒤 callback count·spacing이 configured policy와 일치하는가.

- 시작 조건: Same interval의 fresh timer를 policy별로 만들고 first callback을 gate에서 보류한다.
- 절차: 여러 due boundaries를 지난 뒤 gate를 해제하고 bounded observation window의 callback timestamps를
  수집한다.
- 검증: 각 policy가 spec의 skip, bounded catch-up 또는 delayed-next 규칙을 지킨다. Exact scheduler
  nanosecond와 thread timing은 비교하지 않는다.
- 세부 동작: [Spot messaging §6](../spec/12-spot-messaging.ko.md)을 검증한다.

### Track F — Channel·Node·Spot routes가 같은 MeshNode transport에서 공존

#### SM-F1 Same-node Spot direct request와 send를 처리한다

우선순위: `P0`

Same-process optimization이 public reply·send 의미를 바꾸면 안 된다.

**검증 질문:** Same-node request reply와 send marker가 target Spot에서 한 번씩 관찰되는가.

- 시작 조건: Caller와 target Spot이 같은 MeshNode process에 있다.
- 절차: SpotId request와 send를 각각 한 번 시작한다.
- 검증: Request reply와 send handler evidence가 input markers와 일치한다.
- 세부 동작: [Spot messaging §3](../spec/12-spot-messaging.ko.md)을 검증한다.

#### SM-F2 다른 MeshNode의 Spot을 SpotId로 호출한다

우선순위: `P0`

Remote Spot direct caller도 target owner details를 입력하지 않는다.

**검증 질문:** Remote target handler가 request·send를 각각 한 번 처리하는가.

- 시작 조건: Source와 target MeshNodes가 ready이고 User Spot은 target에 있다.
- 절차: Source endpoint가 SpotId만 사용해 request와 send를 보낸다.
- 검증: Target evidence만 증가하고 request reply가 source로 돌아온다.
- 세부 동작: [Spot messaging §3](../spec/12-spot-messaging.ko.md)을 검증한다.

#### SM-F3 ChannelName·Node direct·Spot direct namespace를 분리한다

우선순위: `P0`

같은 packet name을 사용해도 target surface가 다르면 대응 handler와 reply context를 구분한다.

**검증 질문:** 세 request가 Channel, Node와 Spot handlers에 각각 한 번 도달하는가.

- 시작 조건: Same MeshNode에 세 handlers가 같은 packet name으로 ready다.
- 절차: 각 public target API로 unique marker request를 보낸다.
- 검증: 각 handler가 자기 marker만 처리하고 caller가 matching replies를 받는다.
- 세부 동작: [Interaction model §3](../spec/03-interaction-model.ko.md)을
  검증한다.

#### SM-F4 Missing Spot과 stale SpotRef를 구분한다

우선순위: `P0`

SpotId message는 current logical object를 찾고 exact SpotRef close는 특정 incarnation을 제한한다.

**검증 질문:** Missing direct calls는 `NotFound`이고 old ref close는 `InvalidOperation`인가.

- 시작 조건: Missing ID와 close·recreate한 same ID의 old SpotRef를 준비한다.
- 절차: Missing request·send와 old ref close를 실행한다.
- 검증: Direct calls는 `NotFound`, old ref close는 `InvalidOperation`이고 recreated Spot은 request를 처리한다.
- 세부 동작: [Failover policy §4.1](../spec/31-failure-failover-policy.ko.md)을
  검증한다.

#### SM-F5 Spot close가 MeshNode Channel을 종료하지 않는다

우선순위: `P0`

User Spot lifecycle은 containing MeshNode와 Channel handler lifecycle과 별개다.

**검증 질문:** Spot close 뒤 Spot call만 실패하고 Channel request는 계속 성공하는가.

- 시작 조건: User Spot과 Channel handler가 같은 MeshNode에 ready다.
- 절차: Both requests를 확인한 뒤 Spot을 close하고 다시 둘을 호출한다.
- 검증: Spot request는 NotFound이고 Channel request는 normal reply를 받는다. MeshNode status는 ready다.
- 세부 동작: [MeshNode §4](../spec/13-mesh-node.ko.md)을 검증한다.

#### SM-F6 Cross-node Spot call과 Actor Join을 같은 RouteMesh에서 처리한다

우선순위: `P0`

Spot direct와 cross-node Actor Join은 같은 MeshNode transport를 사용하지만 각 target identity와 lifecycle을
유지한다.

**검증 질문:** Remote Spot request·send와 Actor Join이 target에서 각각 한 번 완료되는가.

- 시작 조건: Source Entry Actor는 play-a, target User Spot은 play-b에 있다.
- 절차: Source가 target Spot request·send를 실행하고 Actor가 같은 target으로 Join한다.
- 검증: Spot handlers와 Join callbacks가 target에서 정식 횟수로 실행된다. Actor generation·state가 유지되고
  follow-up request도 target에서 처리된다.
- 세부 동작: [Spot actor §5](../spec/15-spot-actor.ko.md)을 검증한다.

### Track G — Node crash, scale-out과 placement를 처리

#### SM-G1 Play node crash 뒤 Application이 Actor를 recreate·rebind한다

우선순위: `P0`

이번 failover 범위는 crashed Actor state를 Framework가 자동 복원하지 않는다. Application은 old authority가
invalid된 뒤 같은 ActorId를 새 incarnation으로 만들고 Session을 다시 bind한다.

**검증 질문:** Crash 뒤 old Actor calls가 실패하고 explicit recreate·rebind 뒤 messaging이 복구되는가.

- 시작 조건: Play-a Actor는 Session에 bind되어 있고 play-b의 independent Actor도 정상이다.
- 절차: Play-a를 강제 종료하고 pending·fresh requests 결과를 수집한다. Old owner가 invalid된 뒤 same ActorId를
  replacement 또는 play-b에 GetOrCreate하고 current ref로 rebind한다.
- 검증: Old operations는 bounded error이고 auto retry되지 않는다. New incarnation은 different generation으로
  request·push를 처리하며 old ref bind는 `InvalidOperation`이다. Independent play-b Actor는 영향받지 않는다.
- 세부 동작: [Failover policy §5](../spec/31-failure-failover-policy.ko.md)와
  [§6](../spec/31-failure-failover-policy.ko.md)을 검증한다.

#### SM-G2 Scale-out은 기존 owners를 유지하고 신규 objects만 배치한다

우선순위: `P1`

Node 추가만으로 existing Actor·Spot을 자동 재분배하지 않는다. New create만 current eligible capacity와
weight를 사용한다.

**검증 질문:** Play-b 추가 뒤 old objects는 A, directed new objects는 B에서 처리되는가.

- 시작 조건: A만 eligible할 때 old Actor와 Spot을 만든다.
- 절차: B를 추가해 ready를 확인하고 old requests를 보낸다. A placement weight를 0으로 바꾼 뒤 new Actor와
  Spot을 create한다.
- 검증: Old evidence는 A, new evidence는 B에만 기록된다. Scale-out 자체가 old owners를 바꾸지 않는다.
- 세부 동작: [MeshNode §5](../spec/13-mesh-node.ko.md)을 검증한다.

#### SM-G3 Concurrent Join·Leave requests가 membership terminal을 하나씩 만든다

우선순위: `P1`

Lifecycle requests가 동시에 들어와도 Actor별 current membership과 callback count가 terminal results와
일치해야 한다.

**검증 질문:** Actors 20개의 mixed Join·Leave operations가 중복 callback 없이 final membership에
수렴하는가.

- 시작 조건: Actors와 source·target Spots가 ready다.
- 절차: Actor별 operation plan을 고정하고 concurrent Join 또는 Leave와 state request를 실행한다.
- 검증: Each operation은 Accepted, Rejected 또는 정식 conflict result 하나를 가진다. Accepted final
  memberships와 public callback counts가 일치하고 Actor handler overlap은 없다.
- 세부 동작: [Spot actor §4](../spec/15-spot-actor.ko.md)를 검증한다.

#### SM-G4 많은 bound Session pushes를 target별로 격리한다

우선순위: `P2`

많은 bindings에서도 push가 다른 Session으로 전달되면 안 된다.

**검증 질문:** Actor별 successful pushes가 해당 bound client에만 도착하는가.

- 시작 조건: Actor 100개를 서로 다른 Sessions에 one-to-one bind한다.
- 절차: 각 Actor가 unique marker pushes를 bounded concurrency로 시작한다.
- 검증: Successful push marker는 정확한 client에서 한 번 관찰되고 다른 clients에는 없다. Failed terminal은
  delivery success로 세지 않는다.
- 세부 동작: [Session Actor dispatch §5](../spec/20-session-actor-dispatch.ko.md)를
  검증한다.

#### SM-G5A Placement weight 100:300 비율을 충분한 표본으로 확인한다

우선순위: `P0`

Placement weight는 신규 object target의 상대 선택 비율이며 exact alternation을 보장하지 않는다.

**검증 질문:** Equal-capacity nodes에서 800 creates 중 weight 300 node가 65~85%를 소유하는가.

- 시작 조건: A weight 100, B weight 300이고 같은 type capacity가 충분하다.
- 절차: Unique IDs의 Actors 또는 User Spots 800개를 create한다.
- 검증: 모든 creates가 성공하고 owner count 합계는 800이다. B 비율은 65~85%이며 existing owner는 바뀌지
  않는다.
- 세부 동작: [MeshNode §5](../spec/13-mesh-node.ko.md)을 검증한다.

#### SM-G5B Capacity가 없는 high-weight node를 신규 placement에서 제외한다

우선순위: `P0`

Weight 계산 전에 stable type과 total capacity를 만족하는 candidates만 남겨야 한다.

**검증 질문:** High-weight B capacity가 full이면 new create가 eligible A에서 성공하는가.

- 시작 조건: B weight 10000의 relevant capacity를 채우고 A weight 1에는 slot을 남긴다.
- 절차: New global ID create를 한 번 실행한다. 별도 startup variants에서 weight -1과 10001을 적용한다.
- 검증: Create owner는 A이고 factory는 한 번 실행된다. Invalid weights는 listener ready 전에 configuration
  error다.
- 세부 동작: [MeshNode §5](../spec/13-mesh-node.ko.md)을 검증한다.

## 4. 완료 기준

- 모든 scenario는 public Framework API, public status·observer와 application evidence만 사용한다.
- Factory와 handler 순서는 application signal로 제어하며 internal CAS·queue·Store row를 읽지 않는다.
- Stream·Session scenario는 actual client reply·push·close result를 확인하고 server log만으로 통과하지 않는다.
- Weighted placement는 충분한 sample과 tolerance를 사용하며 target 선택 순서를 고정하지 않는다.
- Raw invalid UTF-8 frame, UUID generator conflict와 private protocol failure는 contract/internal test가
  검증한다.
