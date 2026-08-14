---
title: "바인딩 비동기 완료 표면 정책"
---

<!-- bindings-nav:start -->
[스펙 목록](README.ko.md) | [이전: 개요](README.ko.md) | [다음: C](c/README.ko.md)
<!-- bindings-nav:end -->

# 바인딩 비동기 완료 표면 정책

> **이 장이 정의하는 것** — C를 제외한 언어별 바인딩이 HWM-managed routed
> send/request의 비동기 완료와 raw reply의 동기 one-shot 완료를 노출할 때 지켜야 할
> 이름·반환 타입 정책.

이 문서는 C를 제외한 언어별 바인딩이 HWM-managed routed send/request의 비동기 완료와
raw ROUTER/`Received` reply의 동기 완료를 어떤 이름과 반환 타입으로 노출해야 하는지
정의한다. bindings 라이브러리는 core C API 위에 언어별 완료 경계를 제공한다.
coroutine 실행, virtual thread 실행, event loop 연결, handler dispatcher 연결은
framework가 맡는다.

bindings 라이브러리는 coroutine executor나 scheduler를 직접 소유하지 않는다. 다만
언어 관용의 suspension 객체는 binding 계약에 포함한다. Python builder의 `submit()`은
await 가능한 coroutine object를, Rust builder의 `submit()`은 runtime 비종속 `Future`를
반환한다. 이는 새 operation 시작점이나 framework executor가 아니다.

| 절 | 다루는 내용 |
|---|---|
| [공통 원칙](#공통-원칙) | operation 시작점 이름·builder·submit 실패 표현에 대한 공통 규칙 |
| [Managed routed 완료 방식](#managed-routed-완료-방식) | HWM-managed send/request의 언어 native suspension 완료 표면 |
| [Managed routed 언어별 이름과 반환 타입](#managed-routed-언어별-이름과-반환-타입) | managed builder 마지막 실행 메서드와 반환 타입 |
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
  framework dispatcher를 소유하지 않는다.
- coroutine 전용 recv, virtual thread 전용 recv, framework dispatcher 전용 submit 같은
  별도 public API를 bindings 계약에 추가하지 않는다.
- builder는 한 번 submit된 뒤 다시 submit될 수 없다. 언어가 ownership 타입이나 typestate를
  제공하면 타입으로 막고, 그렇지 않으면 런타임 상태 검사로 막는다.
- submit 실패와 reply 실패는 반환 타입의 실패 표현 또는 언어 관용의
  예외로 전달한다.

## Managed routed 완료 방식

DEALER/ROUTER routed send와 request는 같은 operation entrypoint가 반환하는 builder에서
언어별 canonical terminal을 호출한다. `requestCoroutine`, `request_async`, `submit_async`
같은 별도 이름을 만들지 않는다. HWM-managed routed builder는 callback 또는 blocking 호환
terminal을 함께 노출하지 않는다. Core C ABI의 callback, handler 등록과 routed builder가
아닌 one-shot 즉시 submit은 이 제거 범위가 아니다.

| 구분 | bindings 완료 표면 |
|---|---|
| 실행 의미 | 언어 native suspension 객체 또는 completion channel을 반환한다 |
| reply 전달 | suspension의 성공 값 또는 channel completion |
| submit flags | managed routed terminal은 받지 않는다 |
| timeout | builder의 `timeout(...)` 단계 |
| submit 실패 | failed task/future/promise, error result, 예외 |
| reply 실패 | 같은 suspension 결과의 실패 |

## Managed routed 언어별 이름과 반환 타입

아래 표는 HWM-managed DEALER/ROUTER routed send/request에만 적용하는 bindings 공개 표면
기준이다. raw reply와 PAIR·PUB·STREAM one-shot submit에는 적용하지 않는다. framework는
이 표면을 감싸서 coroutine 또는 다른 실행 모델을 제공할 수 있지만, bindings public API
이름을 늘리면 안 된다.

| Binding | Builder 마지막 실행 메서드 | 반환 타입 또는 완료 표현 | 비고 |
|---|---|---|---|
| C | 해당 없음 | 해당 없음 | C ABI는 builder 정책을 적용하지 않는다. `core/include/zlink.h`의 함수형 계약을 따른다. |
| C++ | `async()` | move-only `async_result_t<T>` | `co_await op.async()`가 canonical이다. `get`/`wait`/callback terminal은 제공하지 않으며 drop과 `cancel()`은 admission cancellation을 요청한다. |
| Java | `submit()` | `CompletionStage<T>` | blocking `await()`와 callback terminal을 병행하지 않는다. |
| .NET | `Async(...)` | `Task<T>`, `ValueTask<T>`, `Task`, 또는 `ValueTask` | 마지막 실행 메서드 이름만 .NET 관례를 따른다. |
| Node | `submit()` | `Promise<T>` 또는 `Promise<void>` | 사용자는 `await op.submit()`으로 suspension한다. |
| Python | `submit()` | await 가능한 coroutine object | event loop를 막지 않는다. |
| Kotlin | `submit()` | Java `CompletionStage<T>` | Kotlin wrapper의 canonical 사용은 `submit().await()`이다. |
| Go | `Submit(ctx)` | completion channel | `context.Context`는 실행 시점에 전달하며 별도 callback/`SubmitAsync` terminal을 만들지 않는다. |
| Rust | `submit()` | send는 `Future<Output = Result<(), SubmitError>>`, request는 `Future<Output = Result<Vec<Message>, ZlinkError>>` | runtime 비종속 Future이며 canonical 사용은 `submit().await?`이다. Future drop은 cancellation을 요청한다. |

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
framework나 framework 언어 wrapper가 소유한다.

### Java framework

- Java framework는 managed routed send/request의 `submit()`으로 받은
  `CompletionStage`를 handler executor, virtual thread, timeout 정책에 연결한다.
- virtual thread를 쓰는 경우에도 bindings에 virtual thread 전용 recv나 submit API를
  추가하지 않는다.

### Kotlin framework

- Kotlin wrapper는 Java `await()`를 호출하지 않는다.
- Kotlin wrapper는 Java managed builder의 `submit()`으로 `CompletionStage`를 얻고,
  `kotlinx-coroutines-jdk8`의 `await()`로 coroutine suspension에 연결한다.
- Kotlin 사용자 코드와 Kotlin sample은 Java `submit()`을 직접 호출하지 않고
  `connect().await()`, `request(...).await<T>()`, `waitFor<T>(...).await()` 같은 Kotlin
  wrapper를 사용한다.
- coroutine scope, dispatcher, cancellation 처리는 Kotlin framework가 소유한다. bindings
  라이브러리는 scope나 dispatcher를 만들지 않는다.

### C++ framework

- C++ bindings는 managed routed send/request의 `async()`로 move-only
  `async_result_t<T>`를 제공하고 사용자와 framework coroutine은 이를 직접
  `co_await`한다.
- standalone coroutine은 binding completion thread에서 재개될 수 있다. Framework
  `task_t` promise는 optional continuation scheduler hook으로 현재 serial turn과 ambient
  context만 handoff한다. admission retry queue는 binding이 계속 소유한다.
- bindings public API에 coroutine 전용 `request_async`, `request_coroutine`, framework
  executor 인자, framework dispatcher 인자를 추가하지 않는다.

### 다른 언어 framework

- .NET framework는 managed routed send/request가 반환한 `Task` / `ValueTask`를 그대로
  `await`한다.
- Node framework는 managed routed send/request가 반환한 `Promise`를 event loop 정책에
  맞게 `await`한다.
- Python framework는 managed routed send/request의 `submit()` coroutine object를 직접
  await한다.
- Rust framework는 managed routed send/request의 runtime 비종속 `submit()` Future를 자기
  executor에서 poll한다.

이 방식이면 bindings는 C API wrapper로서의 책임을 유지하고, framework는 자기 실행 모델에
맞는 coroutine 지원을 독립적으로 제공할 수 있다.
