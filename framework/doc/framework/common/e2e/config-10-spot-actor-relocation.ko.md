<!-- framework-adapter-nav:start -->
[E2E 목차](README.ko.md) | [이전: To-actor messaging](config-9-to-actor-messaging.ko.md) | [다음: 관측·운영 배포](config-11-observability-ops.ko.md)
<!-- framework-adapter-nav:end -->

# Config 10 — Spot actor join과 relocation

Actor가 다른 Spot으로 Join하면 Framework는 Actor의 membership과 current location을 바꾸고, 다른 node로
이동하는 경우 state와 아직 처리하지 않은 message도 target runtime으로 전달한다. Application은 source와
target의 내부 전환 시점을 알거나 message를 다시 보낼 필요가 없다.

이 config는 이 동작을 public Join·Relocate·message·binding API로 검증한다. Location Store row, relocation
payload, temporary queue와 내부 update packet은 직접 읽지 않는다. 대신 Join result, public Actor·Spot ref,
Application lifecycle callback·handler evidence와 bound client가 받은 push를 확인한다.

## 1. 확인 범위

- 같은 node와 다른 node의 Actor Join accept·reject
- `PreserveStateWith`와 `RecreateOnRelocation` state 처리
- 이동 중 수락된 request·send의 순서와 최대 한 번 처리
- Failure가 발생했을 때 source Actor와 binding이 유지되는 조건
- Actor relocation 뒤 bound Session의 route 갱신
- Message Follow 기간의 이전 route message와 만료 뒤 결과
- User Spot `PerActor`·`SpotWide` relocation과 execution turn 경계
- Deferred Join의 completion, timeout와 handler context

이번 version은 source·target node 또는 Store 장애 뒤 relocation operation을 다른 target에서 자동으로 다시
시작하지 않는다. 각 operation은 공개 success 또는 terminal failure 하나로 끝나며 Application이 이후
operation을 결정한다.

## 2. 배포 구성과 공통 evidence

| 역할 | 수 | 하는 일 |
|---|---:|---|
| Actor node | 2, multi-hop은 3 | Entry Spot, `PerActor`·`SpotWide` User Spot, Actor factory·handler와 lifecycle callback을 제공한다. |
| Session gateway | 2 | Stream Session을 열고 Actor를 bind하여 relay와 push를 제공한다. |
| Relocation caller | 1 | 역할 server의 public endpoint에서 Join·Relocate·Actor message를 호출한다. |
| Location Store | 1 | Public Actor·Spot location 조회와 routing을 제공한다. |
| Relocation Store | 1 | Framework가 relocation state를 보존할 때 사용한다. E2E는 내부 record를 읽지 않는다. |
| Network proxy | 필요할 때 1 | 연결 지연·차단을 만들며 Framework message를 생성하지 않는다. |
| E2E client | scenario별 | 역할 server endpoint와 Stream connector만 사용한다. |

Actor node는 callback 이름, Actor·Spot ID, operation ID, handler 순서, node ID와 domain state version을
Application state에 기록한다. Session gateway는 bind 결과와 client가 받은 push의 Actor ID·sequence를
기록한다. 순서 경합은 callback이나 handler가 public Application gate에서 대기하도록 만들어 재현한다.
Fixed sleep이나 내부 queue 길이로 전환 시점을 추정하지 않는다.

## 3. Scenario

### Track A — 같은 node의 Join

#### ST-A1 Local Join accept

우선순위: `P0`

같은 node의 Join은 Actor instance를 다시 만들지 않고 membership을 target Spot으로 바꾼다.

**검증 질문:** Accepted Join 뒤 후속 Actor request가 target Spot membership에서 처리되는가.

- 시작 조건: Actor가 Entry Spot에 있고 같은 node의 User Spot이 Ready다.
- 절차: Target `OnActorJoin`이 accept하도록 Join을 호출하고 success 뒤 Actor request를 보낸다.
- 검증: `OnActorJoin`, source `OnLeaveActor`, target `OnJoinedActor`가 각각 한 번 실행된다. 후속 handler는 target membership을 보고 Actor identity와 state는 유지된다.
- 계약 근거: [Spot actor](../spec/15-spot-actor.ko.md)

#### ST-A2 Local Join reject

우선순위: `P0`

Target Spot이 Join을 거부하면 Actor의 기존 membership과 message route는 바뀌지 않아야 한다.

**검증 질문:** Rejected Join 뒤 Actor가 source에서 계속 request를 처리하는가.

- 시작 조건: Target `OnActorJoin`이 typed rejection을 반환한다.
- 절차: Join result를 받은 뒤 같은 Actor ID로 state request를 보낸다.
- 검증: Join은 Rejected result와 reply를 반환한다. Leave·Joined callback은 실행되지 않고 state request는 source membership에서 처리된다.
- 계약 근거: [Spot actor](../spec/15-spot-actor.ko.md)

#### ST-A3 Local Join callback boundary

우선순위: `P1`

Join 완료 전에는 source와 target이 같은 Actor message를 동시에 처리하면 안 된다.

**검증 질문:** Target lifecycle callback이 대기하는 동안 message가 두 membership에서 중복 처리되지 않는가.

- 시작 조건: Target `OnJoinedActor`가 Application gate에서 대기한다.
- 절차: Join을 시작하고 callback 진입 뒤 고유 operation ID request를 보낸 다음 gate를 연다.
- 검증: Operation ID의 handler 실행은 source 또는 target 한 곳에서 한 번이며 Join과 request는 각각 terminal 하나로 끝난다.
- 계약 근거: [Spot actor](../spec/15-spot-actor.ko.md)

### Track B — 다른 node로 Actor 이동

#### ST-B1 PreserveState relocation

우선순위: `P0`

다른 node로 이동하는 Actor는 adapter가 저장한 Application state를 target instance에 복원한다.

**검증 질문:** Remote Join 완료 뒤 target Actor가 source의 state version을 유지하는가.

- 시작 조건: `PreserveStateWith` Actor가 node A에 있고 state version을 설정했다.
- 절차: Node B의 User Spot으로 Join하고 success 뒤 state request를 보낸다.
- 검증: Target factory·restore·`OnJoinedActor`와 source `OnLeaveActor`가 한 번씩 실행된다. Reply의 state version과 Actor identity는 이동 전과 같다.
- 계약 근거: [Spot actor](../spec/15-spot-actor.ko.md)

#### ST-B2 Moving message ordering

우선순위: `P0`

Framework는 target 복원을 시작한 뒤 이동 중 도착한 Actor message를 target Actor가 사용할 queue에 연결한다.
Application은 이 queue를 선택하거나 relay 시점을 계산하지 않는다.

**검증 질문:** 이동 전에 수락된 message와 이동 중 message가 target에서 순서대로 한 번 처리되는가.

- 시작 조건: Source handler 하나가 Application gate에서 대기하고 subsequent operation ID를 기록한다.
- 절차: `before`, Join, `during-1`, `during-2`를 순서대로 시작한 뒤 gate를 열고 Join 완료 후 `after`를 보낸다.
- 검증: 성공 처리된 IDs는 `before`, `during-1`, `during-2`, `after` 순서를 유지하며 각 ID는 한 번만 처리된다. Source와 target에서 같은 ID가 중복되지 않는다.
- 계약 근거: [Spot actor](../spec/15-spot-actor.ko.md), [Spot messaging](../spec/12-spot-messaging.ko.md)

#### ST-B3 RecreateOnRelocation

우선순위: `P0`

`RecreateOnRelocation` Actor는 relocation adapter 없이 target factory가 새 runtime instance를 만들고 identity를
유지한다.

**검증 질문:** Adapter를 등록하지 않은 Actor가 remote Join 뒤 target에서 message를 처리하는가.

- 시작 조건: Actor type에 `RecreateOnRelocation` policy와 factory만 등록한다.
- 절차: 다른 node의 User Spot으로 Join하고 instance evidence와 follow-up reply를 확인한다.
- 검증: Join이 성공하고 target factory는 한 번 실행된다. Capture·Restore application callback은 없으며 Actor ID를 지정한 follow-up은 target에서 처리된다.
- 계약 근거: [Spot actor](../spec/15-spot-actor.ko.md)

#### ST-B4 Empty relocation state

우선순위: `P1`

State adapter가 빈 payload를 반환하는 것은 relocation failure가 아니다. Target Actor는 factory로 만들어지고
필요한 domain state를 별도 store에서 읽을 수 있다.

**검증 질문:** Empty state Actor가 remote Join과 후속 request를 정상 처리하는가.

- 시작 조건: Adapter가 empty state를 반환하고 target factory가 external state를 읽는다.
- 절차: Remote Join 뒤 domain state request를 보낸다.
- 검증: Join은 성공하고 restore callback은 empty input으로 한 번 실행된다. Reply는 external state의 기대 값을 반환한다.
- 계약 근거: [Spot actor](../spec/15-spot-actor.ko.md)

### Track C — relocation failure

#### ST-C1 Location Store response loss

우선순위: `P0`

Join 중 location 갱신 결과를 확인할 수 없으면 Framework가 성공과 실패를 동시에 반환해서는 안 된다.

**검증 질문:** Store response loss가 Join terminal 하나로 끝나고 Actor message가 owner 한 곳에서만 처리되는가.

- 시작 조건: Network proxy로 relocation 중 Location Store response를 차단할 수 있다.
- 절차: Remote Join 중 response를 차단하고 public terminal 뒤 Actor request를 보낸다.
- 검증: Join은 success 또는 하나의 Store-related failure로 끝난다. Follow-up handler는 public current location과 같은 node에서 한 번 실행된다.
- 계약 근거: [Framework error model](../spec/32-framework-error-model.ko.md)

#### ST-C2 Target connection failure

우선순위: `P0`

Target에 relocation 요청을 전달할 수 없으면 source Actor를 제거하거나 route를 target으로 바꾸면 안 된다.

**검증 질문:** Target 연결 실패 뒤 source Actor가 기존 state로 request를 처리하는가.

- 시작 조건: Actor는 A에 있고 B로 가는 network path를 차단한다.
- 절차: B로 Join하고 failure 뒤 Actor state request를 보낸다.
- 검증: Join은 connection-related failure 하나로 끝난다. Leave·target Joined callback은 없고 state request는 A에서 기존 state를 반환한다.
- 계약 근거: [Framework error model](../spec/32-framework-error-model.ko.md)

#### ST-C3 Application callback failure

우선순위: `P1`

Admission reject와 callback exception, timeout은 서로 다른 공개 결과이므로 같은 Accepted 결과로 표현하면 안
된다.

**검증 질문:** Reject·exception·timeout 입력이 각각 terminal 하나를 반환하고 Actor를 중복 생성하지 않는가.

- 시작 조건: Target callback을 reject, exception, bounded wait로 구성한 Actor를 각각 준비한다.
- 절차: 각 Actor의 Join을 한 번 호출하고 terminal 뒤 public Actor request를 보낸다.
- 검증: Reject는 Rejected result를, exception과 timeout은 계약된 failure를 반환한다. 각 Actor request는 owner 한 곳에서 최대 한 번 처리된다.
- 계약 근거: [Framework error model](../spec/32-framework-error-model.ko.md)

### Track D — current location과 stale route

#### ST-D1 Join completion과 current location

우선순위: `P0`

Join completion callback이 호출될 때 Application은 target Actor를 current Actor로 사용할 수 있어야 한다.

**검증 질문:** Join completion을 받은 직후 별도 대기 없이 Actor request가 target에 도달하는가.

- 시작 조건: Actor는 A, target Spot은 B에 Ready다.
- 절차: Join completion callback에서 바로 state request를 시작한다.
- 검증: Completion은 한 번 호출되고 request는 B에서 한 번 처리된다. Caller가 owner RID를 다시 계산하거나 bind하지 않는다.
- 계약 근거: [Spot actor](../spec/15-spot-actor.ko.md)

#### ST-D2 Stale source message fencing

우선순위: `P1`

이동 완료 뒤 source route로 늦게 도착한 message가 source Actor를 다시 실행하거나 target과 중복 처리하면 안 된다.

**검증 질문:** 늦은 source message의 operation ID가 전체 handler에서 최대 한 번 나타나는가.

- 시작 조건: Network proxy가 이동 전 route의 message를 지연한다.
- 절차: Message를 지연한 채 Join을 완료하고 proxy를 복구한다.
- 검증: Caller는 공개 계약에 따른 result를 받고 operation ID handler count는 전체 node에서 최대 한 번이다. Source state는 변경되지 않는다.
- 계약 근거: [Spot actor](../spec/15-spot-actor.ko.md)

### Track E — Session binding

#### ST-E1 Bound Session push after relocation

우선순위: `P0`

Actor 이동 뒤 Framework는 bind된 Session이 참조하는 Actor location을 갱신한다. 사용자가 owner 변경을 알아내서
다시 bind할 필요가 없다.

**검증 질문:** Remote Join 완료 뒤 기존 bound client가 target Actor의 push를 받는가.

- 시작 조건: Client Session이 A의 Actor에 bind되어 있고 push sequence 1을 받았다.
- 절차: Actor를 B로 Join하고 completion 뒤 sequence 2 push를 보낸다.
- 검증: 같은 client가 재bind 없이 sequence 2를 한 번 받는다. Push evidence의 sender node는 B다.
- 계약 근거: [Session Actor dispatch](../spec/20-session-actor-dispatch.ko.md)

#### ST-E1B Relocation mode별 binding route

우선순위: `P0`

Actor 단독 이동, `PerActor` Spot relocation과 `SpotWide` relocation 모두 bound Session의 current route를
갱신해야 한다.

**검증 질문:** 세 이동 방식 모두 기존 binding으로 target push를 받는가.

- 시작 조건: 이동 방식별 Actor와 bound client를 별도 ID로 준비한다.
- 절차: 각 방식의 relocation completion 뒤 target에서 고유 push sequence를 보낸다.
- 검증: 각 client가 재bind 없이 해당 sequence를 한 번 받고 이전 node에서는 같은 sequence를 보내지 않는다.
- 계약 근거: [Session Actor dispatch](../spec/20-session-actor-dispatch.ko.md)

#### ST-E1C Session location update retry

우선순위: `P0`

Join completion은 Session route 갱신 응답을 기다리지 않는다. 이동한 runtime은 첫 응답을 받지 못하면 1초
뒤에 다시 요청한다. 이후에도 응답이 없으면 1초, 2초, 4초, 5초 간격으로 요청하고 그 뒤에는 5초 간격을
유지한다.

**검증 질문:** Session owner와의 network path가 일시적으로 끊겨도 Join은 완료되고 기존 binding이 결국 target push를 받는가.

- 시작 조건: Proxy가 target runtime과 Session owner 사이의 network path를 차단할 수 있다.
- 절차: Path를 차단한 상태에서 Remote Join completion을 확인한 뒤 path를 복구하고 bounded polling으로 target push delivery를 기다린다.
- 검증: Join completion은 Session owner path 단절 때문에 timeout되지 않는다. Path 복구 뒤 재bind 없이 target push를 받으며 duplicate push는 없다. 재전송 시각 자체는 public trace 계약이 있을 때만 진단한다.
- 계약 근거: [Session Actor dispatch](../spec/20-session-actor-dispatch.ko.md)

#### ST-E1A New Actor incarnation requires bind

우선순위: `P0`

Actor가 명시적으로 제거된 뒤 같은 Actor ID로 다시 만들어지면 이전 Actor와 Session의 binding은 끝난다.

**검증 질문:** 재생성된 Actor의 push는 explicit bind 전에는 이전 Session에 전달되지 않는가.

- 시작 조건: Session이 Actor에 bind되어 있고 이전 push를 받았다.
- 절차: Actor를 제거하고 같은 ID로 다시 만든 뒤 push를 보내고, 명시적으로 bind한 다음 다른 push를 보낸다.
- 검증: 첫 push는 이전 binding에 전달되지 않고 bind 성공 뒤 두 번째 push만 전달된다.
- 계약 근거: [Session Actor dispatch](../spec/20-session-actor-dispatch.ko.md)

#### ST-E2 Failed relocation keeps binding

우선순위: `P0`

Relocation이 실패하면 Session binding도 source Actor를 계속 가리켜야 한다.

**검증 질문:** Failed Join 뒤 source Actor의 push가 기존 bound client에 도달하는가.

- 시작 조건: Session은 A의 Actor에 bind되어 있고 target B connection을 차단한다.
- 절차: B로 Join failure를 확인한 뒤 A에서 push를 보낸다.
- 검증: Client가 재bind 없이 push를 한 번 받고 sender evidence는 A다.
- 계약 근거: [Session Actor dispatch](../spec/20-session-actor-dispatch.ko.md)

### Track F — 이동 중 message와 이전 route

#### ST-F1 In-flight handoff order

우선순위: `P0`

이동 전에 source가 수락한 작업은 이동 중과 이동 뒤에 수락한 작업보다 먼저 처리되어야 한다.

**검증 질문:** Source에서 대기하던 작업과 target으로 향한 작업의 순서가 유지되는가.

- 시작 조건: `old-1` handler를 gate에서 대기시키고 `old-2`가 수락되었음을 reply로 확인한다.
- 절차: Join, `moving-1`, `moving-2`를 시작한 뒤 gate를 열고 `new-1`을 보낸다.
- 검증: 성공 handler 순서는 `old-1`, `old-2`, `moving-1`, `moving-2`, `new-1`이고 각 ID는 한 번이다.
- 계약 근거: [Spot actor](../spec/15-spot-actor.ko.md)

#### ST-F2 Direct message cannot overtake

우선순위: `P0`

Join completion 직후 보낸 direct message도 이동 중 Framework가 보존한 message를 추월하면 안 된다.

**검증 질문:** Completion callback에서 보낸 message가 moving messages 뒤에 처리되는가.

- 시작 조건: Target restore callback을 gate에서 대기시킬 수 있다.
- 절차: Join 중 moving messages를 보내고 gate를 연 뒤 completion callback에서 direct request를 보낸다.
- 검증: Target handler evidence에서 moving IDs가 direct ID보다 앞서고 중복은 없다.
- 계약 근거: [Spot messaging](../spec/12-spot-messaging.ko.md)

#### ST-F3 Bound Session cross-move order

우선순위: `P0`

Session relay message와 direct Actor message는 relocation 경계에서도 Actor의 serial execution을 깨면 안 된다.

**검증 질문:** Bound relay와 direct operation이 target Actor에서 중복 없이 처리되는가.

- 시작 조건: Session이 Actor에 bind되어 있고 각 input에 sequence를 넣는다.
- 절차: Join 중 Session relay와 direct message를 정해진 순서로 제출한다.
- 검증: 모든 성공 sequence가 Actor handler evidence에 한 번씩 있고 동시에 실행된 handler 수는 1이다.
- 계약 근거: [Session Actor dispatch](../spec/20-session-actor-dispatch.ko.md)

#### ST-F3A Late Session route update

우선순위: `P0`

연속 relocation에서 첫 이동의 늦은 route 갱신이 두 번째 이동의 current location을 덮어쓰면 안 된다.
이 scenario는 내부 packet을 식별하지 않고 target runtime과 Session owner 사이의 public route 경계를
지연시켜 실행한다.

**검증 질문:** A→B→C 뒤 B에 대한 늦은 update가 있어도 bound push가 C에서 도착하는가.

- 시작 조건: Network proxy가 B runtime에서 Session owner로 가는 연결의 한 방향만 지연할 수 있다. C
  runtime의 연결은 지연하지 않는다. Proxy는 packet name, frame과 payload를 읽거나 생성하지 않는다.
- 절차: A→B Join을 완료한 뒤 B→Session owner 방향을 유지한 상태에서 B→C Join을 완료한다. Bound
  Session의 public ActorRef location snapshot이 C를 가리키는 것을 bounded wait로 확인하고 B 방향의
  지연을 해제한 다음 C에서 push를 보낸다.
- 검증: Client는 C push를 한 번 받고 B에서 같은 sequence를 받지 않는다.
- 계약 근거: [Session Actor dispatch](../spec/20-session-actor-dispatch.ko.md)

#### ST-F4 Message Follow before and after expiry

우선순위: `P1`

이동 직후 이전 route로 들어온 message는 설정된 Message Follow 기간에는 target으로 전달될 수 있다. 기간이
끝난 뒤에는 오래된 route를 계속 사용하면 안 된다.

**검증 질문:** Follow 기간 안의 message는 최대 한 번 처리되고 만료 뒤 message는 target handler에 들어가지 않는가.

- 시작 조건: 짧지만 test deadline보다 충분한 Message Follow duration을 공개 설정하고 proxy가 old-route message를 보관한다.
- 절차: Join 완료 뒤 첫 old-route message를 duration 안에 전달한다. 두 번째 message는 duration과 scheduler 허용 오차가 지난 뒤 전달한다.
- 검증: 첫 operation ID는 target에서 최대 한 번 처리된다. 두 번째 ID는 처리되지 않고 caller는
  `Unavailable`을 받는다.
- 계약 근거: [Spot actor](../spec/15-spot-actor.ko.md)

#### ST-F5 Message Follow route cleanup

우선순위: `P1`

Message Follow는 이전 route를 영구 유지하는 기능이 아니다. 만료 뒤 current route message는 계속 정상 처리해야
한다.

**검증 질문:** Follow 만료 뒤 old route는 사용되지 않고 current Actor request는 성공하는가.

- 시작 조건: Actor relocation을 완료하고 proxy가 이동 전에 받은 old-route message를 duration과 허용 오차가 지날 때까지 보관한다.
- 절차: Proxy가 old-route message를 전달한 뒤 global Actor ID route로 다른 operation을 보낸다.
- 검증: Old-route operation은 target handler에 없고 global ID operation은 target에서 한 번 처리된다.
- 계약 근거: [Spot actor](../spec/15-spot-actor.ko.md)

#### ST-F6 Request terminal across relocation

우선순위: `P1`

Relocation 중 request는 reply와 timeout을 동시에 반환하거나 reply correlation을 다른 request와 바꾸면 안 된다.

**검증 질문:** 서로 다른 deadline의 requests가 각 correlation에 terminal 하나를 받는가.

- 시작 조건: Handler가 short request deadline보다 길고 long request deadline보다 짧게 gate에서 대기한다.
- 절차: Join 중 short와 long request를 보내고 gate를 연다.
- 검증: Short는 timeout 또는 reply 하나, long은 기대 reply 하나를 받는다. Reply operation ID가 서로 바뀌지 않고 handler count는 각 ID 최대 한 번이다.
- 계약 근거: [Framework error model](../spec/32-framework-error-model.ko.md)

### Track G — Spot relocation과 execution turn

#### ST-G1 Yielded continuation barrier

우선순위: `P0`

Actor handler가 `Yield`한 continuation도 같은 Actor turn의 일부다. Relocation이 이를 다른 node와 동시에 실행하면
안 된다.

**검증 질문:** Yield 전후 state 변경과 relocation 뒤 request가 serial하게 관찰되는가.

- 시작 조건: Handler가 state를 변경하고 Yield한 뒤 두 번째 변경을 수행한다.
- 절차: Yield evidence 뒤 relocation과 follow-up request를 시작한다.
- 검증: Final state가 정해진 두 변경과 follow-up을 모두 포함하고 handler active count는 전체 node에서 1이다.
- 계약 근거: [비동기 실행 정책](../spec/05-async-execution-policy.ko.md)

#### ST-G2 User Spot aggregate capacity

우선순위: `P0`

`SpotWide` relocation은 Spot과 그 Actor 전체를 하나의 단위로 target capacity에 맞춰야 한다.

**검증 질문:** Capacity가 부족하면 일부 Actor만 이동하지 않고 source가 유지되는가.

- 시작 조건: Spot에 여러 Actor가 있고 target capacity는 전체 이동에 부족하다.
- 절차: SpotWide Relocate를 호출하고 terminal 뒤 각 Actor에 state request를 보낸다.
- 검증: Relocate는 capacity failure로 끝나며 모든 Actor request는 source에서 기존 state를 반환한다.
- 계약 근거: [Spot actor](../spec/15-spot-actor.ko.md)

#### ST-G3 PerActor Spot relocation

우선순위: `P0`

`PerActor` mode에서는 Spot과 Actor가 서로 다른 current location을 가질 수 있고 각 Actor route가 독립적으로
갱신되어야 한다.

**검증 질문:** 선택한 Actor만 target으로 이동하고 나머지 Actor는 source에서 처리되는가.

- 시작 조건: 같은 PerActor Spot에 Actor A와 B가 있다.
- 절차: A만 target으로 Relocate한 뒤 A와 B에 request를 보낸다.
- 검증: A handler는 target, B handler는 source에서 실행되고 두 Actor의 identity와 state는 유지된다.
- 계약 근거: [Spot actor](../spec/15-spot-actor.ko.md)

#### ST-G4 ToActor message during Spot move

우선순위: `P0`

SpotWide relocation 중 Actor ID로 보낸 message도 해당 Actor의 이동 순서를 따라야 한다.

**검증 질문:** Spot 이동 중 ToActor messages가 target에서 중복 없이 serial 처리되는가.

- 시작 조건: SpotWide Spot에 여러 Actor와 sequence handler가 있다.
- 절차: Spot Relocate 중 각 Actor에 고유 sequence messages를 보낸다.
- 검증: 각 Actor별 성공 sequence 순서가 유지되고 동일 operation ID가 source와 target에 중복되지 않는다.
- 계약 근거: [Spot actor](../spec/15-spot-actor.ko.md)

#### ST-G5 Relocation interruption measurement

우선순위: `P1`

Relocation 성능은 correctness 조건과 분리해 측정한다. 느린 환경을 실패로 만들기 위해 fixed sleep이나 작은
sample을 사용하지 않는다.

**검증 질문:** 고정 workload에서 message loss·duplicate 없이 relocation interruption 분포를 기록할 수 있는가.

- 시작 조건: Warm-up 뒤 충분한 Actor 수와 일정한 request rate를 사용한다.
- 절차: 동일 profile을 여러 회 실행하며 Join 시작·완료와 client latency를 monotonic clock으로 기록한다.
- 검증: 모든 accepted operation은 terminal 하나와 handler 최대 한 번을 가진다. P50·P95·P99는 결과로 보고하며 spec에 명시된 SLO가 있을 때만 pass/fail에 사용한다.
- 계약 근거: [Runtime metrics](../spec/25-runtime-metrics.ko.md)

#### ST-G6 SpotWide application boundary

우선순위: `P1`

SpotWide relocation은 현재 application turn이 끝난 뒤 시작하고 target 준비가 끝난 뒤 신규 turn을 처리해야 한다.

**검증 질문:** Relocation과 겹친 Spot handler가 source와 target에서 동시에 실행되지 않는가.

- 시작 조건: Spot handler가 Application gate에서 대기한다.
- 절차: Handler 진입 뒤 SpotWide Relocate와 follow-up request를 시작하고 gate를 연다.
- 검증: 기존 handler는 source에서 끝나고 follow-up은 target에서 한 번 실행된다. 전체 active count는 1을 넘지 않는다.
- 계약 근거: [Spot actor](../spec/15-spot-actor.ko.md)

### Track H — Deferred Join과 handler context

#### ST-H1 Deferred Join registration

우선순위: `P0`

Handler 안에서 Join을 defer하면 현재 handler는 completion을 기다리며 blocking하지 않고 종료할 수 있다.

**검증 질문:** Defer 호출 뒤 handler가 종료되고 target `OnActorJoin`이 호출되는가.

- 시작 조건: Actor handler가 public Join defer API와 immutable request를 사용한다.
- 절차: Handler에서 defer를 등록하고 종료 evidence를 남긴다.
- 검증: Evidence 순서는 `defer called`, `handler returned`, `OnActorJoin`이다. Join request payload는 호출 시점 값과 같다.
- 계약 근거: [Spot actor](../spec/15-spot-actor.ko.md)

#### ST-H2 Completion outcome

우선순위: `P0`

Deferred Join completion은 해당 operation의 Accepted, Rejected 또는 failure 결과를 한 번 전달한다.

**검증 질문:** Completion callback이 operation ID와 terminal result를 정확히 한 번 받는가.

- 시작 조건: Accept와 reject target을 각각 준비한다.
- 절차: 고유 operation ID로 deferred Join을 호출한다.
- 검증: 각 completion은 한 번 호출되고 ID와 result가 해당 Join과 일치한다. Accept 뒤 target, reject 뒤 source에서 후속 request를 처리한다.
- 계약 근거: [Spot actor](../spec/15-spot-actor.ko.md)

#### ST-H3 Context identity

우선순위: `P1`

Join admission callback은 Actor ID와 join request를 받고, Join completion callback은 Operation ID와
terminal result를 받는다. Application은 이 두 callback의 값을 섞어 해석하거나 내부 owner token을
요구하지 않는다.

**검증 질문:** Callback context의 public identity가 실제 Join 입력과 일치하는가.

- 시작 조건: `OnActorJoin` admission callback과 Join completion callback이 각각 public evidence를
  기록한다.
- 절차: 서로 다른 Actor와 target Spot으로 Join을 실행하고 admission callback과 completion callback의
  evidence를 따로 수집한다.
- 검증: Admission evidence의 Actor ID와 request가 입력과 일치한다. Completion evidence의 Operation ID,
  result와 Accepted ActorRef가 해당 Join과 일치한다. Target Spot은 callback에 없는 값을 억지로 읽지 않고
  public Actor context 또는 ref 조회로 확인한다.
- 계약 근거: [Spot actor](../spec/15-spot-actor.ko.md)

#### ST-H4 Invalid context and duplicate registration

우선순위: `P1`

Join defer를 허용하지 않는 callback이나 같은 handler turn의 중복 등록은 startup 또는 operation failure로 명확히
드러나야 한다.

**검증 질문:** 잘못된 사용이 hidden Join 없이 계약된 failure를 반환하는가.

- 시작 조건: 허용되지 않은 lifecycle callback과 중복 defer 입력을 별도 case로 준비한다.
- 절차: 각 case를 한 번 실행한다.
- 검증: Public call 또는 host startup이 하나의 configuration·operation failure로 끝나고 target Join callback은 실행되지 않는다.
- 계약 근거: [Framework error model](../spec/32-framework-error-model.ko.md)

#### ST-H4A Completion and timeout race

우선순위: `P0`

Deferred Join의 timeout과 accept가 경합해도 completion은 operation마다 한 번이어야 한다. 이 scenario는
공개 pending limit을 가정하지 않는다.

**검증 질문:** Timeout 경계에서 모든 deferred Join이 Accepted, Rejected 또는 failure 중 하나로 한 번
끝나는가.

- 시작 조건: 소수의 deferred Join과 각 operation의 bounded timeout을 준비하고 target admission callback을
  application gate로 제어한다.
- 절차: 한 operation의 accept를 timeout 경계와 겹치게 하고 다른 operation은 명시적으로 reject하거나
  정상 accept한다.
- 검증: 모든 operation이 Accepted, Rejected 또는 failure 하나로 끝난다. Completion count는 operation당
  1이고 timeout이 확정된 뒤 새 target callback이 실행되지 않는다. Timeout 전에 시작한 callback이 끝나는
  시점은 별도 성공 조건으로 사용하지 않는다.
- 계약 근거: [비동기 실행 정책](../spec/05-async-execution-policy.ko.md)과
  [Spot actor](../spec/15-spot-actor.ko.md)

#### ST-H4B Yield and reply terminal

우선순위: `P1`

Handler가 Yield하거나 비동기 작업을 기다려도 Join completion과 원래 request reply의 correlation이 바뀌면 안
된다.

**검증 질문:** Yield를 포함한 handler가 reply와 Join completion을 각각 한 번 전달하는가.

- 시작 조건: Handler가 Yield 뒤 deferred Join을 등록하고 typed reply를 반환한다.
- 절차: 고유 request와 Join operation ID로 호출한다.
- 검증: Caller reply와 Join completion은 각각 한 번이며 두 ID가 정확히 대응한다. Handler와 callback의 동시 active count는 계약된 execution lane을 넘지 않는다.
- 계약 근거: [비동기 실행 정책](../spec/05-async-execution-policy.ko.md)

#### ST-H5 MessageContext parity

우선순위: `P1`

언어별 Framework는 같은 handler 종류에서 같은 public identity와 cancellation·deadline 의미를 제공해야 한다.

**검증 질문:** 지원 언어의 Actor handler가 같은 MessageContext 값을 관찰하는가.

- 시작 조건: 서로 다른 언어 owner가 같은 typed message와 evidence schema를 구현한다.
- 절차: Send, request와 deferred Join을 각 언어 조합으로 실행한다.
- 검증: Actor ID, operation ID, request correlation과 deadline 존재 여부가 같은 의미를 가진다. Reflection이나 private adapter를 사용하지 않는다.
- 계약 근거: [Public contract governance](../spec/00-public-contract-governance.ko.md)

### Track I — 부하와 multi-hop

#### ST-I1 Payload size profile

우선순위: `P1`

Relocation payload와 이동 중 message 크기가 달라도 correctness 판정은 같아야 한다.

**검증 질문:** 정의한 payload size 구간에서 accepted operation이 유실·중복 없이 처리되는가.

- 시작 조건: Small·medium·large state와 message fixture를 고정한다.
- 절차: 각 구간에서 같은 Actor 수와 message 수로 remote Join을 반복한다.
- 검증: State checksum과 handler operation ID가 입력과 일치하고 각 ID는 한 번이다. Latency와 memory는 측정 결과로 별도 기록한다.
- 계약 근거: [Runtime metrics](../spec/25-runtime-metrics.ko.md)

#### ST-I2 Many Actor relocations

우선순위: `P1`

여러 Actor를 동시에 이동해도 relocation 대상이 아닌 control Actor의 public request가 계속 처리되어야 한다.

**검증 질문:** 대량 Actor relocation 중 control traffic이 유실되지 않는가.

- 시작 조건: 충분한 relocation Actor와 별도 control Actor를 준비한다.
- 절차: Bounded concurrency로 Actor relocation을 실행하면서 control requests를 일정 rate로 보낸다.
- 검증: Relocation과 control operation이 모두 terminal 하나를 가지며 성공 operation ID는 한 번 처리된다. Latency 분포를 보고한다.
- 계약 근거: [Runtime metrics](../spec/25-runtime-metrics.ko.md)

#### ST-I3 Many Spot relocations

우선순위: `P1`

여러 SpotWide 이동도 대상이 아닌 Spot의 execution을 중단시키면 안 된다.

**검증 질문:** 대량 Spot relocation 중 control Spot requests가 계속 완료되는가.

- 시작 조건: 여러 SpotWide Spot과 별도 control Spot을 준비한다.
- 절차: Bounded concurrency로 relocations와 control requests를 함께 실행한다.
- 검증: Control request loss와 duplicate가 없고 relocation 뒤 각 Spot의 Actor state checksum이 유지된다.
- 계약 근거: [Runtime metrics](../spec/25-runtime-metrics.ko.md)

#### ST-I4 Message Follow authority boundaries

우선순위: `P1`

Message Follow는 이동 완료 직전·직후의 old-route message를 다루며 current global route의 정상 처리를 바꾸지
않아야 한다.

**검증 질문:** 경계별 old-route message가 최대 한 번 처리되고 current-route message가 항상 target에 도달하는가.

- 시작 조건: Proxy에서 old-route messages를 이동 전, completion 직후, expiry 뒤로 나누어 보관한다.
- 절차: 각 message를 해당 경계에 전달하고 global Actor ID request도 함께 보낸다.
- 검증: Old-route operation은 계약된 기간 안에서만 최대 한 번 처리된다. Global route operation은 target에서 한 번 처리된다.
- 계약 근거: [Spot actor](../spec/15-spot-actor.ko.md)

#### ST-I5 Message Follow error bounds

우선순위: `P1`

Follow route는 deadline이 지난 request, duplicate와 forwarding loop를 새 업무처럼 처리해서는 안 된다.

**검증 질문:** Expired·duplicate·loop 입력이 handler 중복 없이 bounded terminal로 끝나는가.

- 시작 조건: Proxy가 동일 operation과 expired deadline, A↔B old route를 재현한다.
- 절차: 각 입력을 별도 operation으로 전달한다.
- 검증: 각 caller는 terminal 하나를 받고 operation ID handler count는 최대 한 번이다. Current-route follow-up은 정상 성공한다.
- 계약 근거: [Framework error model](../spec/32-framework-error-model.ko.md)

#### ST-I6 Multi-hop relocation

우선순위: `P1`

A→B→C처럼 연속 이동해도 current route는 마지막 owner를 가리키고 오래된 route chain이 message를 반복 전달하면
안 된다.

**검증 질문:** Multi-hop 완료 뒤 old A·B route message가 중복되지 않고 global request가 C에서 처리되는가.

- 시작 조건: Actor nodes A, B, C와 짧은 Message Follow duration을 준비한다.
- 절차: A→B와 B→C Join을 완료하고 A·B old route messages와 global ID request를 보낸다.
- 검증: Global request는 C에서 한 번 처리된다. Old-route operation은 각각 최대 한 번이며 Follow 만료 뒤에는 처리되지 않는다.
- 계약 근거: [Spot actor](../spec/15-spot-actor.ko.md)

## 4. 완료 기준

- 모든 Join·Relocate·message·bind operation은 역할 server가 public Framework API로 시작한다.
- Client result, public Actor·Spot ref, lifecycle callback·handler evidence와 client push만 통과 판정에 사용한다.
- Temporary queue, Location Store row, relocation payload와 Session route update packet은 직접 검사하지 않는다.
- Accepted operation은 terminal 하나를 가지며 handler operation ID는 최대 한 번 나타난다.
- Fixed sleep, 정확한 scheduler 시점, 내부 retry 횟수와 작은 표본의 latency를 통과 조건으로 사용하지 않는다.
- 지원 언어의 public API가 부족하면 scenario를 우회하지 않고 feature map에 contract gap을 기록한다.
