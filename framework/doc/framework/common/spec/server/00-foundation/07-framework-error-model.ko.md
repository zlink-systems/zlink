---
title: "Framework 오류 모델"
---

# Framework 오류 모델

[Foundation 주제 목차](README.ko.md) · [스펙 목차](../README.ko.md) · [이전: 06. Framework API](06-framework-api.ko.md) · [다음: 08. 계층 경계와 식별자](08-layering.ko.md)

> `Send`, `Request`와 lifecycle operation이 실패했을 때 Application에 전달하는 공통 오류
> `ErrorKind`, Send·Request의 완료·실패 경계와 재시도 판단 규칙을 정의한다.

## 1. 범위

이 문서는 Framework의 `Send`, `Request`, lifecycle operation이 실패했을 때 Application에
전달하는 공통 오류를 정의한다. 오류는 내부 함수나 state machine 단계가 아니라 Application이
구분해야 하는 실패 종류를 나타낸다.

Framework는 오류와 함께 재시도 여부를 제공하지 않는다. Application은 operation의 완료
조건, idempotency와 업무 상태를 확인한 뒤 새 operation을 시작할지 결정한다.

## 2. 공통 ErrorKind

다섯 언어는 다음 이름과 숫자를 해당 언어의 enum naming convention으로 투영한다. 값 `0`도
유효한 오류 값이다.

| 값 | Kind | 의미 |
|---:|---|---|
| 0 | `NotFound` | Actor, 주소와 상태를 가진 논리 instance인 [Spot](02-glossary.ko.md#spot), handler, route 또는 target이 존재하지 않는다. |
| 1 | `AlreadyExists` | 같은 identity나 registration이 이미 존재한다. |
| 2 | `TypeMismatch` | Stable type과 요청한 Application type이 다르다. |
| 3 | `NotConfigured` | 필요한 role, handler 또는 Store가 등록되지 않았다. |
| 4 | `Rejected` | Typed operation 결과가 없는 Framework admission, filter 또는 runtime policy가 operation을 거부했다. |
| 5 | `Unavailable` | Target, route, Store 또는 worker를 현재 사용할 수 없다. |
| 6 | `CapacityExceeded` | Placement, queue 또는 bounded resource에 여유가 없다. |
| 7 | [`DeadlineExceeded`](02-glossary.ko.md#deadlineexceeded) | Operation이 정한 deadline 안에 완료되지 않았다. |
| 8 | `ShuttingDown` | Runtime이 신규 operation을 받지 않는다. |
| 9 | `ProtocolError` | Wire, payload 또는 reply 계약을 처리할 수 없다. |
| 10 | `InvalidOperation` | 현재 object, session 또는 runtime 상태에서 operation을 실행할 수 없다. |
| 11 | `DataLost` | 공개된 Relocation payload가 없거나 검증에 실패했다. |
| 12 | `InternalFailure` | 위 분류로 표현할 수 없는 Framework 실패다. |

Generation, owner fence, moving phase, worker queue 상태와 Relocation 처리 단계는 내부
원인이다. Application이 별도 대응을 선택할 필요가 없으면 새 public kind로 노출하지 않고
log와 trace에 기록한다.

다섯 server package와 HTTP client package는 이 13개 kind를 공유한다. 언어별 interface
문서는 enum 이름, exception과 result 표현만 정의하며 kind를 추가하거나 재시도 boolean을
추가하지 않는다.

## 3. 호출 전에 확인할 수 있는 오류

잘못된 인자와 이미 종료된 handle처럼 호출 위치에서 바로 확인할 수 있는 문제는 각 언어의
표준 argument 또는 invalid-operation 오류로 전달한다. Startup configuration 오류도 언어별
configuration exception으로 전달한다. 이런 오류를 remote error reply로 바꾸지 않는다.

Outbound queue 수락, route resolve 또는 remote reply를 기다리는 중에 확인한 Framework
실패는 언어별 Framework exception이나 `result`의 `ErrorKind`로 전달한다.

## 4. Send 완료와 실패

`Send`는 source runtime의 outbound queue가 message를 수락하면 결과값 없이 완료된다. 이
시점은 target handler가 message를 처리했다는 뜻이 아니다.

| 완료 전에 확인한 조건 | 결과 |
|---|---|
| Logical target이나 route가 존재하지 않음 | `NotFound` |
| Connection 또는 current owner를 현재 사용할 수 없음 | `Unavailable` |
| Send timeout까지 outbound queue가 message를 수락하지 않음 | `DeadlineExceeded` |
| Runtime이 신규 admission을 중단함 | `ShuttingDown` |

`Send`가 완료된 뒤 target activation, admission 또는 handler 실행이 실패해도 이미 완료된
call의 결과를 바꾸지 않는다. Framework는 이 실패를 metric, log와 message-flow trace로
기록하며 같은 message를 다른 target에 자동으로 제출하지 않는다.

## 5. Request 완료와 실패

`Request`는 typed reply를 받으면 정상 완료된다. 정상 reply를 만들 수 없으면 다음
`ErrorKind` 중 하나로 한 번만 완료한다.

- 대상이나 handler가 없으면 `NotFound`다.
- Route, connection 또는 current owner를 사용할 수 없으면 `Unavailable`이다.
- Reply를 deadline 안에 받지 못하면 `DeadlineExceeded`다.
- Wire, payload 또는 reply type을 처리할 수 없으면 `ProtocolError`다.
- Runtime이 종료 중이면 `ShuttingDown`이다.
- 위 종류로 표현할 수 없는 Framework 실행 실패는 `InternalFailure`다.

`CapacityExceeded`와 `Unavailable`은 모두 자원 부족을 나타내지만 서로 다른 자원을
가리킨다.

- **`CapacityExceeded`는 source가 소유한 local bounded resource를 확보하지 못했다는
  뜻이다.** Reply를 보관할 자리, operation table entry, 같은 runtime 안의 Spot·Actor
  대기열이 이 자원이다 — 제출하는 쪽과 대기열이 같은 process에 있으므로 source가
  소유한다.
- **`Unavailable`은 다른 node의 대기열이 가득 차서 실패했다는 뜻이다.** Target의 대기열
  상태는 `CapacityExceeded`로 표현하지 않는다. 두 kind를 나누는 기준은 "실패한 자원을
  이 runtime이 소유하는가"이며, 호출자는 이 구분으로 재시도 대상을 판단한다.
- **이 구분은 대기열에만 적용한다.** Target node의 배치 수용량이 부족한 경우는 대기열이
  아니라 admission 판정이므로 `CapacityExceeded`가 맞다
  ([Spot Actor](../03-spot-actor/05-spot-actor-membership.ko.md), [Spot 주소 메시징](../03-spot-actor/06-spot-address-messaging.ko.md)).
- **Actor나 Spot이 relocation된 뒤에도 이전 owner node에 도착한 message를 새 owner에게
  대신 전달하는 동작인 [Message Follow](02-glossary.ko.md#message-follow) relay queue와
  relocation ingress hold에는 relocation 자체가 정하는
  record 수나 byte 상한이 없다.**
  - 이 queue나 hold에 보관한 양이 늘었다는 이유만으로 `CapacityExceeded`를 반환하지 않는다.
  - 단일 message에 협상된 크기 상한, transport, deadline과 cancellation이 정하는 제한은
    그대로 적용한다.
  - 보관한 work를 일반 application execution lane이 수락한 뒤에는 그 lane의 reservation을
    적용하지만, 이 reservation을 relay queue나 hold의 보관 상한으로 사용하지 않는다.
  - 이 제한 때문에 실패하면, 실패한 자원을 어느 runtime이 소유하는지에 따라 위 규칙으로
    오류를 정한다
    ([Spot Actor](../03-spot-actor/05-spot-actor-membership.ko.md), [위치 runtime](../05-location-relocation/01-location-runtime.ko.md)).

Cancellation은 각 언어의 cancelled awaitable로 전달한다. `DeadlineExceeded`와 cancellation은
호출자가 reply를 기다리지 않게 되었다는 뜻이다. Remote handler가 실행되지 않았다는 뜻이
아니며, 뒤늦게 도착한 reply로 두 번째 결과를 만들지 않는다.

## 6. Typed 결과와 Rejected

Actor create나 join처럼 계약에 `Accepted`와 `Rejected`가 있는 operation은 Application
callback의 판정을 typed 결과로 반환한다. 이때 `Rejected`는 Framework exception이 아니다.

공통 `ErrorKind.Rejected`는 typed 결과가 없는 filter, admission 또는 runtime policy가
operation을 거부했을 때만 사용한다. Application의 업무 규칙이 거부한 모든 결과를 Framework
exception으로 변환하지 않는다.

## 7. 재시도 판단

Public exception, error object와 typed failure에는 `RetryAdvice`, `isRetriable`,
`retriable` 같은 재시도 hint를 넣지 않는다. 같은 `ErrorKind`라도 operation이 이미
실행되었을 가능성과 중복 영향은 서로 다를 수 있기 때문이다.

Application이 새 operation을 시작하려면 다음을 직접 확인한다.

1. 이전 operation의 완료 조건과 remote 실행 가능성을 확인한다.
2. Operation이 idempotent한지, 또는 idempotency key로 중복 영향을 막는지 확인한다.
3. 필요한 경우 업무 상태를 다시 조회한 뒤 새 operation을 시작한다.

하나의 binding operation 안에서 Core가 소유하는 HWM 재시도는 Application retry가 아니다.
Framework는 send-ready waiter를 두지 않고 같은 operation을 다른 logical target에 자동
제출하지 않는다.

## 8. Application job queue 포화

Framework host instance가 application callback 시작 전까지 보유하는 공유 supply permit
queue인 [Application job queue](02-glossary.ko.md#application-job-queue)의 Manual queue 값이
`1..2,147,483,647` 밖이거나 계산 overflow이면 socket bind 전 configuration error다.

Runtime shared-cap 부족은 public error, typed reject나 drop 사유가 아니라 cancellable
wait다.

현재 Actor나 Spot을 실제로 실행하는 MeshNode인 [owner](02-glossary.ko.md#owner) 구조 한도
위반만 기존 owner error를 사용하며 두 조건을 섞지 않는다.

## 9. 검증 요구

공개 표면(각 언어 `ErrorKind` enum과 그 숫자 값, `Send`·`Request`의 반환값·exception, typed
`Rejected` 결과)만으로 다음을 확인한다. 각 항목은 contract test 하나로 이어진다.

**ErrorKind 값과 개수**

- 각 언어의 13개 `ErrorKind`와 숫자가 일치한다.

**Send 완료 경계**

- `Send`는 source outbound queue 수락 시 완료되고, 이후 remote 실패로 결과가 바뀌지 않는다.

**Request 완료 경계**

- `Request`는 timeout과 cancellation 뒤 늦게 도착한 reply로 두 번째 결과를 만들지 않는다.

**Typed Rejected 구분**

- Typed `Rejected` 결과와 `ErrorKind.Rejected` exception이 구분된다.

**재시도 hint 부재**

- Public 오류 표면(exception, error object, typed failure)에 재시도 hint가 없다.

---

[Foundation 주제 목차](README.ko.md) · [스펙 목차](../README.ko.md) · [이전: 06. Framework API](06-framework-api.ko.md) · [다음: 08. 계층 경계와 식별자](08-layering.ko.md)
