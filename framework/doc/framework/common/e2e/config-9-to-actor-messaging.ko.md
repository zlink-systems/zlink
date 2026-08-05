<!-- framework-adapter-nav:start -->
[E2E 목차](README.ko.md) | [이전: 실행 turn과 terminator](config-8-execution-turn.ko.md) | [다음: Spot actor join/relocation](config-10-spot-actor-relocation.ko.md)
<!-- framework-adapter-nav:end -->

# Config 9 — ActorId로 직접 메시지 보내기

Server application은 Session을 거치지 않고 global `ActorId`로 Actor에게 send 또는 request를 보낼 수 있다.
이 호출은 Actor가 Session에 bind되어 있는지와 관계없이 current Ready Actor를 대상으로 한다. 또한 direct
message를 보냈다는 이유로 Session binding이 새로 생기거나 기존 binding이 바뀌어서는 안 된다.

이 config는 caller와 Actor가 서로 다른 process에 있는 배포에서 이 계약을 검증한다. E2E client는 역할
server의 application endpoint를 호출하고, 역할 server는 public Framework API로 operation을 실행한다.
Framework 내부 queue, location record와 private route 정보는 판정에 사용하지 않는다.

## 1. 확인 범위

- Session에 bind된 Actor와 bind되지 않은 Actor의 direct send·request
- Direct message와 이후 Session bind 사이의 독립성
- Session unbind 뒤 유지되는 Actor와 Actor 제거 뒤의 결과
- 존재하지 않는 Actor와 연결할 수 없는 owner의 오류 구분
- 같은 `ActorId`로 다시 만든 Actor에 대한 ID-only message와 이전 `ActorRef` lifecycle operation의 차이

## 2. 배포 구성

| 역할 | 수 | 하는 일과 분리 이유 |
|---|---:|---|
| Location Store | 1 | 두 Actor node, Session gateway와 caller server가 같은 global Actor 위치를 조회하도록 한다. 실행마다 전용 namespace를 사용한다. |
| Actor node | 2 | `to-actor.probe` Actor를 생성하고 direct send·request handler를 실행한다. 두 번째 node는 owner route를 사용할 수 없는 조건을 만드는 데 사용한다. |
| Session gateway | 2 | Stream Session을 수용하고 public binding API로 Actor를 bind·unbind한다. Actor가 보낸 bound-session push를 client에게 전달한다. |
| Caller server | 1 | Client의 HTTP 요청을 받아 public Actor client API로 global `ActorId` send·request를 시작한다. Session을 생성하거나 Actor를 bind하지 않는다. |
| E2E client | 1 | 역할 server의 public application endpoint와 Stream endpoint만 사용한다. Framework 내부 API를 직접 호출하지 않는다. |

Actor handler는 받은 packet 이름, request ID와 application payload를 application state에 기록한다. Session
gateway는 public binding 조회 결과를, Stream client는 실제로 받은 push payload를 evidence로 제공한다.
이 evidence는 역할 server의 public endpoint에서 조회하며 internal mailbox, binding token과 route record를
노출하지 않는다.

## 3. 공통 실행과 판정 방법

Runner는 scenario마다 process, Store namespace와 evidence marker를 새로 만든다. 역할 server의 health와
public RouteMesh status가 ready가 된 뒤 operation을 시작한다. Process 종료나 network 차단이 필요한
scenario에서만 runner가 외부 조건을 변경한다.

Send의 API 완료와 remote handler 실행은 구분한다. Send 호출 결과는 public send terminal로 확인하고,
실제 전달은 Actor handler의 application evidence로 확인한다. Request는 caller server가 받은 reply 또는
public error kind로 판정한다. File log는 실패 원인을 찾는 데만 사용한다.

## 4. Scenario

### Track A — Session binding과 direct message를 분리

#### TA-A1 Bind된 Actor에게 direct send·request를 보낸다

우선순위: `P0`

Actor가 이미 Session에 bind되어 있어도 server 간 direct message는 같은 Actor handler에서 처리되어야
한다. Direct message가 기존 binding을 바꾸면 이후 Actor push가 엉뚱한 client로 전달될 수 있다.

**검증 질문:** Bind된 Actor에게 direct send·request를 보내도 handler가 처리하고 기존 Session binding이
유지되는가.

- 시작 조건: Client가 `session-a`에 연결하고 `actor-bound`를 생성하여 bind한다. Session gateway의 public
  binding 조회에서 해당 Actor가 확인되고, Actor가 보낸 `BeforeNotify` push를 client가 받는다.
- 절차: Caller server가 `actor-bound`로 direct send와 request를 각각 한 번 보낸다. 처리가 끝난 뒤 Actor가
  bound-session API로 `AfterNotify`를 보낸다.
- 검증: Actor handler는 send와 request를 각각 한 번 처리하고 request는 입력 marker가 포함된 reply를
  반환한다. Public binding 조회 결과는 전후가 같으며 `BeforeNotify`와 `AfterNotify`는 처음 연결한 client만
  받는다.
- 세부 동작: [Actor model §5](../spec/14-actor-model.ko.md)와
  [Session Actor dispatch §4](../spec/20-session-actor-dispatch.ko.md)의 direct message와
  binding 분리를 검증한다.

#### TA-A2 Bind되지 않은 Actor에게 direct send·request를 보낸다

우선순위: `P0`

Direct Actor messaging은 Session binding을 전제로 하지 않는다. 이 경로가 binding을 요구하면 backend
작업이나 다른 server가 Actor를 직접 호출할 수 없다.

**검증 질문:** Bound Session이 없는 Actor도 direct send를 처리하고 direct request에 reply하는가.

- 시작 조건: Actor node에 `actor-unbound`를 생성한다. 두 Session gateway의 public binding 조회에는 이
  Actor가 없다.
- 절차: Caller server가 `actor-unbound`로 direct send와 request를 각각 한 번 보낸다.
- 검증: Actor handler가 두 message를 각각 한 번 처리하고 caller server가 request reply를 받는다. 실행
  뒤에도 Session binding은 생기지 않으며 Stream client가 받은 push도 없다.
- 세부 동작: [Actor model §2.3](../spec/14-actor-model.ko.md)과
  [§5](../spec/14-actor-model.ko.md)의 binding 독립성을 검증한다.

#### TA-A3 Direct message 뒤에 Session을 bind한다

우선순위: `P0`

Bind되지 않은 Actor가 direct message를 먼저 처리해도 Application은 나중에 그 Actor를 Session에 bind할
수 있어야 한다. 앞선 direct 호출이 암묵적인 binding을 남기면 명시적 bind 결과가 달라진다.

**검증 질문:** Direct message를 먼저 처리한 Actor를 이후 Session에 bind해도 두 경로가 서로 영향을
주지 않는가.

- 시작 조건: `actor-late-bind`를 생성하고 Session에는 bind하지 않는다.
- 절차: Caller server가 send와 request를 각각 한 번 보낸다. 처리가 확인되면 client가 `session-b`에
  연결하여 같은 Actor를 bind한다. Caller server가 다시 send와 request를 보내고, Actor는
  `LateBindNotify`를 bound Session으로 보낸다.
- 검증: Bind 전후의 direct send·request를 Actor handler가 각각 한 번 처리한다. Public binding 조회는
  bind 전에는 Actor를 반환하지 않고 bind 완료 뒤에는 `session-b`의 binding을 반환한다.
  `LateBindNotify`는 `session-b` client만 받는다.
- 세부 동작: [Session Actor dispatch §2](../spec/20-session-actor-dispatch.ko.md)의
  explicit bind와 direct message의 독립성을 검증한다.

#### TA-A4 Unbind 뒤에는 direct message가 계속되고 Actor 제거 뒤에는 실패한다

우선순위: `P0`

Session binding과 Actor lifecycle은 별개다. Binding만 해제했다면 Actor가 유지되는 동안 direct message는
계속 처리되어야 하고, Actor를 명시적으로 제거한 뒤에는 같은 호출이 성공해서는 안 된다.

**검증 질문:** Unbind 뒤에는 direct message가 성공하고 Actor를 제거한 뒤에는 `NotFound`로 끝나는가.

- 시작 조건: `actor-unbound-lifecycle`을 생성하여 Session에 bind한다. 이 scenario의 Application lifecycle
  정책은 unbind 때 Actor를 제거하지 않는다.
- 절차: Session gateway의 public API로 Actor를 unbind하고 binding이 없음을 확인한다. Caller server가
  send와 request를 각각 한 번 보낸다. 이후 public Actor lifecycle API로 Actor를 제거하고 같은
  `ActorId`로 새 request를 보낸다.
- 검증: Unbind 뒤의 두 message는 Actor handler에서 각각 한 번 처리된다. Actor 제거 뒤의 request는
  `NotFound`로 끝나며 handler evidence가 추가되지 않는다.
- 세부 동작: [Actor model §2.3](../spec/14-actor-model.ko.md)과
  [오류 모델 §2](../spec/32-framework-error-model.ko.md)의 lifecycle 분리를 검증한다.

### Track B — Logical target과 실패 결과를 구분

#### TA-B1 존재하지 않는 Actor를 호출한다

우선순위: `P0`

Global `ActorId`에 current Actor가 없으면 Framework가 임의로 Actor를 만들거나 message를 보관해서는 안
된다. Application은 대상 부재를 `NotFound`로 구분할 수 있어야 한다.

**검증 질문:** 존재하지 않는 `ActorId`의 direct send·request가 `NotFound`로 끝나고 handler가 실행되지
않는가.

- 시작 조건: 실행 namespace에서 한 번도 생성하지 않은 `actor-missing`을 사용한다.
- 절차: Caller server가 `actor-missing`으로 send와 request를 각각 한 번 시도한다.
- 검증: 두 operation의 public error kind는 `NotFound`다. 두 Actor node의 application evidence에는 해당
  Actor ID와 marker가 없다.
- 세부 동작: [오류 모델 §2](../spec/32-framework-error-model.ko.md)의 target 부재
  분류를 검증한다.

#### TA-B2 같은 ActorId로 다시 만든 Actor가 새 direct message를 처리한다

우선순위: `P0`

Application message의 target은 logical `ActorId`다. 따라서 Actor를 제거한 뒤 같은 owner에 같은 ID로
새 Actor를 만들면 이후 direct message는 새 Actor가 처리한다. 반면 이전 `ActorRef`는 특정 incarnation을
가리키므로 새 Actor의 lifecycle이나 binding을 변경해서는 안 된다.

**검증 질문:** 새 Actor가 ID-only message를 처리하면서 이전 `ActorRef`의 lifecycle operation은 거부되는가.

- 시작 조건: Actor node에 `actor-recreated`를 만들고 public API가 반환한 첫 `ActorRef`를 보관한다.
- 절차: 첫 `ActorRef`로 Actor를 제거한 뒤 같은 node에 같은 `ActorId`로 새 Actor를 생성한다. Caller
  server가 `ActorId`로 send와 request를 각각 한 번 보낸다. 그다음 보관한 이전 `ActorRef`로 bind 또는
  destroy를 시도한다.
- 검증: 새 Actor handler가 send와 request를 각각 한 번 처리하고 request reply를 반환한다. 이전
  `ActorRef`의 operation은 `InvalidOperation`으로 끝나며 새 Actor의 binding과 lifecycle은 바뀌지 않는다.
- 세부 동작: [Failover policy §4.1](../spec/31-failure-failover-policy.ko.md)의
  ID-only application message와 exact-reference control 구분을 검증한다.

#### TA-B3 Current owner에 연결할 수 없으면 Unavailable로 끝난다

우선순위: `P0`

Actor가 존재하더라도 caller에서 current owner로 message를 보낼 수 없는 동안에는 대상 부재가 아니라
현재 사용할 수 없는 상태다. Framework는 이 operation을 다른 Actor로 바꾸거나 내부에서 다시 제출하지
않는다.

**검증 질문:** Actor의 current owner route를 사용할 수 없을 때 request가 `Unavailable`로 끝나고, 연결
복구 뒤 새 request가 성공하는가.

- 시작 조건: `actor-route-down`을 `actor-b`에 생성한다. Caller server의 public status가 해당 route를
  ready로 보고 정상 대조 request가 성공한 상태다.
- 절차: Runner가 caller server와 `actor-b` 사이의 network를 차단하고 public status에서 route가 ready가
  아님을 확인한다. Caller server가 request를 한 번 보낸다. 차단을 해제하고 public status가 다시 ready가
  된 뒤 Application이 새 request를 보낸다.
- 검증: 차단 중 request는 `Unavailable`로 한 번 끝나고 Actor handler는 그 marker를 처리하지 않는다.
  Framework가 다른 Actor나 owner로 자동 전환한 evidence가 없어야 한다. 복구 뒤 새 request는 같은
  Actor가 한 번 처리하고 reply를 반환한다.
- 세부 동작: [Failover policy §2](../spec/31-failure-failover-policy.ko.md)와
  [오류 모델 §4](../spec/32-framework-error-model.ko.md)의 route 실패를 검증한다.

## 5. 완료 기준

- 모든 scenario는 역할 server의 public Framework API와 public application endpoint만 사용한다.
- Send 완료, remote handler 처리, request reply와 Session binding을 서로 다른 public evidence로 판정한다.
- `ActorId` direct message에는 `ObjectGeneration`을 target 조건으로 추가하지 않는다. Exact `ActorRef`를
  사용하는 lifecycle·binding operation에는 해당 reference의 generation 규칙을 적용한다.
- 고정 sleep으로 상태 전파를 추정하지 않는다. Health와 public status를 다음 operation의 시작 조건으로
  사용하며, 각 operation은 spec에 정한 timeout 안에서 terminal 결과를 하나만 가져야 한다.
- Actor direct messaging을 제공하는 모든 언어가 같은 scenario ID와 application marker를 사용한다.
