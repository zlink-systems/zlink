<!-- framework-adapter-nav:start -->
[E2E 목차](README.ko.md) | [이전: Monitoring](config-7-monitoring.ko.md) | [다음: Actor 메시징](config-9-to-actor-messaging.ko.md)
<!-- framework-adapter-nav:end -->

# Config 8 — Async, Yield와 실행 turn

Spot과 Actor callback은 자기 execution lane에서 한 번에 하나씩 실행된다. Request나 worker 결과를
`Async`로 기다리면 현재 turn을 유지하고, `SpotWide` User Spot 또는 Instance Spot에서 `Yield`로 기다리면
공유 Spot gate를 잠시 반납한다. `Yield`로 반납해도 member Actor의 FIFO claim은 유지하므로 같은 Actor의
다음 message가 먼저 실행되지는 않는다.

이 config는 실제 remote request, timer, Actor mailbox와 worker를 함께 실행하여 이 차이를 application
evidence로 검증한다. Scheduler thread ID, private queue와 test-only dispatch hook은 사용하지 않는다.

## 1. 확인 범위

- One-way submit, `Async`와 `Yield`의 완료·turn 의미
- Spot 상태, timer와 continuation의 실행 순서
- I/O worker와 CPU worker의 execution lane 분리
- `SpotWide`, `PerActor`와 Actor FIFO의 조합
- Handler에서 등록한 deferred Actor Join
- Remote topology, Session relay, timeout·cancellation·Shutdown 결과
- 언어별 public 표현이 달라도 유지해야 하는 실행 의미

## 2. 배포 구성

| 역할 | 수 | 하는 일과 분리 이유 |
|---|---:|---|
| Location Store | 1 | 두 Object Server와 Session gateway가 같은 Spot·Actor 위치를 사용하게 한다. |
| Play node | 2 | `SpotWide`·`PerActor` User Spot, Entry Spot과 Actor를 제공한다. Counter, timer와 callback sequence를 application evidence로 기록한다. |
| Delay service | 2 | Public Channel request를 받은 뒤 application signal 또는 지정 deadline에 reply한다. Await 구간을 결정적으로 만든다. |
| External API | 1 | Framework 밖의 HTTP server다. I/O worker가 실제 외부 I/O를 기다리는 조건을 만든다. |
| Session gateway | 1 | Stream Session과 Actor binding을 제공하여 Session relay로 시작한 Actor callback을 검증한다. |
| E2E client | 1 | 역할 server의 public application endpoint와 Stream endpoint만 호출한다. |

Application evidence는 operation ID, callback 종류와 시작·완료 sequence를 기록한다. `turn id`나
`mailbox id`처럼 Framework 내부 identity를 새 public evidence로 만들지 않는다. 동시 실행 여부는 handler가
application counter를 증가·감소시켜 기록한 active count와 start/end 순서로 확인한다.

## 3. 공통 실행과 판정 방법

Runner는 scenario마다 Spot·Actor ID, operation ID와 evidence state를 새로 만든다. Await 구간은 fixed sleep이
아니라 delay service와 handler의 application signal로 연다. Timer의 due 경계를 검증할 때만 public timer
설정과 monotonic timestamp를 사용하며 runner tolerance를 더한다.

각 scenario는 역할 server의 health와 public target readiness를 확인한 뒤 시작한다. Request terminal과
handler evidence를 함께 확인하고, file log와 scheduler timing은 성공 조건으로 사용하지 않는다.

## 4. Scenario

### Track A — Async는 현재 turn을 유지

#### TD-A1 One-way send 완료는 handler 완료를 기다리지 않는다

우선순위: `P0`

One-way send는 outbound admission까지만 기다린다. Remote handler가 끝날 때까지 send가 pending이면 request와
같은 완료 의미가 된다.

**검증 질문:** Remote handler가 application signal에서 대기해도 send가 먼저 완료되는가.

- 시작 조건: Delay handler가 marker를 받은 뒤 release signal까지 reply 없이 대기하도록 구성한다.
- 절차: Play node가 delay Channel로 one-way send를 제출한다. Send terminal을 확인한 뒤 handler evidence를
  읽고 release signal을 보낸다.
- 검증: Send는 handler release 전에 정상 terminal을 반환한다. Handler는 marker를 한 번 처리한다.
- 세부 동작: [오류 모델 §4](../spec/32-framework-error-model.ko.md)의 send 완료를
  검증한다.

#### TD-A2 Async 대기 중 같은 Spot의 다음 callback을 시작하지 않는다

우선순위: `P0`

`Async`는 handler가 끝날 때까지 Spot turn을 유지한다. 같은 Spot의 다음 callback이 끼어들면 await 전후의
상태를 안전하게 사용할 수 없다.

**검증 질문:** Async request가 대기하는 동안 같은 Spot의 probe callback이 시작되지 않는가.

- 시작 조건: `TurnProbeSpot` counter가 0이고 delay request는 release signal을 기다린다.
- 절차: 첫 handler가 delay request를 `Async`로 기다리는 상태를 public evidence로 확인한다. 같은 Spot에
  probe request를 보내고 delay reply를 해제한다.
- 검증: Evidence 순서는 `async-held, async-resumed, async-completed, probe-started,
  probe-completed`다. Active callback count는 1을 넘지 않는다.
- 세부 동작: [비동기 실행 정책 §1.1](../spec/05-async-execution-policy.ko.md)을
  검증한다.

#### TD-A3 Async 구간의 read-modify-write를 보존한다

우선순위: `P0`

Turn을 유지하면 여러 handler가 같은 Spot counter를 읽고 기다리더라도 한 handler가 완료된 뒤 다음
handler가 값을 읽는다.

**검증 질문:** Concurrent client request N개가 Async 구간을 거쳐 counter를 정확히 N만큼 증가시키는가.

- 시작 조건: Counter가 0인 Spot과 각 operation ID별 delay reply가 준비되어 있다.
- 절차: 서로 다른 request N개를 동시에 시작하고 각 handler가 counter를 읽은 뒤 Async request를
  기다리게 한다. Delay reply를 모두 해제한다.
- 검증: 모든 request가 reply 하나를 받고 최종 counter는 N이다. Handler active count는 항상 1이다.
- 세부 동작: [비동기 실행 정책 §2](../spec/05-async-execution-policy.ko.md)의 serial turn을
  검증한다.

#### TD-A4 Async turn과 remote completion은 서로 막지 않는다

우선순위: `P0`

Spot turn을 유지하는 동안에도 transport reply와 infrastructure completion은 처리되어야 한다. 그렇지
않으면 모든 Async request가 자기 turn 때문에 deadlock에 빠진다.

**검증 질문:** Spot turn을 유지한 remote request가 reply를 받고 정상 재개되는가.

- 시작 조건: Delay service가 request를 받으면 즉시 public evidence를 남기고 release signal 뒤 reply한다.
- 절차: Spot handler가 request를 Async로 기다린다. Remote 수신을 확인한 뒤 reply signal을 보낸다.
- 검증: Handler가 reply를 받아 재개하고 deadline 전에 정상 완료한다. 같은 Spot의 다음 callback은 그 뒤
  실행된다.
- 세부 동작: [비동기 실행 정책 §3](../spec/05-async-execution-policy.ko.md)을
  검증한다.

#### TD-A5 Async 대기 중 due timer는 handler 뒤 실행된다

우선순위: `P1`

Timer callback도 같은 Spot turn을 사용한다. Async handler가 turn을 유지한 동안 due가 되어도 handler와
겹치지 않고 turn이 끝난 뒤 실행되어야 한다.

**검증 질문:** Async handler가 timer due 시각을 지나도록 대기하면 timer callback이 handler 완료 뒤
실행되는가.

- 시작 조건: One-shot timer를 등록하고 delay request는 application signal을 기다린다.
- 절차: Handler가 Async-held 상태가 된 뒤 timer due timestamp와 runner tolerance가 지난 것을 확인한다.
  Timer evidence가 아직 없음을 읽고 delay reply를 해제한다.
- 검증: Evidence 순서는 `async-held, async-completed, timer-started, timer-completed`이며 callback active
  count는 1을 넘지 않는다.
- 세부 동작: [비동기 실행 정책 §5](../spec/05-async-execution-policy.ko.md)을
  검증한다.

### Track B — Yield와 shared Spot gate 반환

#### TD-B1 Yield 대기 중 같은 Spot의 callback을 실행한다

우선순위: `P0`

`Yield`를 사용하는 이유는 remote 작업을 기다리는 동안 같은 Spot의 다른 업무를 진행하기 위해서다.

**검증 질문:** Yield request가 대기하는 동안 같은 Spot의 probe callback이 완료되는가.

- 시작 조건: `SpotWide` User Spot의 delay request가 release signal을 기다린다.
- 절차: 첫 handler가 Yield-held 상태가 된 뒤 같은 Spot에 probe request를 보낸다. Probe reply를 확인한 뒤
  delay reply를 해제한다.
- 검증: Evidence 순서는 `yield-released, probe-started, probe-completed, yield-resumed,
  yield-completed`다.
- 세부 동작: [비동기 실행 정책 §1.1](../spec/05-async-execution-policy.ko.md)을
  검증한다.

#### TD-B2 Yield continuation은 기존 queue 순서를 따른다

우선순위: `P0`

Remote reply가 도착해도 continuation을 현재 callback 안에 inline으로 실행하지 않는다. Shared gate의
queue에 들어가 앞서 대기한 callback 뒤에 재개되어야 한다.

**검증 질문:** 먼저 queue에 들어간 probe가 Yield continuation보다 먼저 완료되는가.

- 시작 조건: 첫 handler가 Yield-held 상태다. Probe 1 handler는 시작한 뒤 application signal에서
  대기하도록 구성한다.
- 절차: Probe 1과 Probe 2를 순서대로 제출한다. Probe 1이 실행 중인 것을 확인한 뒤 delay reply를
  해제하여 Yield continuation을 ready로 만든다. 마지막으로 Probe 1을 해제한다.
- 검증: Evidence 순서는 `probe-1-started, probe-1-completed, probe-2-completed, yield-resumed`다.
  Continuation과 probe의 active callback count는 겹치지 않는다.
- 세부 동작: [비동기 실행 정책 §3](../spec/05-async-execution-policy.ko.md)을
  검증한다.

#### TD-B3 Yield 뒤에는 shared state를 다시 확인한다

우선순위: `P1`

Yield 전에 읽은 Spot 상태는 다른 callback이 바꿀 수 있다. “Lost update가 우연히 발생하는가”를 통계로
검사하지 않고, 상태 변경 순서를 application signal로 고정하여 검증한다.

**검증 질문:** 첫 handler가 Yield한 동안 두 번째 handler가 바꾼 값을 continuation이 관찰하는가.

- 시작 조건: Counter가 10이고 첫 handler가 이 값을 읽은 뒤 Yield-held 상태가 된다.
- 절차: 두 번째 handler가 counter를 20으로 바꾸고 완료한 것을 확인한다. Delay reply를 해제하여 첫
  continuation을 재개한다.
- 검증: 첫 continuation은 current value 20을 다시 읽고 그 값을 기준으로 처리한다. Yield 전에 읽은 10을
  그대로 쓰지 않는다.
- 세부 동작: [비동기 실행 정책 §4](../spec/05-async-execution-policy.ko.md)을
  검증한다.

#### TD-B4 Yield 대기 중 timer callback을 실행한다

우선순위: `P0`

Yield는 shared Spot gate를 반납하므로 due timer가 remote request 대기 뒤로 밀리지 않아야 한다.

**검증 질문:** Yield-held 구간에 one-shot timer가 실행되는가.

- 시작 조건: `SpotWide` User Spot에 one-shot timer를 등록하고 delay reply는 보류한다.
- 절차: Handler가 Yield-held 상태가 된 뒤 timer evidence를 bounded polling한다. Timer 완료 뒤 delay
  reply를 해제한다.
- 검증: Evidence 순서는 `yield-released, timer-started, timer-completed, yield-resumed`다.
- 세부 동작: [비동기 실행 정책 §5](../spec/05-async-execution-policy.ko.md)을
  검증한다.

### Track C — Worker 종류와 Spot turn을 분리

#### TD-C1 I/O worker를 Yield로 기다린다

우선순위: `P0`

외부 HTTP I/O는 I/O worker에서 실행하고 worker call을 Yield로 기다리면 같은 Spot의 다른 callback과 timer가
진행할 수 있다.

**검증 질문:** External API 대기 중 probe와 timer가 완료된 뒤 I/O continuation이 재개되는가.

- 시작 조건: External API reply를 application signal로 보류하고 Spot timer를 등록한다.
- 절차: Spot handler가 `RunIoWorker`에서 HTTP request를 시작하고 worker call을 Yield로 기다린다. Probe와
  timer 완료를 확인한 뒤 HTTP reply를 해제한다.
- 검증: Probe와 timer가 I/O continuation보다 먼저 완료되고 HTTP 결과가 원래 handler reply에 포함된다.
- 세부 동작: [비동기 실행 정책 §6](../spec/05-async-execution-policy.ko.md)의 I/O worker를
  검증한다.

#### TD-C2 I/O worker를 Async로 기다리면 turn을 유지한다

우선순위: `P1`

Worker 종류가 turn 의미를 자동으로 정하지 않는다. 같은 I/O worker도 Async로 기다리면 현재 Spot turn을
유지한다.

**검증 질문:** I/O worker Async 대기 중 같은 Spot probe가 시작되지 않는가.

- 시작 조건: External API reply가 보류되어 있다.
- 절차: Spot handler가 I/O worker를 Async로 기다리고 같은 Spot에 probe를 제출한다. HTTP reply를
  해제한다.
- 검증: I/O handler가 완료된 뒤 probe가 시작된다. External API request 자체가 아니라 worker call의
  terminator가 turn을 결정한다.
- 세부 동작: [비동기 실행 정책 §6](../spec/05-async-execution-policy.ko.md)를 검증한다.

#### TD-C3 I/O 대기가 CPU worker capacity를 사용하지 않는다

우선순위: `P0`

비동기 I/O가 CPU worker thread를 점유하면 동시 외부 요청 수가 pool 크기를 넘을 때 불필요한 capacity
오류가 발생한다.

**검증 질문:** CPU pool 크기보다 많은 I/O worker request가 `CapacityExceeded` 없이 완료되는가.

- 시작 조건: External API가 동시 request를 application signal에서 보류하고 CPU pool 크기를 public
  configuration으로 고정한다.
- 절차: Pool 크기의 네 배에 해당하는 I/O worker operation을 시작한다. 모두 remote API에 도착한 것을
  확인한 뒤 reply를 해제한다.
- 검증: 모든 operation이 정상 reply를 하나씩 받고 `CapacityExceeded`가 없다. 다른 Spot probe도 대기
  중에 완료된다.
- 세부 동작: [비동기 실행 정책 §6](../spec/05-async-execution-policy.ko.md)의 worker pool 분리를
  검증한다.

#### TD-C4 CPU worker와 terminator 역할을 분리한다

우선순위: `P1`

CPU worker는 계산을 bounded pool로 옮기지만 같은 Spot의 진행 여부는 Async 또는 Yield가 정한다.

**검증 질문:** 같은 CPU 작업을 Async와 Yield로 기다릴 때 worker 결과는 같고 Spot callback 순서만
달라지는가.

- 시작 조건: CPU worker는 application signal까지 계산 완료를 보류한다.
- 절차: Async variant와 Yield variant에서 같은 계산을 실행하고 각각 같은 Spot probe를 제출한다.
- 검증: 두 variant의 계산 결과는 같다. Async에서는 worker handler 뒤 probe가 실행되고 Yield에서는
  probe가 worker continuation보다 먼저 완료된다. Pool limit을 넘긴 별도 batch는 public
  `CapacityExceeded`로 bounded하게 끝난다.
- 세부 동작: [비동기 실행 정책 §6](../spec/05-async-execution-policy.ko.md)를 검증한다.

#### TD-C5 CPU worker saturation이 I/O worker를 막지 않는다

우선순위: `P1`

CPU pool과 비동기 I/O 실행이 같은 제한을 공유하면 CPU 계산이 많은 동안 외부 I/O도 멈춘다.

**검증 질문:** CPU worker가 pool capacity를 모두 사용하는 동안 I/O worker request가 완료되는가.

- 시작 조건: CPU worker를 pool 크기만큼 application gate에서 대기시킨다.
- 절차: CPU workers가 모두 active인 것을 확인한 뒤 별도 Spot에서 external API를 I/O worker로 호출한다.
  I/O reply를 확인한 후 CPU gate를 해제한다.
- 검증: I/O operation은 CPU gate 해제 전에 정상 완료한다. CPU operation도 gate 해제 뒤 각자 terminal을
  하나만 반환한다.
- 세부 동작: [비동기 실행 정책 §6](../spec/05-async-execution-policy.ko.md)의 실행 자원 분리를
  검증한다.

### Track D — SpotWide와 PerActor lane을 구분

#### TD-D1 SpotWide Actor가 Yield하면 다른 Actor와 Spot callback이 진행한다

우선순위: `P0`

Member Actor의 Yield는 User Spot의 shared gate만 반납한다. 따라서 다른 Actor, Spot direct handler와 timer는
진행할 수 있다.

**검증 질문:** Actor A가 Yield-held인 동안 Actor B, Spot handler와 timer가 완료되는가.

- 시작 조건: Actor A와 B가 같은 `SpotWide` User Spot에 있고 A의 delay reply가 보류되어 있다.
- 절차: A handler가 Yield한 뒤 B request, Spot request와 one-shot timer를 실행한다. 모두 완료된 뒤 A
  reply를 해제한다.
- 검증: B, Spot과 timer evidence가 A continuation보다 먼저 나타나며 callback active count는 shared gate
  안에서 1을 넘지 않는다.
- 세부 동작: [비동기 실행 정책 §7](../spec/05-async-execution-policy.ko.md)을
  검증한다.

#### TD-D2 같은 Actor의 다음 record는 Yield continuation 뒤 실행한다

우선순위: `P0`

Actor가 Yield해도 자기 FIFO claim은 유지한다. 같은 Actor의 다음 message가 먼저 실행되면 Actor 상태
순서가 깨진다.

**검증 질문:** Actor A의 두 번째 request가 첫 Yield continuation과 handler 완료 뒤 시작되는가.

- 시작 조건: Actor A의 첫 handler가 Yield-held 상태다.
- 절차: 같은 Actor로 두 번째 request를 보낸 뒤 첫 delay reply를 해제한다.
- 검증: Evidence는 `job1-start, job1-yield, job1-resume, job1-end, job2-start` 순서이며 Actor handler
  active count는 1을 넘지 않는다.
- 세부 동작: [비동기 실행 정책 §7](../spec/05-async-execution-policy.ko.md)을
  검증한다.

#### TD-D3 Timer overrun 중 callback을 겹쳐 실행하지 않는다

우선순위: `P0`

Timer handler가 public async operation을 기다리는 동안에도 같은 timer key의 callback은 동시에 실행되지
않는다. 다음 due가 겹치면 Framework는 선택한 overrun policy에 따라 tick을 건너뛰거나 제한적으로 합치거나
다음 tick을 늦출 수 있지만 callback 실행 구간을 겹치게 만들면 안 된다.

**검증 질문:** 첫 timer handler가 비동기 operation을 기다리는 동안 다음 due가 같은 timer callback을 동시에
실행하지 않고 설정한 overrun policy를 따르는가.

- 시작 조건: 반복 timer의 period를 첫 callback이 기다리는 public async operation보다 짧게 설정하고, 첫
  callback의 operation completion을 application gate에서 보류한다. Application evidence에는 timer key,
  callback generation과 delivery index를 기록한다.
- 절차: 첫 callback 진입을 확인한 뒤 monotonic deadline으로 적어도 한 번의 due 경계를 지난 것을 확인하고
  operation completion gate를 해제한다. 이후 bounded wait로 같은 timer의 다음 evidence를 수집한다.
- 검증: 같은 timer key의 callback active count는 항상 1이다. 수집된 delivery·scheduled index는 선택한
  overrun policy의 skip, bounded catch-up 또는 delayed-next 규칙을 따른다. Timer를 다시 등록하거나
  취소한 뒤 이전 generation callback은 실행되지 않는다.
- 세부 동작: [비동기 실행 정책 §5](../spec/05-async-execution-policy.ko.md)을
  검증한다.

#### TD-D4 PerActor Async는 같은 Actor lane만 막는다

우선순위: `P0`

`PerActor` User Spot은 Actor마다 별도 FIFO lane을 사용한다. Actor A가 Async로 기다려도 Actor B와 별도
timer lane은 진행해야 한다.

**검증 질문:** Actor A가 Async-held인 동안 B와 timer는 완료되고 A의 다음 request만 대기하는가.

- 시작 조건: Actor A와 B가 같은 `PerActor` User Spot에 있고 A delay reply가 보류되어 있다.
- 절차: A가 Async-held가 된 뒤 A의 두 번째 request, B request와 서로 다른 timer 두 개를 실행한다.
  B와 timer evidence를 확인한 뒤 A reply를 해제한다.
- 검증: B와 timers는 A보다 먼저 완료되고 A의 두 번째 request는 첫 A handler 뒤 시작한다. 같은 lane의
  active count는 1을 넘지 않는다.
- 세부 동작: [비동기 실행 정책 §8](../spec/05-async-execution-policy.ko.md)을
  검증한다.

#### TD-D5 지원하지 않는 문맥의 Yield를 operation 제출 전에 거부한다

우선순위: `P0`

Yield는 shared gate를 반납할 수 있는 `SpotWide` User Spot과 Instance Spot에서만 의미가 있다.

**검증 질문:** Entry Spot, PerActor와 owner turn 밖에서 Yield를 호출하면 `InvalidOperation`인가.

- 시작 조건: 각 문맥에서 같은 remote request와 worker call을 시작할 public endpoint를 준비한다.
- 절차: Entry Spot, Entry Actor, `PerActor` Actor, Channel handler와 owner turn 밖 caller에서 Yield variant를
  각각 실행한다. 같은 call의 Async variant도 실행한다.
- 검증: Yield variants는 `InvalidOperation`으로 한 번 끝나고 remote handler evidence가 없다. Async
  variants는 정상 계약대로 실행된다.
- 세부 동작: [Framework API §12](../spec/06-framework-api.ko.md)의 context validation을
  검증한다.

#### TD-D6 같은 claim이 필요한 awaited request를 거부한다

우선순위: `P0`

현재 callback이 가진 claim을 동일 target이 필요로 하는데 그 request를 기다리면 구조적으로 완료할 수
없다. Framework는 timeout까지 기다리지 않고 제출 전에 거부한다.

**검증 질문:** Actor self-request와 same-gate Async request가 `InvalidOperation`으로 끝나는가.

- 시작 조건: Actor와 `SpotWide` handler가 자신과 같은 claim이 필요한 request를 시작하도록 구성한다.
- 절차: Actor self-request의 Async·Yield variant와 Spot same-gate Async variant를 실행한다. 대조로 self
  one-way send도 실행한다.
- 검증: Awaited requests는 `InvalidOperation`이고 target handler evidence가 없다. One-way send는 FIFO에
  수락되어 current handler 뒤 한 번 처리된다.
- 세부 동작: [비동기 실행 정책 §9](../spec/05-async-execution-policy.ko.md)를
  검증한다.

### Track E — Handler가 등록한 Actor Join을 terminal 뒤 시작

#### TD-E1 Entry Spot Actor의 deferred Join을 handler 뒤 실행한다

우선순위: `P0`

Handler에서 `Defer()`를 호출하면 Join intent만 등록한다. Membership 이동은 handler가 정상 종료한 뒤
시작해야 한다.

**검증 질문:** Handler terminal 전에는 Actor가 Entry Spot에 있고 terminal 뒤 target User Spot으로
이동하는가.

- 시작 조건: Actor가 Entry Spot에 있고 target User Spot은 같은 node에 있다. Handler terminal을
  application signal로 보류한다.
- 절차: Actor handler가 Join을 defer하고 `deferred` evidence를 남긴 뒤 대기한다. Public current Spot
  lookup으로 Actor가 Entry에 있음을 확인하고 handler를 해제한다.
- 검증: Handler terminal 전에는 Join callback이 없고 current Spot은 Entry다. 이후 target
  `OnActorJoin`, `OnJoinedActor`, source `OnLeaveActor`와 Actor completion callback이 각 한 번 실행되며
  current Spot은 target이다.
- 세부 동작: [Spot actor §4](../spec/15-spot-actor.ko.md)를 검증한다.

#### TD-E2 PerActor와 SpotWide에서 같은 deferred Join 의미를 사용한다

우선순위: `P0`

Source Spot의 execution mode가 달라도 Defer는 handler terminal까지 Actor claim을 유지한다. SpotWide에서
앞선 request가 Yield했다가 재개된 경우에도 마지막 handler terminal이 기준이다.

**검증 질문:** 두 execution mode 모두 handler terminal 뒤에만 Join callback을 시작하는가.

- 시작 조건: `PerActor`와 `SpotWide` source Spot에 Actor를 하나씩 두고 target Spot은 같은 node에 둔다.
- 절차: 두 handler가 Join을 defer한 뒤 application signal에서 대기한다. SpotWide variant는 앞서 한 번
  Yield하고 재개한 뒤 Defer한다. Terminal 전 current Spot을 확인하고 두 handler를 해제한다.
- 검증: Terminal 전에는 두 Actor 모두 source에 있다. Terminal 뒤에는 각 Actor의 target callback과
  completion callback이 한 번 실행되고 target current Spot으로 바뀐다.
- 세부 동작: [Spot actor §4](../spec/15-spot-actor.ko.md)를 검증한다.

#### TD-E2A Handler 실패 시 등록한 Join을 폐기한다

우선순위: `P0`

한 handler가 여러 Actor의 Join을 defer했더라도 handler가 exception 또는 cancellation으로 끝나면 아직
활성화하지 않은 intent를 모두 버려야 한다.

**검증 질문:** 실패한 handler가 defer한 두 Join이 모두 시작되지 않고 기존 membership을 유지하는가.

- 시작 조건: Source Spot에 Actor A와 B가 있고 handler가 두 Join을 차례로 defer한다.
- 절차: Exception variant와 cancellation variant를 각각 fresh fixture에서 실행한다. Handler terminal 뒤
  두 Actor에게 source Spot request를 보낸다.
- 검증: Target·source Join lifecycle callback과 Actor completion callback이 없다. Public current Spot은
  두 Actor 모두 source이며 후속 request를 정상 처리한다.
- 세부 동작: [비동기 실행 정책 §10](../spec/05-async-execution-policy.ko.md)의
  handler terminal을 검증한다.

#### TD-E3 반대 방향 local Join 두 개를 함께 진행한다

우선순위: `P0`

서로 다른 Actor와 Spot pair의 local Join을 node 전체에서 하나씩만 실행하면 관계없는 이동까지 막힌다.

**검증 질문:** A에서 B로 가는 Actor와 B에서 A로 가는 Actor가 모두 deadline 안에 이동하는가.

- 시작 조건: Actor X는 Spot A, Actor Y는 Spot B에 있고 두 handler terminal을 같은 application barrier로
  해제할 수 있다.
- 절차: 두 Actor handler에서 반대 방향 Join을 defer하고 둘 다 registered인 것을 확인한 뒤 barrier를
  해제한다.
- 검증: 두 completion callback이 Accepted이고 public current Spot이 서로 바뀐다. 각 Actor callback은 한
  번씩 실행되며 timeout이 없다.
- 세부 동작: [Spot actor §4](../spec/15-spot-actor.ko.md)의 Actor별 독립성을
  검증한다.

### Track F — Remote 경로와 terminal 실패에서도 같은 의미를 유지

#### TD-F1 Remote Spot request에서도 Async와 Yield 의미가 같다

우선순위: `P0`

Target Spot이 다른 node에 있어도 source Spot의 turn 관리 의미는 바뀌지 않는다.

**검증 질문:** Remote request의 Async variant는 probe를 막고 Yield variant는 probe를 진행시키는가.

- 시작 조건: Source Spot은 `play-a`, delay target Spot은 `play-b`에 ready다.
- 절차: TD-A2와 TD-B1의 application signal 절차를 remote Spot request로 각각 반복한다.
- 검증: Async evidence는 source handler 뒤 probe가 시작되고 Yield evidence는 probe 뒤 continuation이
  재개된다. 두 remote request는 reply 하나를 반환한다.
- 세부 동작: [비동기 실행 정책 §2](../spec/05-async-execution-policy.ko.md)을 검증한다.

#### TD-F2 Channel handler에서 시작해도 같은 의미를 사용한다

우선순위: `P1`

RouteMesh를 통해 호출된 Channel handler도 public request terminator의 의미를 바꾸지 않는다.

**검증 질문:** Channel handler가 Spot request를 기다릴 때 Async와 Yield의 context validation·순서가
계약과 일치하는가.

- 시작 조건: Remote caller와 Channel handler가 ready다.
- 절차: Handler에서 같은 Spot request의 Async와 Yield variant를 각각 실행한다.
- 검증: Channel handler처럼 Yield를 지원하지 않는 문맥은 `InvalidOperation`이고 remote Spot handler가
  실행되지 않는다. Async variant는 정상 reply를 반환한다.
- 세부 동작: [Framework API §12](../spec/06-framework-api.ko.md)을 검증한다.

#### TD-F3 Session relay로 시작한 Actor handler에서도 같은 의미를 사용한다

우선순위: `P1`

Actor packet이 Stream Session relay로 들어와도 Actor mailbox와 Spot gate 계약은 동일하다.

**검증 질문:** Bound Session이 시작한 Actor request에서도 SpotWide Yield와 Actor FIFO가 유지되는가.

- 시작 조건: Stream Session이 `SpotWide` User Spot의 Actor A에 bind되어 있다.
- 절차: Client가 relay packet을 보내 A handler를 Yield-held로 만든 뒤 같은 Session에서 A의 다음 packet과
  Actor B packet을 보낸다. Delay reply를 해제한다.
- 검증: B packet은 Yield 구간에 처리되고 A의 다음 packet은 첫 A handler 완료 뒤 처리된다.
- 세부 동작: [Session Actor dispatch §6](../spec/20-session-actor-dispatch.ko.md)와
  [비동기 실행 정책 §7](../spec/05-async-execution-policy.ko.md)을 검증한다.

#### TD-F4 Timeout 뒤 Spot turn을 반환한다

우선순위: `P0`

Awaited request가 timeout되어도 현재 turn이나 shared gate가 계속 잠긴 상태로 남아서는 안 된다.

**검증 질문:** Async와 Yield request가 `DeadlineExceeded` 뒤 후속 Spot request를 처리하는가.

- 시작 조건: Delay service가 해당 operation ID에 reply하지 않도록 구성한다.
- 절차: Async와 Yield variant를 fresh Spot에서 각각 실행하고 public deadline terminal을 기다린다. 이어서
  probe request를 보낸다.
- 검증: 두 variant는 `DeadlineExceeded` terminal 하나로 끝나고 probe는 정상 reply를 받는다.
- 세부 동작: [오류 모델 §5](../spec/32-framework-error-model.ko.md)를 검증한다.

#### TD-F5 Waiter cancellation 뒤 owner를 계속 사용한다

우선순위: `P1`

Caller가 await를 취소하는 것은 이미 remote에 수락된 operation이나 owner lifecycle을 취소한다는 뜻이
아니다.

**검증 질문:** Waiter cancellation 뒤 같은 Spot·Actor의 새 request가 정상 처리되는가.

- 시작 조건: Remote handler가 delay request를 수락하고 reply를 보류한다.
- 절차: Async 또는 Yield waiter를 public cancellation으로 끝낸 뒤 같은 owner에 새 request를 보낸다.
  마지막으로 remote reply를 해제한다.
- 검증: 첫 awaitable은 언어별 cancellation 결과 하나를 반환한다. Follow-up request는 정상 reply를 받고
  late reply가 새 operation을 완료하지 않는다.
- 세부 동작: [비동기 실행 정책 §3](../spec/05-async-execution-policy.ko.md)을
  검증한다.

#### TD-F5A Await 중 Host Shutdown을 시작한다

우선순위: `P1`

Shutdown은 신규 admission을 닫고 이미 수락한 callback을 host deadline 안에서 정리한다.

**검증 질문:** Shutdown seal 뒤 신규 operation은 거부되고 기존 await는 terminal 하나로 끝나는가.

- 시작 조건: Delay request가 remote에 수락되고 source handler가 await 중이다.
- 절차: Source host에 public Shutdown을 시작한다. Host status가 신규 작업을 받지 않는 상태가 된 뒤 같은
  owner에 새 request를 보내고 delay reply를 해제한다.
- 검증: 신규 request는 `ShuttingDown`이다. 기존 await는 reply 또는 shutdown deadline 결과 중 하나로 한
  번만 끝나며 Host는 bounded terminal state가 된다.
- 세부 동작: [Graceful drain §5](../spec/28-graceful-drain-handoff.ko.md)을 검증한다.

#### TD-F6 Wait-for cycle을 timeout 전에 거부한다

우선순위: `P1`

현재 Spot claim이 필요한 self-request를 Async로 기다리면 target handler가 시작할 수 없다.

**검증 질문:** Spot self-request Async가 `InvalidOperation`으로 끝나고 후속 request가 성공하는가.

- 시작 조건: Spot A handler가 A 자신에게 request를 시작하도록 구성한다.
- 절차: Self-request Async variant를 실행한 뒤 별도 caller가 A에 probe request를 보낸다.
- 검증: Self-request는 `InvalidOperation`이고 nested target handler evidence가 없다. Probe는 정상 reply를
  받는다.
- 세부 동작: [비동기 실행 정책 §9](../spec/05-async-execution-policy.ko.md)를
  검증한다.

### Track G — 언어 사이에서 같은 실행 의미를 확인

#### TD-G1 Cross-language source와 target도 같은 순서를 만든다

우선순위: `P0`

Terminator method 이름과 await 문법은 언어마다 달라도 process 사이에서 보이는 callback 순서는 같아야
한다.

**검증 질문:** 서로 다른 Framework 언어의 source·target 조합에서 Async와 Yield evidence가 같은가.

- 시작 조건: 최소 두 언어의 Play node와 delay service가 같은 packet contract로 ready다.
- 절차: 지원하는 양방향 언어 조합에서 TD-A2와 TD-B1을 실행한다.
- 검증: Async는 source handler 뒤 probe가 실행되고 Yield는 probe 뒤 continuation이 재개된다. Payload와
  terminal error 의미도 언어 조합마다 같다.
- 세부 동작: [Public contract governance](../spec/00-public-contract-governance.ko.md)의 언어 parity를
  검증한다.

## 5. 완료 기준

- 모든 scenario는 public request·send, timer, worker, Join과 역할 server의 application evidence만 사용한다.
- Await 구간은 application signal로 제어하고 고정 delay로 callback interleaving을 추정하지 않는다.
- Yield의 상태 변경은 “발생할 수 있다”는 확률 조건이 아니라 TD-B3의 결정적 state change로 검증한다.
- 같은 execution lane의 active callback count는 1을 넘지 않으며 request는 terminal 결과를 하나만 가진다.
- API shape, blocking call 검색과 private scheduler 검사는 E2E 완료 조건에 포함하지 않는다.
