---
title: "바인딩 routed 전송 계약과 비동기 완료 표면 정책"
---

<!-- bindings-nav:start -->
[스펙 목록](README.ko.md) | [이전: 개요](README.ko.md) | [다음: C](c/README.ko.md)
<!-- bindings-nav:end -->

# 바인딩 routed 전송 계약과 비동기 완료 표면 정책

> **개정 기록** — 2026-08-23, 소유자 결정으로 이 문서를 개정했다. HWM-managed
> routed **send**의 canonical terminal을 동기 `submit()`으로 되돌리고, 그
> 도입 근거였던 binding-owned admission 기계장치(park queue, WRITABLE-callback
> 재시도, deadline timer, dispatcher thread)와 "HWM-managed routed는 async
> 전용" 규칙을 폐지했다. **request**의 비동기 완료 표면(Core가 reply로 완료를
> 구동하는 suspension)은 그대로 유지한다. 근거와 실측은
> `doc/plan/cpp-routed-async-contract-issue.ko.md` (§0 지배 원칙, §3.1 선행
> 확인, §3.2 최종 설계 확정)를 참고한다.

> **이 장이 정의하는 것** — C를 제외한 언어별 바인딩이 (1) HWM-managed routed
> **send**의 동기 `submit()` 제출 계약과 (2) **request**의 Core-구동 비동기
> 완료, 그리고 raw reply의 동기 one-shot 완료를 노출할 때 지켜야 할 이름·반환
> 타입 정책.

이 문서는 C를 제외한 언어별 바인딩이 HWM-managed DEALER/ROUTER **send**의 동기 제출,
**request**의 비동기 완료, raw ROUTER/`Received` reply의 동기 완료를 어떤 이름과
반환 타입으로 노출해야 하는지 정의한다. bindings 라이브러리는 core C API 위에
언어별 완료 경계를 제공한다. coroutine 실행, virtual thread 실행, event loop
연결, handler dispatcher 연결은 framework가 맡는다.

**bindings 라이브러리는 스레드를 하나도 소유하지 않는다.** routed send는
Core send를 그대로 감싸는 동기 호출이며, HWM 대기·재개·timeout은 전부
Core가 소유한다: blocking 모드는 Core 내부에서 대기하다 Core 신호로 재개하고,
`SNDTIMEO`가 대기 상한을 정하며, `DONTWAIT`은 즉시 `EAGAIN`을 반환한다.
백프레셔 정책의 소유자는 어플리케이션이다. request는 다르다 — reply는 Core
자신이 완료를 구동하는 지점(reply handler callback)이 있으므로, 바인딩은
그 콜백이 suspension을 완료하도록 연결할 뿐 자체 재시도나 스레드를 두지
않는다. suspension 재개는 Core가 완료를 전달한 컨텍스트에서 일어난다. bindings
라이브러리는 coroutine executor나 scheduler를 직접 소유하지 않는다. 다만
언어 관용의 suspension 객체는 binding 계약에 포함한다. Python request builder의
`submit()`은 await 가능한 coroutine object를, Rust request builder의 `submit()`은
runtime 비종속 `Future`를 반환한다. 이는 새 operation 시작점이나 framework
executor가 아니다.

| 절 | 다루는 내용 |
|---|---|
| [공통 원칙](#공통-원칙) | operation 시작점 이름·builder·submit 실패 표현에 대한 공통 규칙 |
| [Routed send 동기 제출 계약](#routed-send-동기-제출-계약) | HWM-managed routed send의 동기 submit 완료 표면 |
| [Request 비동기 완료 방식](#request-비동기-완료-방식) | HWM-managed request의 언어 native suspension 완료 표면 |
| [Routed send와 request 언어별 이름과 반환 타입](#routed-send와-request-언어별-이름과-반환-타입) | routed send/request builder 마지막 실행 메서드와 반환 타입 |
| [Raw reply 동기 one-shot](#raw-reply-동기-one-shot) | 일곱 binding의 reply 종결자와 즉시 실패 계약 |
| [Framework typed Session reply](#framework-typed-session-reply) | raw binding reply와 별개인 awaitable Framework 계약 |
| [Framework에서 coroutine을 붙이는 방법](#framework에서-coroutine을-붙이는-방법) | 언어별 framework가 완료 경계를 자기 실행 모델로 바꾸는 방법 |

## 공통 원칙

- bindings public API는 operation 시작점 이름을 하나로 유지한다. `requestAsync`,
  `request_callback`, `sendNoWait`, `publishWithFlags`처럼 완료 방식이나 flag 조합을
  시작점 이름으로 늘리지 않는다.
- `send`, `request`, `reply`, `publish`, Actor 위치 작업, Actor session attach 작업은
  operation builder를 반환한다.
- payload와 timeout은 operation 시작점 인자가 아니라 builder 단계에서
  표현한다.
- bindings 라이브러리는 coroutine scheduler, Kotlin `CoroutineScope`, C++ executor,
  framework dispatcher를 소유하지 않는다. **바인딩 라이브러리는 자체 스레드,
  대기열, 재시도 정책도 소유하지 않는다** — routed send든 request든 마찬가지다.
- coroutine 전용 recv, virtual thread 전용 recv, framework dispatcher 전용 submit 같은
  별도 public API를 bindings 계약에 추가하지 않는다.
- builder는 한 번 submit된 뒤 다시 submit될 수 없다. 언어가 ownership 타입이나 typestate를
  제공하면 타입으로 막고, 그렇지 않으면 런타임 상태 검사로 막는다.
- submit 실패와 reply 실패는 반환 타입의 실패 표현 또는 언어 관용의
  예외로 전달한다.
- request builder에는 callback 또는 다른 blocking 호환 terminal을 canonical
  suspension terminal과 함께 노출하지 않는다. routed send builder는 이 규칙의
  적용 대상이 아니다 — canonical terminal 자체가 동기다 (아래 참고).

## Routed send 동기 제출 계약

DEALER/ROUTER **send**(request가 아닌 단방향 routed 전송)는 같은 operation
entrypoint가 반환하는 builder에서 언어별 canonical **동기** terminal을 호출한다.
`send_async`, `sendCoroutine` 같은 별도 이름을 만들지 않는다. 이 terminal은
Core send를 직접 감싸며, 바인딩은 admission을 위한 park queue, WRITABLE-callback
재시도, deadline timer, dispatcher thread를 두지 않는다.

| 구분 | bindings 완료 표면 |
|---|---|
| 실행 의미 | Core send를 직접 감싸는 동기 호출. 반환/예외로 즉시 끝난다 |
| Blocking 모드 | Core 내부에서 대기하다 Core 신호로 재개한다 (바인딩 대기 없음) |
| `SNDTIMEO` | Core가 대기 상한으로 사용한다 |
| `DONTWAIT` | Core가 즉시 `EAGAIN`을(언어별 `BACKPRESSURED`로) 반환한다 |
| submit flags | 언어 관용 방식(옵션 인자 또는 builder 단계)으로 받을 수 있다 |
| 실패 | 언어 관용의 예외 또는 `Result`/`error` 반환 — raw reply의 오늘 계약과 같은 형태 |
| 백프레셔 정책 | 어플리케이션이 소유한다. 바인딩은 재시도하지 않는다 |

## Request 비동기 완료 방식

DEALER/ROUTER **request**는 같은 operation entrypoint가 반환하는 builder에서
언어별 canonical suspension terminal을 호출한다. `requestCoroutine`, `request_async`,
`submit_async` 같은 별도 이름을 만들지 않는다. reply 완료는 Core가 구동한다:
reply handler callback이 suspension을 완료하고, 재개는 완료가 발생한 컨텍스트에서
일어난다. request timeout은 이미 Core 소유다(`ZLINK_REQUEST_TIMED_OUT`). 바인딩은
이 완료 표면을 위해 재시도 큐나 전용 스레드를 두지 않는다.

| 구분 | bindings 완료 표면 |
|---|---|
| 실행 의미 | 언어 native suspension 객체 또는 completion channel을 반환한다 |
| reply 전달 | suspension의 성공 값 또는 channel completion |
| submit flags | request managed terminal은 받지 않는다 |
| timeout | builder의 `timeout(...)` 단계. 만료는 Core가 `ZLINK_REQUEST_TIMED_OUT`으로 통지한다 |
| submit 실패 | failed task/future/promise, error result, 예외 |
| reply 실패 | 같은 suspension 결과의 실패 |
| 재개 컨텍스트 | Core가 완료를 전달한 컨텍스트(reply handler callback). 이후 실행 모델 연결은 framework 몫이다 |

## Routed send와 request 언어별 이름과 반환 타입

아래 표는 HWM-managed DEALER/ROUTER **routed send**와 **request**에 적용하는 bindings
공개 표면 기준이다. raw reply와 PAIR·PUB·STREAM one-shot submit에는 적용하지 않는다.
routed send는 raw reply와 같은 동기 형태를, request는 언어 native suspension 표면을
쓴다는 점에 유의한다. framework는 이 표면을 감싸서 coroutine 또는 다른 실행 모델을
제공할 수 있지만, bindings public API 이름을 늘리면 안 된다.

| Binding | Routed send terminal | Send 반환/실패 | Request terminal | Request 반환 타입 또는 완료 표현 |
|---|---|---|---|---|
| C | 해당 없음 | 해당 없음 | 해당 없음 | C ABI는 builder 정책을 적용하지 않는다. `core/include/zlink.h`의 함수형 계약을 따른다. |
| C++ | `submit()` | `void`, 실패 시 `submit_error_t`를 던진다 | `async()` | move-only `async_result_t<T>`. `co_await op.async()`가 canonical이다. `get`/`wait`/callback terminal은 제공하지 않으며 drop과 `cancel()`은 cancellation을 요청한다 |
| Java | `submit()` | `void`, 실패 시 `ZlinkSubmitException`을 던진다 | `submit()` | `CompletionStage<T>`. blocking `await()`와 callback terminal을 병행하지 않는다 |
| .NET | `Submit()` | `void`, 실패 시 `ZlinkSubmitException`을 던진다 | `Async(...)` | `Task<T>`, `ValueTask<T>`, `Task`, 또는 `ValueTask`. 마지막 실행 메서드 이름만 .NET 관례를 따른다 |
| Node | `submit()` | `void`, 실패 시 `SubmitError`를 던진다 | `submit()` | `Promise<T>` 또는 `Promise<void>`. 사용자는 `await op.submit()`으로 suspension한다 |
| Python | `submit()` | `None`, 실패 시 `SubmitError`를 발생시킨다 | `submit()` | await 가능한 coroutine object. event loop를 막지 않는다 |
| Kotlin | `submit()` | Java와 동일 (`void`, 예외) | `submit()` | Java `CompletionStage<T>`. Kotlin wrapper의 canonical 사용은 `submit().await()`이다 |
| Go | `Submit(ctx)` | `error` (`nil` 성공) | `Submit(ctx)` | completion channel. `context.Context`는 실행 시점에 전달하며 별도 callback/`SubmitAsync` terminal을 만들지 않는다 |
| Rust | `submit()` | `Result<(), SubmitError>` | `submit()` | runtime 비종속 `Future<Output = Result<Vec<Message>, ZlinkError>>`를 반환한다. canonical 사용은 `submit().await?`이며 Future drop은 cancellation을 요청한다 |

## Raw reply 동기 one-shot

Raw ROUTER/`Received` reply builder는 HWM-managed routed send/request가 아니다. 종결자는
terminal reply 또는 error reply를 HWM 없는 completion lane에 native 호출 한 번으로
제출하고 동기적으로 끝난다. HWM backpressure는 reply 결과가 아니다. `NOT_CONNECTED`,
`TERMINATED`, `INVALID_ARGUMENT` 또는 다른 non-HWM submit 실패는 즉시 언어별
`SubmitError`로 전달한다.

| Binding | Raw reply terminal | 성공 반환 | 실패 표현 |
|---|---|---|---|
| C++ | `reply_submit_operation_t::submit()` | `void` | `submit_error_t`를 던진다 |
| .NET | `ReplySubmitOperation.Submit()` | `void` | `ZlinkSubmitException`을 던진다 |
| Java | `ReplySubmitOperation.submit()` | `void` | `ZlinkSubmitException`을 던진다 |
| Node | `ReplySubmitOperation.submit()` | `void` | `SubmitError`를 던진다 |
| Go | `ReplySubmitOp.Submit(ctx)` | `nil` | `*SubmitError`를 `error`로 반환한다 |
| Python | `ReplyOp.submit()` | `None` | `SubmitError`를 발생시킨다 |
| Rust | `ReplyOp<Ready>::submit()` | `Ok(())` | `Err(SubmitError)`를 반환한다 |

Kotlin이 Java binding을 직접 사용할 때도 Java의 동기 `submit(): void` reply 계약을
그대로 사용한다. 이는 아래의 Kotlin Framework `reply(...).await()`와 다른 API다.
routed send의 동기 `submit()`도 같은 형태를 따르되, 별개의 operation(단방향 send)에
대한 별개의 builder라는 점에 유의한다.

## Framework typed Session reply

Framework의 typed Session reply는 raw binding reply를 async 종결자로 바꾼 표면이 아니다.
Framework runtime이 typed serialization과 request별 one-shot reply token을 소유하며,
종결자는 token을 원자적으로 claim한 뒤 source-local admission까지 기다린다. 같은 token의
두 번째 reply는 transport를 시도하지 않고 exceptional completion으로 끝난다.

| Framework 언어 | Typed Session reply terminal | 완료 표현 |
|---|---|---|
| C++ | `.reply_packet(...).submit()` | `co_await` 가능한 Framework task |
| .NET | `.Reply(...).Async(ct)` | `ValueTask` |
| Java | `.reply(...).submit()` | `CompletionStage<Void>` |
| Kotlin | `.reply(...).await()` | suspending `Unit` |
| Node | `.reply(...).submit(signal?)` | `Promise<void>` |

따라서 같은 `submit` 이름을 쓰는 C++·Java·Node에서도 반환 타입과 소유 계층으로 의미를
구분한다. Raw binding reply는 동기 one-shot이고, Framework typed Session reply만
source-local admission을 관찰하는 awaitable이다.

## Framework에서 coroutine을 붙이는 방법

framework는 bindings가 제공하는 완료 경계를 자기 실행 모델로 변환한다. 변환 코드는
framework나 framework 언어 wrapper가 소유한다. **bindings는 executor를 제공하지
않는다** — routed send용 awaitable을 원하는 framework는 동기 `submit()`을 자기
executor(thread pool, event loop offload 등)로 감싸야 한다. request는 이미 언어
native suspension을 반환하므로 감쌀 필요가 없다.

### Java framework

- Java framework는 managed request의 `submit()`으로 받은
  `CompletionStage`를 handler executor, virtual thread, timeout 정책에 연결한다.
- routed send에서 awaitable을 원하면 동기 `submit()`을 framework 소유 executor로
  감싼다(예: virtual thread에서 호출).
- virtual thread를 쓰는 경우에도 bindings에 virtual thread 전용 recv나 submit API를
  추가하지 않는다.

### Kotlin framework

- Kotlin wrapper는 Java `await()`를 호출하지 않는다.
- Kotlin wrapper는 Java managed request builder의 `submit()`으로 `CompletionStage`를 얻고,
  `kotlinx-coroutines-jdk8`의 `await()`로 coroutine suspension에 연결한다.
- routed send는 Kotlin wrapper가 `Dispatchers.IO` 같은 자기 dispatcher에서 동기
  `submit()`을 호출해 suspend function으로 감싼다.
- Kotlin 사용자 코드와 Kotlin sample은 Java `submit()`을 직접 호출하지 않고
  `connect().await()`, `request(...).await<T>()`, `waitFor<T>(...).await()` 같은 Kotlin
  wrapper를 사용한다.
- coroutine scope, dispatcher, cancellation 처리는 Kotlin framework가 소유한다. bindings
  라이브러리는 scope나 dispatcher를 만들지 않는다.

### C++ framework

- C++ bindings는 managed request의 `async()`로 move-only
  `async_result_t<T>`를 제공하고 사용자와 framework coroutine은 이를 직접
  `co_await`한다.
- standalone coroutine은 완료가 발생한 컨텍스트(Core reply handler callback 등)에서
  재개될 수 있다. 이는 바인딩이 완료·재개를 위한 자체 스레드, dispatcher,
  scheduler를 생성해도 된다는 뜻이 아니다 — 실행 자원 소유는 framework의
  몫이다. Framework `task_t` promise는 optional continuation scheduler hook으로
  현재 serial turn과 ambient context만 handoff한다.
- routed send는 동기 `submit()`이다. C++ framework가 awaitable routed send를
  원하면 자기 executor(thread pool 등)에서 `submit()`을 호출하고 그 결과를
  `task_t`로 감싼다 — bindings는 이 executor를 만들지 않는다.
- bindings public API에 coroutine 전용 `request_async`, `request_coroutine`, framework
  executor 인자, framework dispatcher 인자를 추가하지 않는다.

### 다른 언어 framework

- .NET framework는 managed request가 반환한 `Task` / `ValueTask`를 그대로
  `await`한다. routed send에서 awaitable이 필요하면 동기 `Submit()`을
  `Task.Run(...)` 같은 framework 소유 executor로 감싼다.
- Node framework는 managed request가 반환한 `Promise`를 event loop 정책에
  맞게 `await`한다. routed send는 동기 `submit()`이므로, Node framework가
  non-blocking 실행을 원하면 자기 worker offload 정책으로 감싼다.
- Python framework는 managed request의 `submit()` coroutine object를 직접
  await한다. routed send에서 awaitable이 필요하면 `loop.run_in_executor(...)` 같은
  framework 소유 실행 경로로 동기 `submit()`을 감싼다.
- Rust framework는 managed request의 runtime 비종속 `submit()` Future를 자기
  executor에서 poll한다. routed send는 동기 `submit()`이므로, 비동기 실행이
  필요하면 framework가 `spawn_blocking` 등으로 감싼다.

이 방식이면 bindings는 C API wrapper로서의 책임을 유지하고, framework는 자기 실행 모델에
맞는 coroutine 지원을 독립적으로 제공할 수 있다. bindings는 어느 경우에도 스레드를
소유하지 않는다.
