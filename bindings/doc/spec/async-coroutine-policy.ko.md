---
title: "바인딩 비동기 완료 표면 정책"
---

<!-- bindings-nav:start -->
[스펙 목록](README.ko.md) | [이전: 개요](README.ko.md) | [다음: C](c/README.ko.md)
<!-- bindings-nav:end -->

# 바인딩 비동기 완료 표면 정책

> **이 장이 정의하는 것** — C를 제외한 언어별 바인딩이 비동기 완료를 노출할 때
> 지켜야 할 이름·반환 타입 정책.

이 문서는 C를 제외한 언어별 바인딩이 비동기 완료를 어떤 이름과 반환 타입으로
노출해야 하는지 정의한다. bindings 라이브러리는 core C API 위에 언어별 완료 경계를
제공한다. coroutine 실행, virtual thread 실행, event loop 연결, handler dispatcher
연결은 framework가 맡는다.

bindings 라이브러리는 coroutine을 직접 실행하거나 재개하는 기능을 제공하지 않는다.
따라서 C++ `co_await` 지원, Rust `async fn submit_async`, Python coroutine 반환
메서드처럼 coroutine 전용 public API를 bindings 계약에 넣지 않는다.

| 절 | 다루는 내용 |
|---|---|
| [공통 원칙](#공통-원칙) | operation 시작점 이름·builder·submit 실패 표현에 대한 공통 규칙 |
| [Request 완료 방식](#request-완료-방식) | 완료 객체 반환과 callback 기반 submit의 표면 비교 |
| [언어별 이름과 반환 타입](#언어별-이름과-반환-타입) | 언어별 builder 마지막 실행 메서드와 반환 타입 |
| [Framework에서 coroutine을 붙이는 방법](#framework에서-coroutine을-붙이는-방법) | 언어별 framework가 완료 경계를 자기 실행 모델로 바꾸는 방법 |

## 공통 원칙

- bindings public API는 operation 시작점 이름을 하나로 유지한다. `requestAsync`,
  `request_callback`, `sendNoWait`, `publishWithFlags`처럼 완료 방식이나 flag 조합을
  시작점 이름으로 늘리지 않는다.
- `send`, `request`, `reply`, `publish`, Actor 위치 작업, Actor session attach 작업은
  operation builder를 반환한다.
- payload, timeout, callback, 완료 방식은 operation 시작점 인자가 아니라 builder 단계에서
  표현한다.
- bindings 라이브러리는 coroutine scheduler, Kotlin `CoroutineScope`, C++ executor,
  framework dispatcher를 소유하지 않는다.
- coroutine 전용 recv, virtual thread 전용 recv, framework dispatcher 전용 submit 같은
  별도 public API를 bindings 계약에 추가하지 않는다.
- builder는 한 번 submit된 뒤 다시 submit될 수 없다. 언어가 ownership 타입이나 typestate를
  제공하면 타입으로 막고, 그렇지 않으면 런타임 상태 검사로 막는다.
- submit 실패와 reply 실패는 반환 타입의 실패 표현, callback result, 또는 언어 관용의
  예외로 전달한다.

## Request 완료 방식

`request`는 같은 `request` entrypoint가 반환하는 `RequestOp` builder에서 완료 방식을
고른다. coroutine을 위해 `requestCoroutine`, `submitAsync`, `submit_async` 같은 별도
coroutine 실행 메서드를 만들지 않는다.

| 구분 | bindings 완료 표면 | callback 기반 submit |
|---|---|---|
| 실행 의미 | 언어가 기본으로 쓰는 완료 객체를 반환하거나 현재 thread에서 완료를 기다린다 | callback으로 완료를 전달한다 |
| reply 전달 | 완료 객체의 성공 값 또는 blocking 반환값 | callback 인자 |
| submit flags | 받지 않는다 | 필요하면 `DONTWAIT` 같은 flags를 받을 수 있다 |
| timeout | builder의 `timeout(...)` 단계 | builder의 `timeout(...)` 단계 |
| submit 실패 | failed future, rejected promise, error result, 예외 | 언어별 submit 실패 표현 |
| reply 실패 | failed future, rejected promise, error result, 예외 | callback result |

## 언어별 이름과 반환 타입

아래 표는 bindings 라이브러리의 공개 표면 기준이다. framework는 이 표면을 감싸서
coroutine 또는 다른 실행 모델을 제공할 수 있지만, bindings public API 이름을 늘리면 안
된다.

| Binding | Builder 마지막 실행 메서드 | 반환 타입 또는 완료 표현 | Callback 표면 | 비고 |
|---|---|---|---|---|
| C | 해당 없음 | 해당 없음 | C callback 함수와 flags | C ABI는 builder 정책을 적용하지 않는다. `core/include/zlink.h`의 함수형 계약을 따른다. |
| C++ | `async()` 또는 `submit(callback)` | `async()`는 `async_result_t<T>`를 반환한다. `async_result_t<T>`는 `wait()`, `wait_for(...)`, `wait_until(...)`, `get()`을 지원한다. | `submit(callback)` | bindings 라이브러리 기준은 C++20이다. `async_result_t<T>`는 `co_await`를 지원하지 않는다. |
| Java | `submit()` / `await()` / `submit(callback)` | `submit()`은 `CompletionStage<T>`, `await()`는 현재 thread에서 완료를 기다린 뒤 `T` 또는 `void` 반환 | `submit(callback)` | `await()`는 새 network 의미가 아니라 `submit()` 결과를 기다리는 blocking adapter다. |
| .NET | `Async(...)` | `Task<T>`, `ValueTask<T>`, `Task`, 또는 `ValueTask` | callback이 필요한 builder에서 별도 submit 단계 | .NET 관례에 맞춰 마지막 실행 메서드 이름만 `Async`를 쓴다. 시작점 이름은 `RequestAsync`처럼 늘리지 않는다. |
| Node | `submit(...)` | `Promise<T>` 또는 `Promise<void>` | callback이 필요한 builder에서 별도 submit 단계 | JavaScript 사용자는 `await op.submit()`으로 기다릴 수 있지만, bindings가 별도 coroutine scheduler를 만들지 않는다. |
| Python | `submit(callback)` | callback 완료 | `submit(callback)` | bindings는 `submit_async()`나 coroutine object 반환 메서드를 제공하지 않는다. |
| Go | `Submit(ctx)`, `Submit(ctx, callback)`, 또는 `SubmitAsync(ctx)` | send 계열은 `(bool, error)`, reply 계열은 `error`, request 계열은 callback 완료 또는 `SubmitAsync(ctx)`가 반환하는 `<-chan RequestReplyCompletion` | `Submit(ctx, callback)` | `context.Context`는 operation 시작점이 아니라 실행 시점에 전달한다. request builder는 callback 경로(`Submit`)와 channel 경로(`SubmitAsync`)를 모두 제공한다. |
| Rust | `submit(callback)` 또는 즉시 submit | callback 완료 또는 `Result<_, SubmitError>` | `submit(callback)` | typestate builder로 payload 필수 조건과 중복 submit을 가능한 한 타입으로 막는다. bindings는 `async fn submit_async`를 제공하지 않는다. |

## Framework에서 coroutine을 붙이는 방법

framework는 bindings가 제공하는 완료 경계를 자기 실행 모델로 변환한다. 변환 코드는
framework나 framework 언어 wrapper가 소유한다.

### Java framework

- Java framework는 `submit()`으로 받은 `CompletionStage`를 handler executor, virtual
  thread, timeout 정책에 연결한다.
- virtual thread를 쓰는 경우에도 bindings에 virtual thread 전용 recv나 submit API를
  추가하지 않는다.
- `await()`는 virtual thread 위에서 호출하도록 만든 blocking adapter다. Virtual
  thread가 block되면 JVM이 그 thread를 park하고 carrier(platform) thread를 다른
  작업에 돌려주므로, platform thread를 막을 때와 달리 thread 하나를 낭비하지 않는다.
  이 성질 덕분에 sample이나 virtual thread 안의 코드는 `submit()`의 `CompletionStage`
  대신 `await()`로 순차적인 흐름을 안전하게 쓸 수 있다. framework 내부의 기본 비동기
  경로는 `submit()`을 기준으로 한다.

### Kotlin framework

- Kotlin wrapper는 Java `await()`를 호출하지 않는다.
- Kotlin wrapper는 Java builder의 `submit()`으로 `CompletionStage`를 얻고,
  `kotlinx-coroutines-jdk8`의 `await()`로 coroutine suspension에 연결한다.
- Kotlin 사용자 코드와 Kotlin sample은 Java `submit()`을 직접 호출하지 않고
  `connect().await()`, `request(...).await<T>()`, `waitFor<T>(...).await()` 같은 Kotlin
  wrapper를 사용한다.
- coroutine scope, dispatcher, cancellation 처리는 Kotlin framework가 소유한다. bindings
  라이브러리는 scope나 dispatcher를 만들지 않는다.

### C++ framework

- C++ bindings는 `async()`로 `async_result_t<T>` 완료 객체를 제공하고,
  `submit(callback)`으로 callback 완료를 제공한다.
- C++ framework가 coroutine을 지원할 때는 framework가 `async_result_t<T>`나 callback
  완료를 자기 `task_t<T>`로 감싼 뒤 framework coroutine 안에서 `co_await`한다.
- handler coroutine executor, handler dispatch, cancellation, resume thread 정책은
  framework가 소유한다.
- bindings public API에 `co_await` awaiter, coroutine 전용 `request_async`,
  `request_coroutine`, framework executor 인자, framework dispatcher 인자를 추가하지
  않는다.

### 다른 언어 framework

- .NET framework는 bindings의 `Task` / `ValueTask` 반환을 그대로 `await`한다.
- Node framework는 bindings의 `Promise` 반환을 event loop 정책에 맞게 `await`한다.
- Python framework는 bindings의 callback 완료를 `asyncio.Future`나 framework task로
  변환한다.
- Rust framework는 bindings의 callback 완료를 runtime별 `Future`로 변환하거나 framework
  channel에 연결한다.

이 방식이면 bindings는 C API wrapper로서의 책임을 유지하고, framework는 자기 실행 모델에
맞는 coroutine 지원을 독립적으로 제공할 수 있다.
