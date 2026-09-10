---
title: "바인딩 전송과 비동기 완료 표면 정책"
---

<!-- bindings-nav:start -->
[스펙 목록](README.ko.md) | [이전: 개요](README.ko.md) | [다음: C](c/README.ko.md)
<!-- bindings-nav:end -->

# 바인딩 전송과 비동기 완료 표면 정책

> 이 문서는 C를 제외한 first-party binding이 send, request, publish와 reply operation을
> 시작하고 끝내는 방법을 정의한다. Native completion은 binding 내부에서 pull하고 언어별
> awaitable·blocking 결과로 바꾼다. C의 raw completion 계약은 [C binding 스펙](c/README.ko.md)이
> 소유한다.

## 1. Operation과 완료 경계

Send와 request는 local send queue admission에서 기다릴 수 있다. 고수준 binding의 blocking
terminal은 Core `NONE`, awaitable terminal은 Core `DONTWAIT`를 사용한다. Go는 public
`Submit(context.Context)` 하나에서 Core `DONTWAIT`로 제출하고 결과 객체를 즉시 돌려주며,
internal completion 대기는 결과 객체의 `Admitted(ctx)`·`Reply(ctx)`가 한다.

| Operation | Public 완료 경계 |
|---|---|
| Send | [Submit 결과 투영](README.ko.md#submit-result-projection)의 admission 결과. |
| Request | Blocking·awaitable terminal 모두 reply, timeout 또는 terminal request error까지 기다린다. |
| Publish | lossy/NODROP flag를 사용하며 submit 결과는 synchronous다. |
| Reply | Socket `SNDTIMEO`를 따르는 synchronous `NONE` admission으로 끝난다. |

Send operation별 timeout과 고수준 send·request flags는 public 계약에 포함하지 않는다. Request의
reply timeout은 builder에 남는다. Go와 Python의 publish는 별도 `PublishOp`를 사용하며 publish
flags와 synchronous submit 의미를 가진다.

## 2. Builder와 operation 시작

Socket은 target을 operation 생성 시 capture한다. Routed·non-routed send는 언어마다 하나의 send
operation family를 사용한다. `Received.send()/Send()`는 source target을 capture한 send builder를,
`Received.reply()/Reply()`는 source routing ID와 `ReplyToken`을 capture한 reply builder를 반환한다.
Reply가 없는 DATA envelope에서 reply builder를 요청하면 language invalid-state로 실패한다.

| Binding | PAIR | DEALER | ROUTER | STREAM |
|---|---|---|---|---|
| C++ | `send()` | `send()`, `request()` | `send(rid)`, `request(rid)`, `reply(rid, token)` | `send(rid)` |
| .NET | `Send()` | `Send()`, `Request()` | `Send(rid)`, `Request(rid)`, `Reply(rid, token)` | `Send(rid)` |
| Java/Kotlin | `send()` | `send()`, `request()` | `send(rid)`, `request(rid)`, `reply(rid, token)` | `send(rid)` |
| Node | `send()` | `send()`, `request()` | `send(rid)`, `request(rid)`, `reply(rid, token)` | `send(rid)` |
| Python | `send()` | `send()`, `request()` | `send(rid)`, `request(rid)`, `reply(rid, token)` | `send(rid)` |
| Go | `Send()` | `Send()`, `Request()` | `SendTo(rid)`, `Request(rid)`, `Reply(rid, token)` | `SendTo(rid)` |
| Rust | `send()` | `send()`, `request()` | `send(rid)`, `request(rid)`, `reply(rid, token)` | `send(rid)` |

Builder는 payload를 part 단위로 모으고 한 번만 submit할 수 있다. 고수준 binding은 기존
언어별 message ownership을 유지한다. Submit 실패 때 lvalue·managed message를 복구하던 binding은
staging에서 복구하고, rvalue·move input은 소비한다. 이 동작은 재전송 queue가 아니다.

## 3. Awaitable 완료

Submit 결과는 [공통 결과 투영](README.ko.md#submit-result-projection)을 따른다.
완료 합류·context 수명·정확히 한 번의 정리는
[비동기 실행 모델](async-execution-model.ko.md#5-submit-결과와-completion의-합류)이,
언어 wait 취소와 typed request error는
[caller wait cancellation](async-execution-model.ko.md#6-caller-wait-cancellation)이 소유한다.

## 4. PollCompletion과 pull event

Raw readiness와 고수준 progress의 구분, `NO_DATA`까지의 drain, public poller로의 owner 이전은
[비동기 실행 모델](async-execution-model.ko.md#4-poller와-completion-drain)을 따른다.

## 5. ReplyToken과 reply

ROUTER REQUEST receive만 유효한 `ReplyToken`을 만든다. Token은 responder socket instance와 opaque
value를 함께 보유하며 equality와 hash는 두 값을 함께 비교한다. Public constructor, parse, raw
숫자 변환, ordering, serialization과 close는 제공하지 않는다. 다른 responder socket에서 나온
같은 raw value는 같지 않다.

언어가 default 또는 zero 생성을 막을 수 없으면 그 값은 invalid다. 명시적 ROUTER reply는 token
owner와 receiver socket이 다른 경우 native 호출 전에 language invalid-argument로 실패한다.
C++과 Rust는 ROUTER wrapper가 만든 shared owner tag를 token이 함께 보유하여 wrapper 주소가
재사용돼도 다른 socket의 token과 같아지지 않게 한다.

Reply는 모든 고수준 binding에서 flag 없는 synchronous terminal 하나만 제공한다. C++·Go·Node·
Python·Rust reply builder도 flags를 받지 않는다.

## 6. 언어별 terminal interface

다음 선언은 각 언어 README의 전체 signature를 요약한다. 언어별 overload, visibility와 ownership은
해당 README가 소유한다.

비동기 종결자는 **제출 시점 결과 객체**를 돌려준다. `SendSubmission`은 `result`(제출 시점 스냅샷
`OK`|`BACKPRESSURED`)와 admission stage를, `RequestSubmission`은 여기에 reply stage를 더한다.
`result`가 `OK`면 admission은 이미 완료 상태이고(SEND는 이것으로 끝, REQUEST는 reply가 completion에서
완료된다), `BACKPRESSURED`면 바인딩이 입력을 보관하고 WRITABLE 재제출로 admission을 완성한다. 그 밖의
제출 실패(`NOT_CONNECTED`·`NOT_FOUND`·`NOT_ADMITTED`·`INVALID_ARGUMENT`·`TERMINATED`·`OUT_OF_MEMORY`·
`INTERNAL_ERROR` 등)는 결과 객체가 아니라 **예외/에러**로 낸다(결과 객체와 예외 두 곳을 보게 하지 않는다).
객체·필드 이름(`Submission`·`result`·`admitted`·`reply`)은 7언어가 공유한다. 구조와 합류 규칙은
[공통 결과 투영](README.ko.md#submit-result-projection)과
[비동기 실행 모델 §5](async-execution-model.ko.md#5-submit-결과와-completion의-합류)가 소유한다. 동기
종결자(`submit_sync()`, .NET·C++ `Submit()`/`submit()`)는 바뀌지 않는다.

| Binding | Send terminal | Request terminal | Reply terminal |
|---|---|---|---|
| C++ | `void submit() &&`, `send_submission_t async() &&` | `vector<message_t> submit() &&`, `request_submission_t async() &&` | `void submit() &&` |
| .NET | `void Submit()`, `SendSubmission Async(CancellationToken)` | `IReadOnlyList<Message> Submit()`, `RequestSubmission Async(CancellationToken)` | `void Submit()` |
| Java/Kotlin | `SendSubmission submit()`, `void submit_sync()` | `RequestSubmission submit()`, `List<Message> submit_sync()` | `void submit()` |
| Node | `SendSubmission submit()`, `void submit_sync()` | `RequestSubmission submit()`, `Message[] submit_sync()` | `void submit()` |
| Python | `SendSubmission submit()`, `None submit_sync()` | `RequestSubmission submit()`, `list[Message] submit_sync()` | `None submit()` |
| Go | `Submit(context.Context) (SendSubmission, error)` | `Submit(context.Context) (RequestSubmission, error)` | `Submit(context.Context) error` |
| Rust | `Result<SendSubmission, SubmitError> submit()`, `Result<(), SubmitError> submit_sync()` | `Result<RequestSubmission, ZlinkError> submit()`, `Result<Vec<Message>, ZlinkError> submit_sync()` | `Result<(), SubmitError> submit()` |

Kotlin suspend 표면은 `admitted()`·`reply()`를 `await()`하는 확장으로 대응한다. Go의 대기는 결과 객체의
`Result()`·`Admitted(ctx)`·`Reply(ctx)` 메서드가 한다(§6 Go 행의 `Submit(ctx)`는 제출만 하고 즉시 돌려준다).

Go와 Python의 publish는 다음 별도 operation family를 사용한다.

```text
Go:     PublishOp -> PublishSubmitOp.Flags(SendFlags).Submit(context.Context) (bool, error)
Python: PublishOp.flags(flags).submit() -> None
```

나머지 고수준 binding은 분리된 publish operation type과 publish terminal을 사용한다.

## 7. 구현 및 contract test 검증 요구

Public builder, terminal, poller event와 language result만으로 다음을 확인한다. 각 항목은 contract
test 하나로 이어진다.

**Operation 표면**

- 각 socket의 send·request·reply factory가 §2의 이름과 하나의 send operation family를 반환하고
  target을 builder에 보존한다.
- Send와 request terminal은 §6의 signature만 제공하며 send/request flags, send timeout과 request
  callback terminal을 제공하지 않는다.
- 비동기 종결자는 결과 객체를 돌려준다. `result`는 제출 시점 `OK`|`BACKPRESSURED` 스냅샷이고, `OK`면
  `admitted`가 완료 상태, `BACKPRESSURED`면 WRITABLE 재제출 뒤 `admitted`가 완료된다. REQUEST의 `reply`는
  `admitted` 성공 뒤에만 완료되고 `admitted` 실패 시 같은 원인으로 실패한다(정확히 한 번). `OK`|`BACKPRESSURED`
  외 제출 실패가 예외/에러로 나오는 것과 `admitted`·`reply`가 각각 한 번만 끝나는 것을 contract test로 확인한다.
- Go·Python publish는 별도 `PublishOp`에서 publish flags와 synchronous submit 결과를 제공한다.
- Reply terminal은 flags 없이 synchronous `NONE` admission 결과를 반환한다.

**완료와 cancellation**

Submit 경합·cancellation·request error의 관측은
[공통 실행 모델의 검증 요구](async-execution-model.ko.md#7-구현-및-contract-test-검증-요구)를 따른다.

**Poller와 token**

Poller의 관측은 [공통 실행 모델의 검증 요구](async-execution-model.ko.md#7-구현-및-contract-test-검증-요구)를 따른다.

- ROUTER REQUEST receive가 만든 token은 같은 socket·value에서 같고 다른 socket에서는 다르며,
  invalid token과 다른 owner token은 native reply 전에 거부된다.

<!-- bindings-nav:start -->
[스펙 목록](README.ko.md) | [이전: 개요](README.ko.md) | [다음: C](c/README.ko.md)
<!-- bindings-nav:end -->
