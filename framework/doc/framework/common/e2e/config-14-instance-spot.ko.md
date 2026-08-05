<!-- framework-adapter-nav:start -->
[E2E 목차](README.ko.md) | [이전: One-way submit admission](config-13-submit-admission.ko.md)
<!-- framework-adapter-nav:end -->

# Config 14 — Instance Spot activation

Instance Spot은 Application이 global Spot ID로 처음 message를 보낼 때 필요한 Spot을 만든다. Caller는 owner
node를 선택하거나 Spot을 먼저 만들지 않는다. Framework는 공개 Instance intent를 확인하고 적합한 node에
Spot 하나를 준비한 뒤 첫 message부터 같은 Spot의 execution queue에서 처리한다.

이 config는 이 공개 동작이 여러 process와 실제 Store를 사용하는 배포에서도 유지되는지 검증한다. Location
Store row, activation barrier, claim token, recovery cursor와 Core frame은 판정에 사용하지 않는다. Application
factory·handler·lifecycle callback이 실행된 결과는 역할 server의 public evidence endpoint로 확인한다.

## 1. 확인 범위

- Missing global Spot ID에 Instance intent를 지정한 첫 request와 send
- Concurrent first call이 Instance Spot 하나로 수렴하는 동작
- First message와 후속 message의 처리 순서
- Capacity, stable type, initial Mesh와 기존 owner routing
- Crash, Store 장애, deadline과 relocation 뒤의 공개 결과
- Instance Spot에서 허용하지 않는 Actor와 Logical Multicast 기능
- 서로 다른 Framework 언어 사이의 같은 payload와 terminal 의미

일반 Spot direct call은 existing-only다. Instance intent가 없는 Missing Spot 호출은 Spot을 만들지 않는다.
Instance Spot 생성은 Spot manager의 public Create·GetOrCreate 기능으로 노출하지 않는다.

## 2. 배포 구성과 판정 방법

| 역할 | 수 | 하는 일 |
|---|---:|---|
| Instance caller | 2 | 서로 다른 process에서 public Spot request·send를 시작한다. |
| Instance owner | 2 | 같은 stable type의 Instance factory와 packet·timer handler를 제공한다. |
| User Spot owner | 1 | 같은 ID에 다른 kind의 Spot을 만드는 경합과 existing-only 회귀를 검증한다. |
| Location Store | 1 | Global Spot location과 node 상태를 제공한다. |
| Relocation Store | 1 | Instance relocation과 activation 복구에 필요한 Framework state를 보존한다. |
| External state store | 1 | Application domain state를 보존한다. |
| E2E runner | 1 | Process와 network·Store 장애만 제어하고 Framework operation은 역할 server의 public endpoint로 시작한다. |

각 factory와 handler는 Spot ID, operation ID, 실행 순서, process ID와 domain state version을 Application
state에 기록한다. E2E는 client result, public Spot 조회·status와 이 Application evidence만 사용한다.
Readiness는 public startup evidence를 bounded polling하여 확인한다. Fixed sleep으로 lease나 복구 완료를
추정하지 않는다.

## 3. Scenario

### Track A — 첫 call과 기본 routing

#### IS-E2E-01 Cold request

우선순위: `P0`

Missing Spot에 Instance intent를 지정한 첫 request는 Spot 준비와 첫 업무 처리를 하나의 public operation으로
제공해야 한다.

**검증 질문:** Caller가 Spot을 미리 만들지 않아도 첫 request가 한 번 처리되고 reply를 받는가.

- 시작 조건: 두 owner가 stable type을 제공하며 해당 Spot ID는 public 조회에서 Missing이다.
- 절차: Caller A가 Instance intent와 함께 request를 한 번 보낸다.
- 검증: Factory와 request handler가 각각 한 번 실행되고 reply의 Spot ID와 operation ID가 입력과 같다. 이후 public 조회는 Ready Spot ref를 반환한다.
- 계약 근거: [Spot 주소 메시징](../spec/16-spot-address-messaging.ko.md), [Location runtime](../spec/21-location-runtime.ko.md)

#### IS-E2E-02 Cold send

우선순위: `P0`

One-way send의 성공은 Framework가 message를 전송 경로에 수락했다는 뜻이다. Remote handler 완료를 기다린다는
뜻은 아니다.

**검증 질문:** Cold send가 public send 의미로 완료되고 수락된 message가 최종 owner에서 한 번 처리되는가.

- 시작 조건: 대상 Spot은 Missing이고 owner factory의 진행을 Application gate로 지연할 수 있다.
- 절차: Gate를 닫고 Instance intent send를 호출한 뒤 send result를 확인하고 gate를 연다.
- 검증: Send는 handler 완료 전 성공할 수 있으며, gate 해제 뒤 handler evidence가 한 건 생긴다. Activation 실패를 주입한 별도 입력은 이미 반환한 send result를 바꾸지 않는다.
- 계약 근거: [비동기 실행 정책](../spec/05-async-execution-policy.ko.md), [Spot messaging](../spec/12-spot-messaging.ko.md)

#### IS-E2E-03 Concurrent first call

우선순위: `P0`

서로 다른 caller가 같은 Missing ID를 동시에 호출해도 Application에는 Spot 하나만 보여야 한다.

**검증 질문:** Concurrent requests가 factory 하나와 serial handler 하나로 수렴하는가.

- 시작 조건: 같은 stable type을 제공하는 owner 두 개가 Ready다.
- 절차: Caller A와 B가 같은 Spot ID에 고유 operation ID request를 충분한 수로 동시에 보낸다.
- 검증: 모든 성공 reply가 같은 Spot identity를 가리키며 factory 실행은 한 번이다. 모든 operation ID는 중복 없이 한 번씩 처리되고 handler active count는 1을 넘지 않는다.
- 계약 근거: [Spot 주소 메시징](../spec/16-spot-address-messaging.ko.md)

#### IS-E2E-04 Different Spot ID

우선순위: `P1`

서로 다른 Instance Spot은 각자의 execution queue를 사용하므로 한 Spot의 handler가 다른 Spot의 처리를 막지
않아야 한다.

**검증 질문:** Spot A handler가 대기하는 동안 Spot B request가 완료되는가.

- 시작 조건: 서로 다른 ID의 Instance Spot A와 B가 Ready다.
- 절차: A handler를 Application gate에서 대기시키고 B에 request를 보낸다.
- 검증: Gate를 열기 전에 B reply를 받고, A와 B evidence의 Spot ID가 섞이지 않는다.
- 계약 근거: [Spot messaging](../spec/12-spot-messaging.ko.md)

### Track B — owner 상실과 다시 활성화

#### IS-E2E-05 Ready owner crash는 자동 takeover로 이어지지 않는다

우선순위: `P0`

Ready owner가 종료되면 Framework는 다른 node를 새 owner로 선택하거나 같은 ID의 새 incarnation을 자동으로
만들지 않는다. 새 request는 Application이 명시적으로 Close와 recreate를 완료하기 전까지 사용할 수 없다.

**검증 질문:** 이전 owner 종료 뒤 새 request가 bounded `Unavailable`로 끝나고 다른 owner에서 실행되지
않는가.

- 시작 조건: Spot이 owner A에서 Ready이고 domain state는 external store에 저장되어 있다. 다른 node B는
  같은 type을 제공하지만 이 Spot의 owner가 아니다.
- 절차: A를 crash하고 public liveness·owner lease 상태가 invalid가 된 뒤 request를 보낸다. Location Store
  authority record가 자동으로 삭제되는지는 이 scenario의 판정 대상이 아니다.
- 검증: Request는 `Unavailable`로 한 번 끝난다. A와 B 어느 쪽에도 해당 operation ID의 handler·factory
  evidence가 없고 Framework가 자동으로 새 generation을 만들지 않는다. 명시적 recreate와 rebind는
  `IS-E2E-08`과 Application 복구 시나리오에서 별도로 확인한다.
- 계약 근거: [Failure와 failover](../spec/31-failure-failover-policy.ko.md)

#### IS-E2E-06 Creating owner crash는 같은 generation의 recovery 경계를 지킨다

우선순위: `P0`

첫 request를 처리할 Spot이 아직 `Ready`가 되지 않은 상태에서 owner가 종료되면 Framework는 같은 generation의
생성 record를 계속 사용하거나 정확히 취소할 수 있다. 이 recovery는 Ready owner failover와 다르다.

**검증 질문:** Factory 진행 중 owner crash가 기존 request를 한 번 끝내고 후속 request가 다른 generation이나
stale owner와 섞이지 않는가.

- 시작 조건: Factory가 Application gate에서 대기한다.
- 절차: 첫 request를 시작해 factory 진입을 확인하고 owner를 crash한다. 이전 call의 terminal을 받은 뒤 새 request를 보낸다.
- 검증: 이전 request는 success 또는 정식 failure 중 하나로 한 번 끝난다. 같은 generation의 recovery가
  계속되면 후속 request는 그 activation에 합류하고, 생성이 취소되어 `Missing`이 되면 그때 새 activation을
  시작한다. Stale owner의 handler evidence와 generation 혼합은 없어야 한다.
- 계약 근거: [Location runtime](../spec/21-location-runtime.ko.md)과
  [Failure와 failover](../spec/31-failure-failover-policy.ko.md)

#### IS-E2E-07 Normal Relocate

우선순위: `P0`

정상 relocation은 같은 Instance identity와 domain state를 target owner로 옮기며 처리 중인 message를 중복
실행하지 않아야 한다.

**검증 질문:** Public Relocate 완료 뒤 후속 request가 target에서 복원된 state를 사용하는가.

- 시작 조건: Spot은 A에서 Ready이고 state version을 조회할 수 있다.
- 절차: Public host relocation을 B로 시작하고 terminal success를 기다린 뒤 state request를 보낸다.
- 검증: 후속 handler는 B에서만 실행되고 Spot identity와 state version이 유지된다. Relocation 전 수락된 operation ID도 전체 evidence에서 한 번만 처리된다.
- 계약 근거: [Location runtime](../spec/21-location-runtime.ko.md)과
  [Graceful drain과 handoff](../spec/28-graceful-drain-handoff.ko.md)

#### IS-E2E-08 Close and reactivate

우선순위: `P1`

Spot을 명시적으로 닫은 뒤 같은 ID에 Instance intent call을 보내면 이전 runtime object를 재사용하지 않고 새
instance를 준비해야 한다.

**검증 질문:** Close 완료 뒤 첫 call이 새 factory instance에서 처리되는가.

- 시작 조건: Spot이 Ready이고 factory instance ID를 조회할 수 있다.
- 절차: Public close operation의 완료를 기다린 뒤 같은 ID로 Instance request를 보낸다.
- 검증: 새 factory instance ID는 이전 값과 다르고 handler는 새 instance에서 한 번 실행된다.
- 계약 근거: [Location runtime](../spec/21-location-runtime.ko.md)

#### IS-E2E-09 Ready owner crash 뒤 concurrent request를 자동 전환하지 않는다

우선순위: `P0`

Ready owner가 무효화된 뒤 여러 caller가 동시에 요청해도 Framework는 새 owner와 factory를 자동으로 만들지
않는다.

**검증 질문:** Crash 뒤 concurrent requests가 각각 bounded failure로 끝나고 자동 takeover가 없는가.

- 시작 조건: 이전 owner crash가 public liveness·owner lease 상태에서 무효화되었고 Application recreate는
  실행하지 않았다.
- 절차: 두 caller가 같은 ID로 requests를 동시에 보낸다.
- 검증: 두 request가 각각 `Unavailable`로 한 번 끝나고 어떤 owner에도 handler·factory evidence가 없다.
- 계약 근거: [Failure와 failover](../spec/31-failure-failover-policy.ko.md)

#### IS-E2E-10 Stale owner resume 뒤에도 자동 owner가 생기지 않는다

우선순위: `P0`

오래 중지되었던 이전 owner가 다시 실행되어도 Application이 recreate하지 않은 동안 message를 처리해서는
안 된다. 이 scenario에는 자동으로 만들어진 current owner를 두지 않는다.

**검증 질문:** 이전 owner 재개 뒤 request가 `Unavailable`로 끝나고 stale owner가 처리하지 않는가.

- 시작 조건: Spot은 A에서 Ready이고 A를 pause한 뒤 public owner lease가 무효화된다. B는 같은 type을 제공하지만
  이 Spot을 자동으로 소유하지 않는다.
- 절차: A를 resume하고 고유 operation ID request와 timer 관찰 request를 보낸다.
- 검증: Request는 `Unavailable`로 끝나고 A와 B 어느 쪽에도 신규 handler·timer evidence가 없다.
- 계약 근거: [Failure와 failover](../spec/31-failure-failover-policy.ko.md)

### Track C — 실패 결과와 재제출 경계

#### IS-E2E-11 Confirmed not admitted

우선순위: `P1`

Target이 request를 수락하지 않았다는 결과는 caller에게 하나의 failure로 전달되어야 한다.

**검증 질문:** 확정된 admission failure가 다른 owner의 handler 실행 없이 한 번 반환되는가.

- 시작 조건: 선택 가능한 target이 request admission을 거부하도록 공개 capacity를 구성한다.
- 절차: 고유 operation ID request를 한 번 보낸다.
- 검증: Request는 계약된 failure 한 번으로 끝나고 모든 owner의 handler evidence에는 operation ID가 없다.
- 계약 근거: [Framework error model](../spec/32-framework-error-model.ko.md)

#### IS-E2E-12 Ambiguous result

우선순위: `P1`

Target 수락 직후 connection이 끊기면 Framework는 같은 request를 다른 owner에서 몰래 다시 실행하지 않아야 한다.

**검증 질문:** Connection failure가 발생해도 operation ID의 handler 실행 수가 최대 한 번인가.

- 시작 조건: Handler가 operation ID를 durable application state에 기록한다.
- 절차: Target이 request를 수락한 직후 network proxy로 connection을 종료한다.
- 검증: Caller는 reply 또는 하나의 terminal failure만 받고, 모든 owner를 합친 handler 실행 수는 최대 한 번이다.
- 계약 근거: [Framework error model](../spec/32-framework-error-model.ko.md)

#### IS-E2E-13 Accepted send then failure

우선순위: `P1`

Send 성공 뒤 target이 종료되어도 Framework가 다른 owner에서 같은 one-way message를 replay한다는 보장은 없다.

**검증 질문:** Accepted send가 target failure 뒤 다른 owner에서 중복 처리되지 않는가.

- 시작 조건: Send operation ID를 handler evidence로 확인할 수 있다.
- 절차: Send 성공 직후 target을 종료하고 대체 owner를 준비한다.
- 검증: 전체 owner에서 operation ID 처리 수는 0 또는 1이며 2가 되지 않는다.
- 계약 근거: [비동기 실행 정책](../spec/05-async-execution-policy.ko.md)

#### IS-E2E-14 Store outage

우선순위: `P0`

Location Store에서 current owner를 확인할 수 없으면 Framework는 local 추측으로 Missing Spot을 만들거나 오래된
route에 신규 message를 보내서는 안 된다.

**검증 질문:** Store 장애 중 신규 request가 handler 실행 없이 bounded failure로 끝나는가.

- 시작 조건: Spot을 Ready로 만든 뒤 Location Store 접근을 차단한다.
- 절차: Cached location의 유효 기간이 끝난 뒤 request를 보낸다.
- 검증: Request는 계약된 Store·route failure로 끝나고 모든 owner의 handler 실행 수는 0이다.
- 계약 근거: [Location runtime](../spec/21-location-runtime.ko.md), [Framework error model](../spec/32-framework-error-model.ko.md)

#### IS-E2E-15 Kind·type atomic conflict

우선순위: `P0`

같은 global Spot ID는 동시에 서로 다른 Spot kind나 stable type이 될 수 없다.

**검증 질문:** User Spot 생성과 Instance cold request의 경합에서 한 종류만 성공하는가.

- 시작 조건: 해당 ID는 Missing이고 User Spot owner와 Instance owners가 Ready다.
- 절차: User Spot GetOrCreate와 다른 type의 Instance request를 동시에 시작한다.
- 검증: 한 operation만 성공하고 public 조회의 kind·type은 성공한 operation과 같다. 실패한 쪽 factory와 handler는 실행되지 않는다.
- 계약 근거: [Spot 주소 메시징](../spec/16-spot-address-messaging.ko.md)

#### IS-E2E-16 No eligible node

우선순위: `P0`

Stable type을 제공하는 node가 없거나 모든 capacity가 소진되면 caller가 원인을 구분할 수 있는 공개 failure를
받아야 한다.

**검증 질문:** Type 미제공과 capacity 소진이 각각 계약된 terminal로 끝나는가.

- 시작 조건: Type 미제공 topology와 capacity 0 topology를 별도 실행으로 준비한다.
- 절차: 각 topology에서 같은 형태의 cold request와 send를 호출한다.
- 검증: Request와 send가 각 조건에 맞는 terminal을 반환하고 factory·handler evidence는 없다.
- 계약 근거: [Framework error model](../spec/32-framework-error-model.ko.md)

#### IS-E2E-17 Activation backpressure

우선순위: `P0`

Cold activation을 동시에 여러 개 시작해도 public activation concurrency 설정을 넘겨 factory와
initialization을 실행하지 않아야 한다. 이 scenario는 내부 waiter capacity를 공개 설정으로 가정하지 않는다.

**검증 질문:** public activation concurrency가 1일 때 factory 동시 실행 수가 1을 넘지 않고 각 request가
bounded terminal을 가지는가.

- 시작 조건: 언어별 public configuration interface로 activation concurrency를 `1`로 설정하고 factory를
  Application gate에서 대기시킨다.
- 절차: 서로 다른 Spot ID에 대한 유한한 cold requests를 동시에 시작하고 gate를 한 번에 하나씩 연다.
- 검증: 동시에 실행 중인 factory·initialize는 application evidence에서 항상 1개 이하이며 각 request는
  reply 또는 정식 failure 하나로 끝난다. Requests의 admission 순서나 내부 waiter 수는 판정하지 않는다.
- 계약 근거: [Framework API §5](../spec/06-framework-api.ko.md)와
  [비동기 실행 정책](../spec/05-async-execution-policy.ko.md)

#### IS-E2E-18 Cross-language

우선순위: `P1`

Caller와 owner의 구현 언어가 달라도 typed payload, reply와 failure 의미는 같아야 한다.

**검증 질문:** 지원 언어 조합이 같은 cold request와 failure case를 같은 결과로 해석하는가.

- 시작 조건: 서로 다른 Framework 언어의 caller와 owner가 같은 public contract와 stable type을 등록한다.
- 절차: 각 방향 조합에서 성공 request와 type 미제공 request를 실행한다.
- 검증: 성공 payload와 reply가 같고 failure category도 같다. 별도 raw frame이나 test adapter를 사용하지 않는다.
- 계약 근거: [Public contract governance](../spec/00-public-contract-governance.ko.md)

### Track D — ordering과 concurrency

#### IS-E2E-19 Ready ordering

우선순위: `P0`

Cold first message는 Spot을 만들게 한 업무 입력이다. Spot 준비 중 도착한 후속 message가 이를 추월하면 안 된다.

**검증 질문:** First request가 후속 requests보다 먼저 handler에서 처리되는가.

- 시작 조건: Factory를 Application gate에서 지연할 수 있다.
- 절차: First request를 시작하고 factory 진입을 확인한 뒤 후속 messages를 보내고 gate를 연다.
- 검증: Handler evidence의 첫 operation ID는 first request이며 나머지는 수락 순서를 유지한다.
- 계약 근거: [Spot messaging](../spec/12-spot-messaging.ko.md)

#### IS-E2E-20 Closing owner crash

우선순위: `P1`

Close가 authority release를 완료하기 전에 owner가 종료되면 이전 owner가 다시 release하거나 신규 업무를
처리해서는 안 된다. 이 경우에도 Framework가 다른 owner를 자동으로 만들지 않는다.

**검증 질문:** Closing owner crash 뒤 다음 call이 `Unavailable`로 끝나고 새 factory가 실행되지 않는가.

- 시작 조건: Close callback을 Application gate에서 지연하여 authority release 전에 owner를 crash할 수 있게
  한다.
- 절차: Close가 시작된 뒤 owner를 crash하고 public liveness·owner lease 상태가 invalid가 된 뒤 Instance
  request를 보낸다.
- 검증: 새 request는 `Unavailable`로 한 번 끝나고 어떤 owner에도 새 factory·handler evidence가 없다.
  명시적 Close 완료 뒤 새 activation을 시작하는 흐름은 `IS-E2E-08`에서 확인한다.
- 계약 근거: [Failure와 failover](../spec/31-failure-failover-policy.ko.md)

#### IS-E2E-21 Multi-Mesh initial placement

우선순위: `P0`

Initial Mesh 선택은 Spot이 없을 때 어디에서 처음 만들지 정한다. 이미 Ready인 Spot을 다른 Mesh로 옮기는 요청은
아니다.

**검증 질문:** Cold call의 Mesh 선택은 적용되고 Ready 뒤 다른 Mesh를 지정한 call은 current owner로 가는가.

- 시작 조건: Mesh A와 B가 같은 type을 제공하고 Spot은 Missing이다.
- 절차: Mesh A를 지정해 cold request를 보낸 뒤 Mesh B를 지정한 후속 request를 보낸다.
- 검증: 두 handler 모두 최초 owner에서 실행되고 factory는 한 번이다.
- 계약 근거: [Spot 주소 메시징](../spec/16-spot-address-messaging.ko.md)

#### IS-E2E-22 Monotonic owner deadline

우선순위: `P0`

Owner process가 긴 시간 중지되었다가 재개되면 이전에 계산한 local 유효 상태로 신규 업무를 처리하면 안 된다.

**검증 질문:** Deadline 뒤 재개한 owner가 message와 timer handler를 실행하지 않고 request가 `Unavailable`로
끝나는가.

- 시작 조건: Spot은 A에서 Ready이고 A를 process pause할 수 있다. 다른 node는 이 Spot을 자동으로 소유하지
  않는다.
- 절차: A를 pause하고 public liveness·owner lease 상태에서 lease가 무효화된 뒤 resume한다. 그 후
  message와 timer 관찰 request를 보낸다.
- 검증: Resume 뒤 A와 다른 node의 신규 message·timer evidence는 0건이고 caller operation은
  `Unavailable`로 끝난다.
- 계약 근거: [Failure와 failover](../spec/31-failure-failover-policy.ko.md)

#### IS-E2E-23 Handler capability

우선순위: `P1`

Instance Spot은 Actor membership과 Logical Multicast subscription을 제공하지 않는다. 잘못된 factory 구성은
업무 message를 받기 전에 드러나야 한다.

**검증 질문:** 금지된 handler capability를 등록한 type의 cold request가 application handler 실행 없이 실패하는가.

- 시작 조건: Negative type factory가 public registration API로 금지된 capability를 구성한다.
- 절차: 해당 type의 Instance request를 보낸다.
- 검증: Request는 configuration failure로 끝나고 packet handler와 Actor lifecycle callback은 실행되지 않는다.
- 계약 근거: [Spot actor](../spec/15-spot-actor.ko.md)

#### IS-E2E-24 Late Store response

우선순위: `P0`

Store 응답이 operation deadline 뒤 도착해도 만료된 request의 handler를 뒤늦게 실행해서는 안 된다.

**검증 질문:** 늦은 location 응답 뒤에도 request가 timeout 하나로 끝나고 handler가 실행되지 않는가.

- 시작 조건: Network proxy가 Location Store response를 request deadline보다 길게 지연한다.
- 절차: 짧은 deadline request를 보내고 timeout 뒤 proxy를 복구한다.
- 검증: Caller는 timeout 한 번만 받고 해당 operation ID의 factory·handler evidence는 없다.
- 계약 근거: [Framework error model](../spec/32-framework-error-model.ko.md)

#### IS-E2E-25 Activation completion failure

우선순위: `P1`

Factory는 끝났지만 Spot을 사용 가능하게 만드는 마지막 단계가 실패하면 caller에게 Ready Spot으로 보이면 안 된다.

**검증 질문:** Activation completion failure 뒤 public 조회가 Ready를 반환하지 않고 다음 call이 정상적으로 수렴하는가.

- 시작 조건: Owner의 application initialization callback이 한 번 실패하도록 구성한다.
- 절차: 첫 request의 terminal을 확인한 뒤 failure 설정을 제거하고 같은 ID로 다시 요청한다.
- 검증: 첫 handler는 실행되지 않고 첫 request는 한 번 실패한다. 다음 request는 factory와 handler 한 번으로 성공한다.
- 계약 근거: [Location runtime](../spec/21-location-runtime.ko.md)

#### IS-E2E-26 Concurrent claim

우선순위: `P0`

서로 다른 target에 같은 cold call이 도착하더라도 Application object는 한 target에만 만들어져야 한다.

**검증 질문:** Network 경합에서도 factory와 handler가 owner 하나에서만 실행되는가.

- 시작 조건: 두 owner가 같은 type과 capacity를 제공한다.
- 절차: 두 caller가 같은 ID의 first requests를 동시에 보낸다.
- 검증: Factory evidence는 한 owner에만 한 건이고 모든 성공 handler evidence도 그 owner에만 있다.
- 계약 근거: [Location runtime](../spec/21-location-runtime.ko.md)

#### IS-E2E-27 Deadline isolation

우선순위: `P0`

같은 activation을 기다리는 operation도 각 caller의 deadline을 독립적으로 지켜야 한다.

**검증 질문:** 짧은 request만 timeout되고 긴 request와 send는 계속 처리되는가.

- 시작 조건: Factory gate 지연이 짧은 deadline보다 길고 긴 deadline보다 짧다.
- 절차: 짧은 request, 긴 request와 send를 같은 Spot에 순서대로 시작한 뒤 gate를 연다.
- 검증: 짧은 request는 timeout, 긴 request는 reply를 받는다. Send와 긴 request의 operation ID는 각각 한 번 처리된다.
- 계약 근거: [Framework error model](../spec/32-framework-error-model.ko.md)

#### IS-E2E-28 Close·admission 경쟁

우선순위: `P1`

Close가 시작된 Spot은 새 업무를 기존 instance queue에 수락해서는 안 된다.

**검증 질문:** Close와 동시에 보낸 request가 이전 handler에서 처리되지 않는가.

- 시작 조건: Close callback 진입을 public Application evidence로 확인할 수 있다.
- 절차: Close 진입 직후 고유 operation ID request를 보낸다.
- 검증: 이전 instance의 handler에는 operation ID가 없고 request는 하나의 failure 또는 close 뒤 새 instance의 한 번 처리로 끝난다.
- 계약 근거: [Location runtime](../spec/21-location-runtime.ko.md)

### Track E — relocation 경합과 복구

#### IS-E2E-29 Cross-Mesh in-flight Relocate

우선순위: `P1`

다른 Mesh로 relocation 중인 Spot에 도착한 message도 한 owner의 queue에서 한 번만 처리되어야 한다.

**검증 질문:** Relocate와 concurrent request가 중복 없이 terminal 하나로 끝나는가.

- 시작 조건: Spot은 Mesh A에서 Ready이고 Mesh B가 compatible target을 제공한다.
- 절차: B로 Relocate를 시작하고 동시에 고유 operation ID request를 보낸다.
- 검증: Relocate와 request가 각각 terminal 하나로 끝나며 request handler는 A 또는 B 한 곳에서만 한 번 실행된다.
- 계약 근거: [Location runtime](../spec/21-location-runtime.ko.md)과
  [Graceful drain과 handoff](../spec/28-graceful-drain-handoff.ko.md)

#### IS-E2E-30 Multi-Mesh concurrent Relocate

우선순위: `P1`

같은 Spot을 서로 다른 target으로 동시에 옮기려는 요청은 owner를 둘로 만들면 안 된다.

**검증 질문:** Concurrent Relocate operations 뒤 public 조회가 owner 하나를 반환하는가.

- 시작 조건: Source와 compatible targets 두 개가 Ready다.
- 절차: 서로 다른 target을 지정한 Relocate operations를 동시에 시작한다.
- 검증: 각 operation은 terminal 하나를 받고 최종 public 조회는 Ready owner 하나다. 후속 request handler도 그 owner에서만 실행된다.
- 계약 근거: [Location runtime](../spec/21-location-runtime.ko.md)

#### IS-E2E-31 Remote selection loser

우선순위: `P1`

Cold activation target 경합에서 선택되지 않은 target은 별도 Application instance를 만들거나 request를 처리하면
안 된다.

**검증 질문:** 경합 뒤 factory·handler evidence가 final owner 한 곳에만 존재하는가.

- 시작 조건: 두 target에 동일한 type과 weight를 구성하고 같은 Spot ID는 `Missing`으로 둔다.
- 절차: 여러 caller가 같은 Spot ID에 서로 다른 operation ID로 cold requests를 동시에 시작한다.
- 검증: Public 조회의 owner와 factory·handler evidence가 일치하며 선택되지 않은 target의 factory count는
  0이다.
- 계약 근거: [Spot 주소 메시징](../spec/16-spot-address-messaging.ko.md)

#### IS-E2E-32 Activation crash boundary

우선순위: `P0`

Cold activation 중 process가 종료되어도 다음 request가 영구 대기하거나 같은 first operation을 중복 처리하면
안 된다. 생성이 `Ready`가 되기 전에는 같은 generation의 recovery가 계속되거나 정확히 취소될 수 있다.

**검증 질문:** Source 또는 target crash 뒤 기존 call이 terminal 하나로 끝나고 후속 call이 같은 generation
recovery 또는 명시적 새 activation 경계 안에서 처리되는가.

- 시작 조건: Source call 전과 target factory 진입을 구분해 crash할 수 있다.
- 절차: 두 경계를 별도 실행으로 crash하고 기존 call terminal 뒤 후속 request를 보낸다.
- 검증: 각 기존 call은 reply 또는 failure 하나로 끝난다. 같은 generation recovery가 계속되면 후속 request가
  그 activation에 합류하고, 생성이 취소되어 `Missing`이 되면 다음 call이 새 activation을 시작한다. Ready
  owner crash를 다른 owner의 자동 처리로 해석하지 않으며 모든 operation ID의 처리 수는 최대 한 번이다.
- 계약 근거: [Failure와 failover](../spec/31-failure-failover-policy.ko.md)

#### IS-E2E-33 Cold activation failure release

우선순위: `P0`

Factory나 initialize가 실패한 Spot ID는 보이지 않는 failed instance에 고정되어서는 안 된다.

**검증 질문:** 실패 원인을 제거한 다음 call이 새 factory에서 성공하는가.

- 시작 조건: Factory failure와 initialize failure를 별도 type 또는 입력으로 재현할 수 있다.
- 절차: 각 실패 request의 terminal을 확인하고 failure를 제거한 뒤 같은 ID로 다시 요청한다.
- 검증: 실패한 operation의 handler 실행은 0이고 terminal은 하나다. 후속 request는 factory와 handler 한 번으로 성공한다.
- 계약 근거: [Framework error model](../spec/32-framework-error-model.ko.md)

#### IS-E2E-34 Unpublished activation cleanup

우선순위: `P1`

Owner가 factory를 완료하기 전에 종료된 activation은 다음 call의 payload나 result와 섞이면 안 된다.

**검증 질문:** Crash 뒤 새 payload가 이전 activation의 payload와 섞이지 않고 recovery 경계를 지키는가.

- 시작 조건: Factory가 payload A와 함께 진입 evidence를 남긴 뒤 Application gate에서 대기한다.
- 절차: Factory 진입을 확인한 뒤 target을 crash하고 request terminal을 확인한다. Target role을 다시
  시작한 뒤 같은 Spot ID에 payload B를 포함한 public Instance intent request를 보낸다. Public resolve와
  factory·handler application evidence를 함께 수집한다.
- 검증: 같은 generation recovery가 계속되면 A가 recovery root에서 한 번 처리된 뒤 B가 이어지고, 첫
  activation이 취소되어 public resolve가 Ready object를 반환하지 않으면 B만 새 activation에서 처리된다.
  A와 B payload가 합쳐지거나 B 대신 오래된 A가 새 operation의 reply에 사용되지 않는다.
- 계약 근거: [Location runtime](../spec/21-location-runtime.ko.md)

#### IS-E2E-35 Ready owner crash 뒤 queue를 자동 복구하지 않는다

우선순위: `P0`

Spot이 이미 `Ready`가 된 뒤 owner가 종료되면 Framework는 first request와 follow-up을 다른 owner에서
자동으로 replay하지 않는다.

**검증 질문:** Crash 뒤 각 operation이 terminal 하나로 끝나고 다른 owner에서 handler가 실행되지 않는가.

- 시작 조건: Public lookup이 Spot을 `Ready`로 반환한 뒤 first handler 시작 사이를 Application gate로
  넓힌다. Owner는 이미 Ready다.
- 절차: First와 follow-up requests를 보낸 뒤 gate 구간에서 owner를 crash하고 restart한다. Application
  recreate는 실행하지 않는다.
- 검증: 각 caller는 terminal 하나를 받으며, crash 뒤 다른 owner의 handler·factory evidence는 없다. 이후
  messaging은 명시적 Close와 recreate 전까지 `Unavailable`로 끝난다.
- 계약 근거: [Failure와 failover](../spec/31-failure-failover-policy.ko.md)

#### IS-E2E-36 First handler terminal recovery

우선순위: `P0`

First handler가 시작된 상태에서 owner가 종료되면 Framework가 같은 operation을 반드시 재실행한다고 가정할 수
없다. 다만 다른 owner에서 중복 실행하거나 call을 무기한 유지해서는 안 된다.

**검증 질문:** Handler 시작 전·후 crash에서 caller terminal이 하나이고 다른 owner의 자동 replay가 없는가.

- 시작 조건: Handler 진입과 domain commit을 별도 Application evidence로 기록한다.
- 절차: Handler 진입 직전과 진입 뒤를 별도 실행으로 crash하고 caller와 후속 request 결과를 확인한다.
- 검증: 각 caller는 reply 또는 failure 하나로 끝난다. 각 operation ID의 domain commit은 최대 한 번이고
  다른 owner의 handler에는 해당 ID가 없다. 후속 request는 명시적 recreate·rebind 전까지 `Unavailable`로
  끝난다.
- 계약 근거: [Framework error model](../spec/32-framework-error-model.ko.md)

## 4. Reference sample 확인

### 4.1 GameQuest {#71-gamequest}

GameQuest는 Player ID를 global Spot ID로 사용하고 quest message에 public Instance intent를 지정한다. Sample
code가 owner node를 선택하거나 Spot manager로 Instance Spot을 먼저 만들면 안 된다.

- 서로 다른 Quest node에 동시에 도착한 첫 message가 factory 하나와 serial handler 하나로 수렴한다.
- 같은 Player ID의 gameplay send와 progress request가 first-message 순서를 유지한다.
- Ready owner crash 뒤 Framework가 후속 call을 current owner에서 자동 처리하지 않는다. Application은
  external state를 사용해 새 generation을 recreate하고 Session을 다시 bind해야 한다.
- Sample E2E는 client reply, handler operation ID와 domain state만 판정하며 activation 내부 상태를 읽지 않는다.

### 4.2 ShoppingMall {#72-shoppingmall}

ShoppingMall은 Order ID를 global Spot ID로 사용하고 workflow message에 public Instance intent를 지정한다.
Caller는 Instance address, owner node와 activation phase를 다루지 않는다.

- 첫 start request가 runtime Instance와 domain workflow를 각각 한 번 만든다.
- Runtime Spot이 없고 domain workflow가 있는 주문은 external state에서 복구한 뒤 command를 처리한다.
- Runtime과 domain workflow가 모두 없는 ID의 continue·rebuild는 빈 workflow를 성공으로 만들지 않는다.
- Close 뒤 다음 유효 command는 새 factory instance에서 처리되며 이전 operation을 중복 실행하지 않는다.

## 5. 완료 기준

- 36개 scenario가 역할 server의 public Framework API로 operation을 시작한다.
- 통과 판정은 client result, public Spot 조회·status와 Application factory·handler·callback evidence만 사용한다.
- Fixed sleep, Store record, private activation phase, raw frame와 internal counter를 통과 조건으로 사용하지 않는다.
- 지원 언어가 scenario를 구현하지 못하면 skip으로 완료하지 않고 feature map에 public contract gap을 기록한다.
- 최소 두 Framework 언어의 caller·owner 조합이 성공 payload와 terminal failure를 같은 의미로 해석한다.
