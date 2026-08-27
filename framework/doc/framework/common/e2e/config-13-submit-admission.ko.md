<!-- framework-adapter-nav:start -->
[E2E 목차](README.ko.md) | [이전: Channel egress routing](config-12-channel-egress-routing.ko.md) |
[다음: Instance Spot activation](config-14-instance-spot.ko.md)
<!-- framework-adapter-nav:end -->

# Config 13 — One-way submit admission

One-way send와 publish는 payload를 반환하지 않는다. 정상 완료는 해당 operation family의 source admission이
message를 수락했다는 뜻이며 원격 handler 실행 완료를 뜻하지 않는다. Queue가 바로
수락하지 못하면 public send deadline 안에서 capacity를 기다리고, timeout 예외·Shutdown 중 먼저
확정된 결과 하나로 끝난다.

이 config는 실제 process 사이에서 public one-way API의 완료와 실패를 검증한다. Client는 역할 server의
application endpoint를 호출하고, 역할 server가 Framework public API로 send·publish·reply를 시작한다.
Transport attempt count, private queue, socket buffer 크기와 test-only snapshot barrier는 사용하지 않는다.

## 1. 확인 범위

- 즉시 수락과 capacity 회복 뒤 수락
- Bounded pending admission, deadline, Shutdown과 지원 언어의 cancellation
- Node·Channel·Spot·Actor·Session·STREAM·classic fanout의 one-way 의미
- Logical Multicast의 부분 전달과 target별 결과 부재
- Direct logical target과 select-one Channel의 target 선택 차이
- Terminal 뒤 route 복구와 자동 재제출 금지
- STREAM send ordering과 reply token의 one-shot 사용

## 2. 배포 구성

| 역할 | 수 | 하는 일과 분리 이유 |
|---|---:|---|
| Location Store | 1 | Automatic topology와 global Spot·Actor 위치를 제공한다. |
| Admission caller | 1 | Object Client이며 Node·Channel·Spot·Actor와 Logical Multicast operation을 시작한다. |
| Mesh target | 2 | Channel, Spot, Actor와 Logical Multicast handler를 제공한다. Public job queue cap과 handler-start gate로 admission 조건을 만든다. |
| ClientServer target | 2 | ClientServer send handler와 weighted select-one 후보를 제공한다. |
| Session gateway | 2 | Bound Session send, Session Actor relay와 server Stream send·reply를 제공한다. |
| Fanout publisher·subscriber | 각 1 | Classic fanout publish terminal과 subscriber delivery를 분리해 검증한다. |
| Stream peer | 1 | Public stream connector로 server message를 받고 request를 보낸다. |
| E2E client | 1 | 역할 server의 public application endpoint와 Stream endpoint만 호출한다. |

각 target handler는 operation ID, sequence와 application payload를 evidence에 기록한다. Source endpoint는
public awaitable의 pending 여부와 terminal result를 application operation 상태로 제공할 수 있다. 이 상태는
Framework 내부 waiter나 queue length를 노출하지 않는다.

## 3. 공통 backpressure와 판정 방법

Backpressure scenario는 public `MaxQueuedApplicationJobs`와 handler-start gate로 shared permit
capacity wait를 만든다. Public status의 effective max, reserved/queued와 waiter만 읽는다. Source endpoint가 시작한
public awaitable이 terminal이 아닌 상태인지를 bounded polling하여 pending을 확인한다. 필요한 pending 상태가
common setup timeout 안에 만들어지지 않으면 scenario setup 실패로 끝내며 payload 크기, socket buffer와 반복
횟수나 queue cap을 실행 중에 늘리지 않는다.

Send terminal과 remote 실행은 별도 evidence로 판정한다. 정상 send terminal을 먼저 확인하고 handler
completion은 application gate를 연 뒤 확인한다. Deadline 또는 cancellation으로 끝난 operation은 gate와
route가 복구된 뒤에도 handler에서 실행되면 안 된다.

Logical Multicast는 target별 delivery report를 반환하지 않는다. Public result와 수락 가능한 target의
handler evidence만 확인하며 private snapshot member와 admission attempt 횟수는 internal test가 검증한다.

## 4. Scenario

### Track A — Public one-way terminal을 확인

#### SA-E2E-01 Ready target에 즉시 submit한다

우선순위: `P0`

Queue에 capacity가 있으면 one-way call은 payload 없는 정상 terminal을 반환해야 한다.

**검증 질문:** 각 one-way family가 ready 상태에서 정상 완료하고 target handler가 한 번 실행되는가.

- 시작 조건: Node direct, RouteMesh·ClientServer Channel, Spot, Actor, bound Session, Session Actor relay,
  Stream과 classic fanout target이 ready이고 handler gate가 열려 있다.
- 절차: Family마다 고유 operation ID의 send 또는 publish를 한 번 시작한다.
- 검증: 각 public awaitable은 결과 payload 없이 정상 완료한다. 대응 handler는 operation ID를 한 번
  기록한다.
- 세부 동작: [오류 모델 §4](../spec/server/00-foundation/07-framework-error-model.ko.md)의 정상 send 완료를
  검증한다.

#### SA-E2E-02 Shared job capacity가 회복되면 pending send를 수락한다

우선순위: `P0`

Target의 shared permit이 모두 예약돼도 deadline 전에 handler가 시작해 permit을 반환하면 Application이
같은 operation을 다시 호출하지 않고 원래 awaitable이 완료되어야 한다.

**검증 질문:** Shared job capacity 회복 뒤 pending send가 정상 완료되고 handler에서 최대 한 번 처리되는가.

- 시작 조건: Target의 `MaxQueuedApplicationJobs = 1`이고 handler-start gate가 닫혀 있다. Blocker job으로
  permit을 예약하고 public status에서 reserved/queued 1을 확인한다.
- 절차: Source endpoint가 다음 marker를 send하고 awaitable이 pending임을 확인한다. Gate를 열어 blocker
  handler가 시작하고 permit을 반환하게 한다.
- 검증: 원래 send가 결과 payload 없이 정상 완료하고 marker는 handler evidence에 한 번만 나타난다.
  Application은 send를 다시 호출하지 않는다.
- 세부 동작: [오류 모델 §4](../spec/server/00-foundation/07-framework-error-model.ko.md)의 send-ready 대기를
  검증한다.

#### SA-E2E-03 Pending send를 bounded terminal로 끝낸다

우선순위: `P0`

Shared job capacity wait 중에도 유한한 send 집합은 각자의 deadline 안에서 terminal
결과를 가져야 한다. 이 scenario는 내부 pending waiter의 크기를 검증하지 않는다.

**검증 질문:** Shared capacity로 pending된 sends가 무기한 남지 않고 success 또는 `DeadlineExceeded`로 한 번씩 끝나는가.

- 시작 조건: 서로 다른 target process 두 개에 `MaxQueuedApplicationJobs = 1`과 handler-start gate를
  설정한다. 각 target에 blocker job을 먼저 보내 reserved/queued 1을 확인한다. 두 send deadline은 짧고 유한하다.
- 절차: 각 target으로 서로 다른 operation ID의 send를 시작한다. 첫 target의 gate는 deadline 전에 열고
  두 번째 target의 gate는 deadline까지 유지한다.
- 검증: 모든 awaitable이 bounded 시간 안에 terminal 하나를 가진다. 첫 target의 marker는 최대 한 번이고
  두 번째 target의 operation은 `DeadlineExceeded`로 끝나며 handler evidence에 없다.
- 세부 동작: [오류 모델 §4](../spec/server/00-foundation/07-framework-error-model.ko.md)를 검증한다.

#### SA-E2E-04 Deadline 뒤 늦은 capacity가 operation을 되살리지 않는다

우선순위: `P0`

Send deadline이 먼저 끝났다면 이후 queue capacity가 생겨도 완료된 operation을 제출해서는 안 된다.

**검증 질문:** `DeadlineExceeded` 뒤 gate를 열어도 이전 marker가 handler에 전달되지 않는가.

- 시작 조건: Send가 pending이 되도록 `MaxQueuedApplicationJobs = 1`과 handler-start gate를 구성한다.
- 절차: Public send deadline이 끝날 때까지 gate를 유지한다. `DeadlineExceeded` terminal을 확인한 뒤 gate를
  열고 새 operation ID의 send를 보낸다.
- 검증: 이전 marker는 handler evidence에 없고 새 marker만 한 번 처리된다.
- 세부 동작: [오류 모델 §4](../spec/server/00-foundation/07-framework-error-model.ko.md)의 late admission
  차단을 검증한다.

#### SA-E2E-05 Target 부재와 route 미연결을 구분한다

우선순위: `P0`

Logical target이 없는 경우와 target은 있지만 current route를 사용할 수 없는 경우는 Application이
구분할 수 있어야 한다.

**검증 질문:** Missing target은 `NotFound`, known target의 disconnected route는 `Unavailable`인가.

- 시작 조건: 한 ID는 생성하지 않고, 다른 Actor·Spot은 생성한 뒤 runner가 owner route를 차단한다.
- 절차: 두 logical ID로 send를 각각 한 번 시작한다.
- 검증: Missing ID는 `NotFound`, known ID는 `Unavailable` terminal 하나로 끝난다. 두 marker 모두 target
  handler evidence에 없다.
- 세부 동작: [오류 모델 §4](../spec/server/00-foundation/07-framework-error-model.ko.md)의 error mapping을
  검증한다.

#### SA-E2E-06 Relocate와 Shutdown admission seal을 지킨다

우선순위: `P0`

Host가 신규 작업 수락을 닫은 뒤 시작한 send는 queue에 들어가지 않아야 한다.

**검증 질문:** Relocating 또는 ShuttingDown 상태에서 시작한 신규 send가 정식 terminal로 거부되는가.

- 시작 조건: Source host와 target이 ready이며 public Host operation으로 admission seal을 시작할 수 있다.
- 절차: Relocate variant와 Shutdown variant를 fresh host에서 실행한다. Public Host status가 신규 작업을
  받지 않는 상태가 된 뒤 send를 한 번 시작한다.
- 검증: Relocate variant는 해당 operation 계약의 rejection result, Shutdown variant는 `ShuttingDown`으로
  끝나며 target handler evidence가 없다.
- 세부 동작: [Host state와 완료 결과](../spec/server/05-location-relocation/05-host-relocation-flow.ko.md#3-host-state와-완료-결과)와
  [State별 admission](../spec/server/05-location-relocation/05-host-relocation-flow.ko.md#15-state별-admission)을 검증한다.

#### SA-E2E-07 Admission terminal과 publish commit을 구분한다

우선순위: `P1`

Pending admission이 timeout 또는 Shutdown으로 먼저 끝나면 handler가 실행되지 않아야 한다. Public admission
cancellation을 제공하는 언어에서는 cancellation variant도 실행한다. Logical Multicast submit이 정상
완료된 뒤 caller scope 종료는 이미 시작한 fanout을 rollback하지 않는다.

**검증 질문:** Commit 전 timeout·Shutdown과 지원 언어의 cancellation은 delivery를 막고, 정상 publish
terminal 뒤 caller scope 종료는 기존 delivery를 취소하지 않는가.

- 시작 조건: 일반 send는 pending 상태로 만들고, multicast target handler는 application gate에서
  대기하도록 구성한다.
- 절차: Pending send를 timeout 또는 Shutdown으로 끝낸다. 지원 언어에서는 cancellation도 별도 fixture에서
  실행한다. 별도 multicast를 publish하여 정상 terminal을 받은 뒤 caller scope를 끝내고 handler gate를 연다.
- 검증: Terminal send marker는 없다. Multicast marker는 수락된 target에서 최대 한 번 처리되고 public
  publish terminal은 바뀌지 않는다.
- 세부 동작: [Spot messaging §4](../spec/server/03-spot-actor/02-spot-messaging.ko.md)와
  [오류 모델 §4](../spec/server/00-foundation/07-framework-error-model.ko.md)를 검증한다.

### Track B — Operation family가 같은 admission 의미를 사용

#### SA-E2E-08 Node direct의 local·remote send를 비교한다

우선순위: `P0`

Target node가 같은 process인지 다른 process인지와 관계없이 send terminal은 source admission을 뜻한다.

**검증 질문:** Local과 remote Node direct send가 같은 terminal과 handler evidence를 만드는가.

- 시작 조건: Caller가 local Node RID와 remote Node RID에 모두 send할 수 있다.
- 절차: 같은 payload 의미의 send를 local과 remote target에 각각 한 번 보낸다.
- 검증: 두 awaitable은 결과 payload 없이 정상 완료하고 각 node handler가 자기 marker를 한 번 처리한다.
- 세부 동작: [Interaction model §3](../spec/server/00-foundation/04-interaction-model.ko.md)을
  검증한다.

#### SA-E2E-09 Channel topology별 send deadline을 적용한다

우선순위: `P0`

RouteMesh와 ClientServer Channel send는 queue capacity가 없으면 family send deadline까지 기다린 뒤
같은 public terminal 하나로 끝나야 한다.

**검증 질문:** RouteMesh와 ClientServer Channel send가 capacity 회복 전에는 pending이고 deadline 전
회복 시 성공하며, 회복하지 않으면 같은 timeout 결과를 반환하는가.

- 시작 조건: RouteMesh와 ClientServer를 실제 별도 topology로 구성한다. 각 topology에서 성공·timeout
  variant는 서로 다른 ChannelName과 target process를 사용한다. 각 target에
  `MaxQueuedApplicationJobs = 1`과 handler-start gate를 설정하고 blocker job의 reserved/queued 상태를 확인한다.
- 절차: 각 topology의 성공 target으로 send를 시작해 deadline 전에 gate를 열고, timeout target의 별도
  send는 gate를 deadline까지 유지한다.
- 검증: 두 topology 모두 success send는 payload 없는 정상 terminal과 handler 1회를 만들고, timeout
  send는 `DeadlineExceeded`와 handler 0회를 만든다.
- 세부 동작: [Channel messaging §7](../spec/server/02-channel-transport/02-channel-messaging.ko.md)과
  [ClientServer Channel §6](../spec/server/02-channel-transport/03-client-server-channel.ko.md)를 검증한다.

#### SA-E2E-11 SpotId send의 admission과 logical identity를 유지한다

우선순위: `P0`

Spot send가 pending인 동안 route가 사라져도 다른 Spot으로 target을 바꾸면 안 된다.

**검증 질문:** Original Spot route를 잃은 pending send가 `Unavailable`로 끝나고 다른 Spot이 처리하지
않는가.

- 시작 조건: `spot-a`가 ready이고 send를 pending으로 만들 수 있다. `spot-b`는 같은 stable type이지만 다른
  ID다.
- 절차: `spot-a` send가 pending인 것을 확인한 뒤 owner route를 종료한다. 이후 route를 복구하고 새
  operation ID를 보낸다.
- 검증: 이전 operation은 `Unavailable`이고 A·B 어느 handler에도 marker가 없다. 새 operation만 A에서 한
  번 처리된다.
- 세부 동작: [Failover policy §2](../spec/server/05-location-relocation/06-failure-failover-policy.ko.md)을 검증한다.

#### SA-E2E-12 ActorId send의 admission과 logical identity를 유지한다

우선순위: `P0`

Actor direct send도 original `ActorId`를 유지하며 route 복구를 이유로 완료된 operation을 재제출하지 않는다.

**검증 질문:** Actor route loss로 실패한 operation이 복구 뒤 자동 전달되지 않고 새 send만 처리되는가.

- 시작 조건: Actor가 ready이고 pending send를 만들 수 있다.
- 절차: Pending 중 owner route를 종료하여 terminal을 확인한다. Route ready 복구 뒤 다른 operation ID로
  send한다.
- 검증: 이전 send는 `Unavailable`이고 handler marker가 없다. 새 send는 정상 완료하고 Actor handler가 한
  번 처리한다.
- 세부 동작: [Failover policy §4.1](../spec/server/05-location-relocation/06-failure-failover-policy.ko.md)을
  검증한다.

#### SA-E2E-13 Logical Multicast는 수락 가능한 target을 한 번 처리한다

우선순위: `P0`

Logical Multicast는 current matching targets에 one-way로 전달하며 target별 success·failure 결과를 caller에
반환하지 않는다. 일부 target이 unavailable이어도 이미 가능한 target의 delivery를 rollback하지 않는다.

**검증 질문:** Matching target 하나가 unavailable이어도 다른 target이 marker를 한 번 처리하는가.

- 시작 조건: 같은 subscription의 target A와 B가 ready다. Runner가 B route를 unavailable로 만든 뒤 public
  status를 확인한다.
- 절차: Source가 고유 marker를 한 번 publish한다.
- 검증: Public terminal은 target별 결과 payload 없이 정식 의미로 완료한다. A는 marker를 한 번 처리하고
  B는 처리하지 않는다. Target이 0개인 variant도 정상 완료한다. 일부 target만 처리되는 partial 전달의
  target별 결과는 public result나 publish 전용 monitoring으로 반환·집계하지 않는다. E2E는 private snapshot과
  attempt count를 읽지 않는다.
- 세부 동작: [Spot messaging §4](../spec/server/03-spot-actor/02-spot-messaging.ko.md)를
  검증한다.

#### SA-E2E-14 Subscriber가 없어도 classic fanout publish를 완료한다

우선순위: `P0`

Classic fanout publish는 subscriber count나 delivery acknowledgement를 반환하지 않는다.

**검증 질문:** Subscriber가 0명인 publish가 정상 완료되고 late subscriber에게 replay되지 않는가.

- 시작 조건: Publisher는 ready이고 subscriber process는 시작하지 않았다.
- 절차: Marker를 publish하여 terminal을 확인한 뒤 subscriber를 시작해 ready로 만든다. 새 marker는 보내지
  않는다.
- 검증: Publish는 결과 payload 없이 정상 완료한다. Late subscriber handler에는 이전 marker가 없다.
- 세부 동작: [Framework API §11](../spec/server/00-foundation/06-framework-api.ko.md)을 검증한다.

#### SA-E2E-15 Bound Session과 Session Actor relay의 local·remote 결과를 비교한다

우선순위: `P0`

Session owner와 Actor owner가 local인지 remote인지에 따라 one-way terminal 의미가 바뀌어서는 안 된다.

**검증 질문:** Local·remote bound Session send와 Actor relay가 같은 deadline과 non-replay 규칙을
사용하는가.

- 시작 조건: Local binding과 remote binding을 fresh Session으로 각각 구성한다. Pending 변형은 각 Session을
  별도 gateway process에 배치하여 host shared job capacity를 공유하지 않게 하고, 해당 gateway의
  `MaxQueuedApplicationJobs = 1`과 handler-start gate로 capacity 대기를 만든다.
- 절차: 네 조합에서 정상 send를 한 번 실행하고, 별도 pending send는 deadline까지 capacity를 열지 않는다.
- 검증: 정상 sends는 결과 payload 없이 완료하고 target evidence가 한 번씩 나타난다. Pending sends는
  `DeadlineExceeded`이며 이후 capacity 복구 뒤 replay되지 않는다.
- 세부 동작: [One-way submit](../spec/server/01-execution/README.ko.md),
  [Admission deadline](../spec/server/01-execution/01-submit-and-completion.ko.md)과
  [Session Actor inbound dispatch](../spec/server/04-session/02-session-actor-binding.ko.md)를 검증한다.

#### SA-E2E-16 Server Stream send 순서를 유지한다

우선순위: `P0`

같은 Stream Session에서 수락된 server sends는 application 제출 순서를 유지해야 한다. 각 send call의
public `Timeout(...)` modifier로 실패한
send는 나중에 client에게 나타나면 안 된다.

**검증 질문:** Stream client가 성공한 sequence만 제출 순서로 받는가.

- 시작 조건: Public stream connector가 server Session에 연결되어 있고 server send HWM을 작게 설정한다.
  Server send gate는 닫아 두고 성공 marker에는 긴 call timeout, `timeout` marker에는 더 짧은 timeout을
  설정한다.
- 절차: `1`, `timeout`, `2`, `3` 순서로 server send를 시작한다. `timeout` operation이 deadline으로
  끝난 것을 확인한 뒤 긴 deadline이 끝나기 전에 gate를 연다.
- 검증: Client가 받은 성공 sequence `1,2,3`은 source의 successful terminal 순서와 같고 중복이 없다.
  `timeout` marker는 client에게 도착하지 않는다.
- 세부 동작: [Async execution — STREAM send call별 timeout](../spec/server/01-execution/README.ko.md)과
  [Stream session의 codec 계층 분리](../spec/server/04-session/01-stream-session.ko.md)를 검증한다.

#### SA-E2E-17 Stream reply token은 한 번만 사용한다

우선순위: `P0`

Request reply token은 유효한 첫 reply call이 사용한다. 첫 call이 normal, socket send timeout 또는
Shutdown으로 끝나도 같은 token을 다시 사용할 수 없다. Cancellation은 지원 언어의 추가 variant다.

**검증 질문:** 같은 reply token의 두 call 중 하나만 admission을 시작하고 client reply도 최대 하나인가.

- 시작 조건: Stream peer가 request를 보내고 server handler가 public reply token을 받는다.
- 절차: 같은 token으로 reply call 두 개를 만들고 application barrier에서 동시에 시작한다. Normal,
  socket send timeout과 Shutdown variant를 fresh request에서 반복하고, 지원 언어에서는 cancellation도 실행한다.
- 검증: 각 request에서 한 call만 정상 또는 첫 terminal을 얻고 다른 call은 local invalid-state error다.
  Client reply는 최대 하나이며 terminal token을 재사용해도 reply가 생기지 않는다.
- 세부 동작: [오류 모델 §3](../spec/server/00-foundation/07-framework-error-model.ko.md)의
  one-shot state를 검증한다.

### Track C — Target 선택과 terminal 뒤 동작을 확인

#### SA-E2E-18 Direct target과 Channel select-one을 구분한다

우선순위: `P0`

Direct send는 caller가 정한 logical identity를 유지한다. Channel select-one은 operation을 시작할 때 current
eligible member 중 하나를 선택할 수 있다.

**검증 질문:** Direct target이 unavailable일 때 다른 ID로 바꾸지 않고, Channel은 remaining ready member를
선택하는가.

- 시작 조건: Direct target A의 route는 unavailable이고 다른 logical target B는 ready다. 같은 ChannelName의
  Server A는 unavailable, Server B는 ready다.
- 절차: Direct A send와 ChannelName send를 각각 한 번 시작한다.
- 검증: Direct send는 `Unavailable`이며 B logical target은 처리하지 않는다. Channel send는 정상 완료하고
  ready Server B가 marker를 한 번 처리한다.
- 세부 동작: [Interaction model §3](../spec/server/00-foundation/04-interaction-model.ko.md)과
  [Failover policy §2](../spec/server/05-location-relocation/06-failure-failover-policy.ko.md)을 검증한다.

#### SA-E2E-19 Terminal 뒤 route 복구가 operation을 재제출하지 않는다

우선순위: `P0`

Timeout, connection loss 또는 Shutdown으로 끝난 operation은 route가 복구되어도 다시 pending 상태로
돌아가지 않는다. Public admission cancellation을 지원하는 언어는 cancellation variant도 실행한다.

**검증 질문:** Route 복구 뒤 이전 marker는 전달되지 않고 새 operation만 처리되는가.

- 시작 조건: Route를 unavailable하게 유지하여 send 하나를 terminal failure로 끝낼 수 있다.
- 절차: Failure terminal을 확인한 뒤 route를 복구하고 public status가 ready가 되면 새 operation ID로
  send한다.
- 검증: 이전 marker는 target evidence에 없고 새 marker만 한 번 처리된다. 이전 awaitable의 terminal도
  바뀌지 않는다.
- 세부 동작: [Transport liveness §6](../spec/server/02-channel-transport/05-transport-liveness.ko.md)의
  non-replay를 검증한다.

#### SA-E2E-20 Submit 완료와 remote handler 완료를 분리한다

우선순위: `P0`

Application이 send terminal을 remote 업무 완료로 해석하면 실제 handler 실패나 지연을 놓치게 된다.

**검증 질문:** Remote handler가 대기하는 동안 send terminal이 먼저 완료되는가.

- 시작 조건: Channel, Spot, Actor, fanout subscriber, bound Session과 Stream target handler가 marker를 받은
  뒤 application signal에서 대기한다.
- 절차: Family별 send를 한 번 시작하여 public terminal을 먼저 기다린다. Handler entered evidence를
  확인한 뒤 release signal을 보낸다.
- 검증: 각 send는 handler completion 전에 결과 payload 없이 정상 완료한다. Handler completion은 gate를
  연 뒤 한 번 기록된다.
- 세부 동작: [오류 모델 §4](../spec/server/00-foundation/07-framework-error-model.ko.md)의 source admission
  완료를 검증한다.

## 5. 완료 기준

- 모든 절차는 public one-way call, public Host·route status와 역할 server의 application evidence만 사용한다.
- Transport attempt, send-ready signal, private waiter, snapshot pass, socket buffer와 raw frame은 E2E 통과
  조건이 아니다.
- Pending 상태는 public awaitable의 미완료 상태와 `MaxQueuedApplicationJobs`·handler-start gate 조합으로 만들며 setup
  timeout 안에 재현되지 않으면 실패한다. Runtime 값을 바꾸어 재시도하지 않는다.
- 정상 terminal, timeout, cancellation과 Shutdown 결과는 operation마다 하나만 발생한다.
- Terminal 뒤 capacity·route 복구가 기존 operation을 자동 재제출하지 않는다.
- Public API 형태와 internal resource cleanup은 언어별 interface·contract test가 별도로 검증한다.
