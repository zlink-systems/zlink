---
title: "Framework 오류 모델"
---

# Framework 오류 모델

[스펙 목차](README.ko.md) · [이전: 장애 대응과 failover 범위](31-failure-failover-policy.ko.md)

> **이 장이 정의하는 것** — `Send`, `Request`, lifecycle operation이 실패했을 때
> Application에 전달하는 공통 오류.

## 1. 범위

이 문서는 Framework의 `Send`, `Request`, lifecycle operation이 실패했을 때 Application에
전달하는 공통 오류를 정의한다. 오류는 내부 함수나 state machine 단계가 아니라 Application이
구분해야 하는 실패 종류를 나타낸다.

Framework는 오류와 함께 재시도 여부를 제공하지 않는다. Application은 operation의 완료 조건,
idempotency와 업무 상태를 확인한 뒤 새 operation을 시작할지 결정한다.

## 2. 공통 `ErrorKind`

다섯 언어는 다음 이름과 숫자를 해당 언어의 enum naming convention으로 투영한다. 값 `0`도
유효한 오류 값이다.

| 값 | Kind | 의미 |
|---:|---|---|
| 0 | `NotFound` | Actor, Spot, handler, route 또는 target이 존재하지 않는다. |
| 1 | `AlreadyExists` | 같은 identity나 registration이 이미 존재한다. |
| 2 | `TypeMismatch` | Stable type과 요청한 Application type이 다르다. |
| 3 | `NotConfigured` | 필요한 role, handler 또는 Store가 등록되지 않았다. |
| 4 | `Rejected` | Typed operation 결과가 없는 Framework admission, filter 또는 runtime policy가 operation을 거부했다. |
| 5 | `Unavailable` | Target, route, Store 또는 worker를 현재 사용할 수 없다. |
| 6 | `CapacityExceeded` | Placement, queue 또는 bounded resource에 여유가 없다. |
| 7 | `DeadlineExceeded` | Operation이 정한 deadline 안에 완료되지 않았다. |
| 8 | `ShuttingDown` | Runtime이 신규 operation을 받지 않는다. |
| 9 | `ProtocolError` | Wire, payload 또는 reply 계약을 처리할 수 없다. |
| 10 | `InvalidOperation` | 현재 object, session 또는 runtime 상태에서 operation을 실행할 수 없다. |
| 11 | `DataLost` | 공개된 Relocation payload가 없거나 검증에 실패했다. |
| 12 | `InternalFailure` | 위 분류로 표현할 수 없는 Framework 실패다. |

Generation, owner fence, moving phase, worker queue 상태와 Relocation 처리 단계는 내부 원인이다.
Application이 별도 대응을 선택할 필요가 없으면 새 public kind로 노출하지 않고 log와 trace에
기록한다.

## 3. 호출 전에 확인할 수 있는 오류

잘못된 인자와 이미 종료된 handle처럼 호출 위치에서 바로 확인할 수 있는 문제는 각 언어의
표준 argument 또는 invalid-operation 오류로 전달한다. Startup configuration 오류도 언어별
configuration exception으로 전달한다. 이런 오류를 remote error reply로 바꾸지 않는다.

Outbound queue 수락, route resolve 또는 remote reply를 기다리는 중에 확인한 Framework 실패는
언어별 Framework exception이나 `result`의 `ErrorKind`로 전달한다.

## 4. `Send` 완료와 실패

`Send`는 source runtime의 outbound queue가 message를 수락하면 결과값 없이 완료된다. 이 시점은
target handler가 message를 처리했다는 뜻이 아니다.

| 완료 전에 확인한 조건 | 결과 |
|---|---|
| Logical target이나 route가 존재하지 않음 | `NotFound` |
| Connection 또는 current owner를 현재 사용할 수 없음 | `Unavailable` |
| Send timeout까지 outbound queue가 message를 수락하지 않음 | `DeadlineExceeded` |
| Runtime이 신규 admission을 중단함 | `ShuttingDown` |

`Send`가 완료된 뒤 target activation, admission 또는 handler 실행이 실패해도 이미 완료된 call의
결과를 바꾸지 않는다. Framework는 이 실패를 metric, log와 message-flow trace로 기록하며 같은
message를 다른 target에 자동으로 제출하지 않는다.

## 5. `Request` 완료와 실패

`Request`는 typed reply를 받으면 정상 완료된다. 정상 reply를 만들 수 없으면 다음 `ErrorKind` 중
하나로 한 번만 완료한다.

- 대상이나 handler가 없으면 `NotFound`다.
- Route, connection 또는 current owner를 사용할 수 없으면 `Unavailable`이다.
- Reply를 deadline 안에 받지 못하면 `DeadlineExceeded`다.
- Wire, payload 또는 reply type을 처리할 수 없으면 `ProtocolError`다.
- Source runtime이 이 operation에 필요한 local bounded resource를 확보하지 못하면
  `CapacityExceeded`다. Reply를 보관할 자리, operation table entry처럼 source가 소유한
  자원이 대상이다. **같은 runtime 안의 Spot·Actor 대기열도 여기 해당한다** — 제출하는
  쪽과 대기열이 같은 process에 있으므로 source가 소유한 자원이다.
- 반면 **다른 node의 대기열이 가득 차서 실패한 것은 `Unavailable`이다.** Target의 대기열
  상태를 `CapacityExceeded`로 표현하지 않는다. 두 kind를 나누는 기준은 "실패한 자원을
  이 runtime이 소유하는가"이며, 호출자는 이 구분으로 재시도 대상을 판단한다.
- 이 구분은 **대기열에만** 적용한다. Target node의 배치 수용량이 부족한 경우는 대기열이
  아니라 admission 판정이므로 `CapacityExceeded`가 맞다
  ([Spot Actor](15-spot-actor.ko.md), [Spot 주소 메시징](16-spot-address-messaging.ko.md)).
- **Message Follow relay queue는 예외로 `CapacityExceeded`다.** 이 queue는 물리적으로 이전
  owner node에 있지만, relay 책임을 맡은 runtime이 자기 자원으로 소유하고 bound도 계약이
  정한 고정값(1024 messages, 16 MiB)이다. 호출자에게는 "상대 node가 못 받는 상태"가 아니라
  "이동 경로의 정해진 용량을 넘겼다"는 뜻이므로 재시도 판단이 다르다
  ([Spot Actor](15-spot-actor.ko.md), [위치 runtime](21-location-runtime.ko.md)).
  이 kind는 **`Request`에만 적용한다** — 이미 완료된 one-way의 결과는 어떤 relay 실패로도
  바뀌지 않으며(§4), relay bound 초과는 metric·log·trace로만 남는다.
- Runtime이 종료 중이면 `ShuttingDown`이다.
- 위 종류로 표현할 수 없는 Framework 실행 실패는 `InternalFailure`다.

Cancellation은 각 언어의 cancelled awaitable로 전달한다. `DeadlineExceeded`와 cancellation은
호출자가 reply를 기다리지 않게 되었다는 뜻이다. Remote handler가 실행되지 않았다는 뜻이 아니며,
뒤늦게 도착한 reply로 두 번째 결과를 만들지 않는다.

## 6. Typed 결과와 `Rejected`

Actor create나 join처럼 계약에 `Accepted`와 `Rejected`가 있는 operation은 Application callback의
판정을 typed 결과로 반환한다. 이때 `Rejected`는 Framework exception이 아니다.

공통 `ErrorKind.Rejected`는 typed 결과가 없는 filter, admission 또는 runtime policy가 operation을
거부했을 때만 사용한다. Application의 업무 규칙이 거부한 모든 결과를 Framework exception으로
변환하지 않는다.

## 7. 재시도 판단

Public exception, error object와 typed failure에는 `RetryAdvice`, `isRetriable`, `retriable` 같은
재시도 hint를 넣지 않는다. 같은 `ErrorKind`라도 operation이 이미 실행되었을 가능성과 중복 영향은
서로 다를 수 있기 때문이다.

Application이 새 operation을 시작하려면 다음을 직접 확인한다.

1. 이전 operation의 완료 조건과 remote 실행 가능성을 확인한다.
2. Operation이 idempotent한지, 또는 idempotency key로 중복 영향을 막는지 확인한다.
3. 필요한 경우 업무 상태를 다시 조회한 뒤 새 operation을 시작한다.

Framework 내부의 send-ready 대기, Store 결과 재확인과 수락 전 target 재선택은 Application retry가
아니다. Framework는 operation이 수락됐거나 수락 여부를 알 수 없게 된 뒤 다른 logical target에 같은
operation을 자동으로 제출하지 않는다.

## 8. 언어별 투영과 검증

다섯 server package와 HTTP client package는 같은 13개 kind를 사용한다. 언어별 interface 문서는
enum 이름, exception과 result 표현만 정의하며 kind를 추가하거나 재시도 boolean을 추가하지 않는다.

Contract test와 E2E는 다음을 검증한다.

- 각 언어의 13개 kind와 숫자가 일치한다.
- `Send`는 source outbound queue 수락 시 완료되고 이후 remote 실패로 결과가 바뀌지 않는다.
- `Request` timeout과 cancellation 뒤 늦은 reply가 두 번째 결과를 만들지 않는다.
- Typed `Rejected` 결과와 `ErrorKind.Rejected` exception을 구분한다.
- Public 오류 표면에 재시도 hint가 없다.
