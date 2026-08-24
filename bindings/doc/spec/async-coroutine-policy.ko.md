---
title: "바인딩 routed 전송 계약과 비동기 완료 표면 정책"
---

<!-- bindings-nav:start -->
[스펙 목록](README.ko.md) | [이전: 개요](README.ko.md) | [다음: C](c/README.ko.md)
<!-- bindings-nav:end -->

# 바인딩 routed 전송 계약과 비동기 완료 표면 정책

> **이 장이 정의하는 것** — C를 제외한 언어별 바인딩이 (1) HWM-managed
> **send**(PAIR send, DEALER/ROUTER routed send)의 async 완료 표면,
> (2) PUB/XPUB **publish**의 동기 `submit()` 완료 표면, (3) **request**의
> Core-구동 비동기 완료(C++는 예외적으로 `submit()`/`submit(callback)`/
> `async()` 세 terminal을 갖는다), 그리고 (4) raw reply의 HWM-free 동기
> one-shot 완료를 노출할 때 지켜야 할
> 이름·반환 타입 정책.

이 문서는 C를 제외한 언어별 바인딩이 HWM-managed PAIR **send**와
DEALER/ROUTER **routed send**의 async 완료, PUB/XPUB **publish**의 동기
`submit()` 완료, **request**의 async 완료, raw ROUTER/`Received` reply의
동기 완료를 어떤 이름과 반환 타입으로 노출해야 하는지 정의한다. bindings
라이브러리는 core C API 위에 언어별 완료 경계를 제공한다. coroutine 실행,
virtual thread 실행, event loop 연결, handler dispatcher 연결은 framework가
맡는다.

**bindings 라이브러리는 스레드를 하나도 소유하지 않는다.** HWM-managed
send와 routed send의 수용 대기와 재시도는 Core가 소유한다. C++의 blocking
`submit()`과 Go의 `Submit(ctx)`는 Core 안에서 대기하고, 그 밖의 언어별 비동기
terminal은 `zlink_send_async`를 제출한 뒤 `zlink_send_complete_handler`가 전달하는
최종 완료로 awaitable을 끝낸다. Core가 수용한 operation은
`ZLINK_SEND_ADMITTED`, `ZLINK_SEND_TIMED_OUT`, `ZLINK_SEND_TERMINAL` 중 하나로
정확히 한 번 완료된다. Binding은 operation id와 언어별 완료 객체만 연결하며
재시도하지 않는다. Core의 pending-operation 상한 때문에 최초 제출이 거부된
경우에만 어플리케이션이 재시도 여부를 정한다. `publish`는 HWM에서 대기하지
않으므로 동기 `submit()`으로 완료한다. request는 이미 Core 자신이 완료를
구동하는 지점(reply handler callback, `ZLINK_REQUEST_TIMED_OUT`)이 있으므로,
바인딩은 그 지점이 suspension·callback·completion channel을 완료하도록
연결할 뿐 자체 재시도나 스레드를 두지 않는다. suspension 재개는 Core가
완료를 전달한 컨텍스트에서 일어난다. bindings 라이브러리는 coroutine
executor나 scheduler를 직접 소유하지 않는다. 다만 언어 관용의 suspension
객체는 binding 계약에 포함한다. Python request builder의 `submit()`은 await
가능한 coroutine object를, Rust request builder의 `submit()`은 runtime
비종속 `Future`를 반환한다. 이는 새 operation 시작점이나 framework
executor가 아니다.

| 절 | 다루는 내용 |
|---|---|
| [분류 원칙](#분류-원칙) | HWM 대기 가능 여부로 send/request를 ASYNC로, publish/raw reply를 SYNC로 가르는 기준 |
| [공통 원칙](#공통-원칙) | operation 시작점 이름·builder·submit 실패 표현에 대한 공통 규칙 |
| [HWM-managed send 완료 계약](#hwm-managed-send-완료-계약) | HWM-managed send(routed send 포함)의 async 완료와 PUB/XPUB publish 동기 제출 계약 |
| [Request 완료 표면과 C++ 세 terminal](#request-완료-표면과-c-세-terminal) | HWM-managed request의 언어별 완료 표면과 C++의 세 terminal |
| [Send·Publish·Request·Raw reply 언어별 정규 표](#sendpublishrequestraw-reply-언어별-정규-표) | 네 operation 유형 각각의 언어별 terminal과 반환 타입 |
| [Raw reply 동기 one-shot](#raw-reply-동기-one-shot) | 일곱 binding의 reply 종결자와 즉시 실패 계약 |
| [Framework typed Session reply](#framework-typed-session-reply) | raw binding reply와 별개인 awaitable Framework 계약 |
| [Framework에서 coroutine을 붙이는 방법](#framework에서-coroutine을-붙이는-방법) | 언어별 framework가 완료 경계를 자기 실행 모델로 바꾸는 방법 |

## 분류 원칙

- **HWM 대기가 발생할 수 있는 조작은 ASYNC 함수로 분류한다.** PAIR send,
  DEALER/ROUTER의 routed send, request는 HWM 대기가 발생할 *수* 있는 지점을
  지나므로 ASYNC로 분류한다. 분류 기준은 "HWM 대기가 실제로 자주 발생하는가"가
  아니라 "HWM 대기가 발생할 가능성이 있는가"다.
- **raw reply는 HWM-free이며 진짜 synchronous다.** raw ROUTER/`Received`
  reply는 HWM 경로를 전혀 거치지 않으므로 순수 동기인 completion lane이다.
- **PUB/XPUB `publish`는 이 ASYNC 분류에 포함하지 않는다.** 기본 PUB
  의미론은 lossy drop이므로 subscriber queue가 HWM에 도달해도 해당 copy를
  drop하고 publisher는 즉시 진행한다. 따라서 HWM 대기가 없고, publish의
  동기 `submit()`이 terminal이다. `ZLINK_PUB_OPT_NODROP`에서는 가득 찬
  subscriber가 동기 submit에서 즉시 `BACKPRESSURED`/`EAGAIN`을 표면화하며,
  재시도 정책은 어플리케이션이 소유한다.
- 이 분류는 bindings와 framework 표면에 동일하게 적용된다. framework의
  typed Session reply, managed request, HWM-managed send도 같은 기준으로
  ASYNC/SYNC가 갈린다 — framework가 별도의 완화된 규칙을 갖지 않는다.
  publish는 위의 lossy 계약에 따라 별도로 synchronous다.
- 이 원칙이 아래 모든 절의 근거다: C++가 send에 `submit()`/`async()`(또는
  request의 세 terminal)를 함께 노출하는 것도, 다른 언어가 send에서 awaitable을
  반환하는 것도, publish와 raw reply가 순수 동기로 남는 것도 모두 이 분류
  원칙에서 도출된다.

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
  대기열, 재시도 정책도 소유하지 않는다** — send, publish, routed send,
  request든 마찬가지다.
- coroutine 전용 recv, virtual thread 전용 recv, framework dispatcher 전용 submit 같은
  별도 public API를 bindings 계약에 추가하지 않는다.
- builder는 한 번 submit된 뒤 다시 submit될 수 없다. 언어가 ownership 타입이나 typestate를
  제공하면 타입으로 막고, 그렇지 않으면 런타임 상태 검사로 막는다.
- submit 실패와 reply 실패는 반환 타입의 실패 표현 또는 언어 관용의
  예외로 전달한다.
- **이름 구분 원칙 — 언어적 특성 반영이 기본 정책이다.** 각 언어의 terminal
  모양은 그 언어의 관용을 따른다. C++는 plain thread에서 바로 호출 가능한
  함수와 coroutine에서만 호출 가능한 함수를 이름으로 구분해야 한다 —
  `submit()`은 plain thread용(Core 내부에서 blocking하는 호출), `async()`는
  coroutine용(suspend하는 호출)이다. .NET은 async-classified operation에
  자신의 Async 접미사 관례를 따르는 `Async()` terminal을, synchronous
  publish와 raw reply에는 `Submit()` terminal을 둔다. 그 외 모든 언어는
  operation마다 단일 `submit()`(Go는 `Submit(ctx)`) terminal을 유지한다.
  async-classified operation의 terminal이 반환하는 awaitable은 각 언어가
  관용적인 방식(await / join / block_on / channel recv)으로 소비하고,
  synchronous publish와 raw reply의 terminal은 호출 즉시 소비한다. 같은
  원칙에 따라 Go의 send `Submit(ctx)`는 `error`를 반환하는 동기형이다.
  goroutine 안에서의 blocking 호출이 Go의 관용적 대기 방식이고
  `context.Context`는 제출 전 취소와 시한을 확인한다.
- C++ request builder는 `submit()`(blocking) · `submit(callback)`(completion
  전달 전용) · `async()`(coroutine) 세 terminal을 함께 노출한다 — 위 이름
  구분 원칙에 따라 C++만 이 세 terminal이 필요하기 때문이다. 다른 언어는
  단일 terminal만 유지한다 — 그 terminal이 반환하는 awaitable이 이미 모든
  소비 방식을 지원하므로 별도 terminal을 늘릴 이유가 없다.

## HWM-managed send 완료 계약

PAIR의 **send**와 DEALER/ROUTER의 **routed send**는 [분류 원칙](#분류-원칙)에
따라 ASYNC로 분류한다 — HWM 대기가 발생할 수 있기 때문이다. 같은 operation
entrypoint가 반환하는 builder에서 언어별 canonical terminal을 호출한다.
`send_async`, `sendCoroutine` 같은 별도 이름은 만들지 않는다.

- **C++**: `submit()`(blocking, `bool`을 반환하거나 `submit_error_t`를
  던지고 끝난다 — Core 내부에서 대기하다 Core 신호로 재개한다)과 `async()`
  (coroutine용, move-only `async_result_t<T>`를 반환한다) 두 terminal을 모두
  노출한다. 이름 구분 원칙에 따라 plain-thread 호출과 coroutine 호출을
  나눈 것이다.
- **.NET·Java·Node·Python·Rust**: 언어별 단일 비동기 terminal이 `Task`,
  `CompletionStage`, `Promise`, coroutine object, `Future` 같은 awaitable을
  반환한다. 사용자는 `await`/`join`/`block_on` 등 언어 관용 방식으로 소비한다.
- **Go**: `Submit(ctx) error`는 동기 terminal이며 Core 안에서 대기한다. `ctx`는
  submit 경계에서 확인하고, Core가 record를 수용한 뒤의 대기는 Core가 소유한다.
- **완료 구동.** C++ `async()`와 .NET·Java·Node·Python·Rust의 비동기 send
  terminal은 Core `zlink_send_async`를 사용한다. 먼저
  `zlink_send_complete_handler`를 설치하고, 수용된 operation의 socket-local
  operation id를 awaitable에 연결한다. `zlink_send_complete_event_t`는 admission,
  deadline 만료 또는 terminal 실패를 정확히 한 번 전달한다. 같은 target의 완료는
  제출 순서를 유지하고 한 socket의 completion callback은 서로 동시에 실행되지
  않는다. `ZLINK_POLLCOMPLETION` 등록은 같은 callback과 event의 dispatch 위치를
  `zlink_poller_wait` 호출 thread로 옮길 뿐 별도 완료 경로를 만들지 않는다.
  C++ `submit()`과 Go `Submit(ctx)`는 이 비동기 통지 대신 Core 내부 blocking
  대기·재개를 사용한다.
- 공개 send-ready handler는 없다. `ZLINK_POLLOUT`은 동기 nonblocking send의
  재시도를 위한 readiness 값이고, accepted async operation의 완료가 아니다.
- 바인딩은 이 완료 표면을 위해 park queue, WRITABLE-callback 재시도,
  deadline timer, dispatcher thread를 두지 않는다 — 완료는 Core
  send-completion 통지가 구동한다.
- submit flags는 언어 관용 방식(옵션 인자 또는 builder 단계)으로 받을 수
  있다.
- accepted async operation의 HWM 재시도는 Core가 소유한다. Binding은 재시도하지
  않으며, 최초 submit이 pending-operation 상한에서 즉시 거부된 경우의 정책만
  어플리케이션이 소유한다.

| 구분 | bindings 완료 표면 |
|---|---|
| 분류 | ASYNC (HWM 대기 가능) |
| C++ 실행 의미 | `submit()`은 Core 내부에서 대기하다 Core 신호로 재개하는 blocking 호출. `async()`는 coroutine에서 `co_await`하는 awaitable |
| 다른 언어 실행 의미 | .NET·Java·Node·Python·Rust는 언어 관용 awaitable을 반환한다. Go `Submit(ctx) error`는 Core 안에서 대기한다 |
| Blocking 모드(C++ `submit()`) | Core 내부에서 대기하다 Core 신호로 재개한다 (바인딩 대기 없음) |
| `SNDTIMEO` | Core가 대기 상한으로 사용한다 |
| `DONTWAIT` | Core가 즉시 `EAGAIN`을(언어별 `BACKPRESSURED`로) 반환한다 |
| completion 통지 | `zlink_send_complete_handler`가 accepted operation마다 `zlink_send_complete_event_t`를 정확히 한 번 전달한다. `ZLINK_POLLCOMPLETION`은 같은 callback의 dispatch 소유자만 바꾼다 |
| submit flags | 언어 관용 방식(옵션 인자 또는 builder 단계)으로 받을 수 있다 |
| 실패 | 언어 관용의 예외 또는 `Result`/`error` 반환 |
| 백프레셔 정책 | accepted operation은 Core가 재시도한다. 최초 submit 거부만 어플리케이션 정책이다 |

### Publish 동기 제출 계약

PUB/XPUB의 `publish`는 기본적으로 lossy다. subscriber의 queue가 HWM에
도달하면 해당 subscriber에게 보내는 copy를 drop하고 publisher는 즉시
진행하며, publisher는 HWM에서 절대 대기하지 않는다. 따라서 publish는
ASYNC 분류에 포함되지 않고 synchronous `submit()`만 제공한다.

`ZLINK_PUB_OPT_NODROP`(ZMQ `XPUB_NODROP`과 동등한 opt-in)을 설정하면
가득 찬 subscriber에 대한 publish가 동기 `submit()`에서 즉시
`BACKPRESSURED`/`EAGAIN` 오류가 된다. 재시도 여부와 정책은 어플리케이션이
소유한다. Core의 `zlink_send_async`는 PUB/XPUB에서 `ENOTSUP`을 반환하므로
publish async 표면은 없다. auto-HWM budget, per-queue caps, 수동
`sndhwm` 설정은 모두 subscriber별 drop threshold와 memory bound로 적용되며,
publisher 대기를 의미하지 않는다. 각 언어의 publish terminal과 실패 표현은
아래 정규 표의 raw reply 열과 같은 동기 submit 형태를 따른다.

- **C++ async 실패 매핑 (규범).** `async()`의 completion 결과가 `TIMED_OUT`
  또는 `TERMINAL`이면 `submit_error_t(submit_result_t::not_admitted,
  terminal_errno)`로 전달한다 — admission에 도달하지 못한 실패이므로
  `not_admitted`가 맞고, 원인은 `terminal_errno`가 보존한다.
- **flags는 동기 `submit()` 전용.** `DONTWAIT` 등 submit flags는 async
  terminal에서 의미가 없으므로 C++ `async()`는 non-zero flags를
  `EINVAL`로 거부한다.

## Request 완료 표면과 C++ 세 terminal

DEALER/ROUTER **request**도 [분류 원칙](#분류-원칙)에 따라 ASYNC로 분류한다.
같은 operation entrypoint가 반환하는 builder에서 terminal을 호출한다.
`requestCoroutine`, `request_async`, `submit_async` 같은 별도 이름은 만들지
않는다.

### C++ request의 세 terminal

C++ request builder는 세 개의 terminal을 노출한다.

1. **`submit()`** — blocking. Core-owned 대기 끝에 reply
   (`std::vector<message_t>`)를 직접 반환한다. timeout 만료는
   `ZLINK_REQUEST_TIMED_OUT`으로 알린다.
2. **`submit(callback)`** — 즉시 반환한다. Core의 reply callback이 app
   callback을 구동한다 — completion 전달만 담당하며, 바인딩은 재시도나
   스케줄링을 하지 않는다.
3. **`async()`** — coroutine용. move-only `async_result_t<T>`를 반환한다.
   framework canonical terminal이다 — framework는 coroutine-mandatory이므로
   오직 이 terminal만 사용한다.

다른 언어는 단일 terminal(`submit()`, .NET은 `Async(...)`)만 유지한다 —
이름 구분 원칙에 따라 C++만 이 세 terminal이 필요하고, 다른 언어는 그
terminal이 반환하는 awaitable이 이미 모든 소비 방식(await/join/block_on/
channel recv)을 지원하므로 별도 terminal을 늘릴 이유가 없다.

reply 완료는 Core가 구동한다: reply handler callback이 suspension·callback·
completion channel을 완료하고, 재개는 완료가 발생한 컨텍스트에서 일어난다.
request timeout은 이미 Core 소유다(`ZLINK_REQUEST_TIMED_OUT`). 바인딩은 이
완료 표면을 위해 재시도 큐나 전용 스레드를 두지 않는다.

| 구분 | bindings 완료 표면 |
|---|---|
| 분류 | ASYNC (HWM 대기 가능) |
| C++ terminal | `submit()`(blocking, reply 직접 반환) / `submit(callback)`(completion 전달) / `async()`(coroutine, framework canonical) |
| 다른 언어 terminal | 단일 `submit()`(또는 .NET `Async(...)`) — 언어 관용 awaitable 또는 completion channel 반환 |
| reply 전달 | suspension의 성공 값, callback 인자, 또는 channel completion |
| submit flags | request managed terminal은 받지 않는다 |
| timeout | builder의 `timeout(...)` 단계. 만료는 Core가 `ZLINK_REQUEST_TIMED_OUT`으로 통지한다 |
| submit 실패 | failed task/future/promise, error result, 예외 (C++ `submit()`은 던지는 예외) |
| reply 실패 | 같은 완료 표면의 실패 |
| 재개 컨텍스트 | Core가 완료를 전달한 컨텍스트(reply handler callback). 이후 실행 모델 연결은 framework 몫이다 |

## Send·Publish·Request·Raw reply 언어별 정규 표

아래 표는 네 operation 유형 — HWM-managed **send**(PAIR send와
DEALER/ROUTER routed send 포함), **publish**, **request**, **raw reply** —
각각에 대해 언어별 terminal과 반환/완료 표현을 정리한다. send와 request는
[분류 원칙](#분류-원칙)에 따라 ASYNC로, publish와 raw reply는 SYNC로
분류한다. framework는 이 표면을 감싸서 다른 실행 모델을 제공할 수 있지만,
bindings public API 이름을 늘리면 안 된다.

| Binding | HWM-managed send | Publish | Request | Raw reply |
|---|---|---|---|---|
| C | 해당 없음 — C ABI는 builder 정책을 적용하지 않으며 `core/include/zlink.h`의 함수형 계약을 따른다 | 해당 없음 | 해당 없음 | 해당 없음 |
| C++ | `submit()`(blocking) → PAIR/STREAM은 `bool`, routed는 `void`, 실패는 모두 `submit_error_t`를 던진다. `async()` → move-only `async_result_t<T>` | `submit()` → 동기 `bool`, 실패 시 `submit_error_t`를 던진다 | `submit()`(blocking) → reply, 실패 시 예외. `submit(callback)` → 즉시 반환, completion은 callback으로 전달. `async()` → `async_result_t<T>`(framework canonical) | `submit()` → `void`, 실패 시 `submit_error_t`를 던진다 |
| .NET | `Async()` → `Task`/`ValueTask`/`Task<T>`/`ValueTask<T>` | `Submit()` → 동기 `void`, 실패 시 `ZlinkSubmitException`을 던진다 | `Async(...)` → 위와 동일 | `Submit()` → 동기 `void`, 실패 시 `ZlinkSubmitException`을 던진다 |
| Java, Kotlin | `submit()` → `CompletionStage<T>` | `submit()` → 동기 `void`, 실패 시 `ZlinkSubmitException`을 던진다 | `submit()` → `CompletionStage<T>` (위와 동일) | `submit()` → 동기 `void`, 실패 시 `ZlinkSubmitException`을 던진다 |
| Node | `submit()` → `Promise<T>`(또는 `Promise<void>`) | `submit()` → 동기 `void`, 실패 시 `SubmitError`를 던진다 | `submit()` → `Promise<T>` (위와 동일) | `submit()` → 동기 `void`, 실패 시 `SubmitError`를 던진다 |
| Python | `submit()` → await 가능한 coroutine object | `submit()` → 동기 `None`, 실패 시 `SubmitError`를 발생시킨다 | `submit()` → 위와 동일 | `submit()` → 동기 `None`, 실패 시 `SubmitError`를 발생시킨다 |
| Go | `Submit(ctx)` → `error`(`nil` 성공) | `Submit(ctx)` → 동기 `error`(`nil` 성공) | `Submit(ctx)` → completion channel | `Submit(ctx)` → 동기 `error`(`nil` 성공) |
| Rust | `submit()` → `Future<Output = Result<(), SubmitError>>` | `submit()` → 동기 `Result<(), SubmitError>` | `submit()` → runtime 비종속 `Future<Output = Result<Vec<message_t>, ZlinkError>>` | `submit()` → 동기 `Result<(), SubmitError>` |

Kotlin이 Java binding을 직접 사용할 때도 위 Java 열의 계약을 그대로 따른다.
Kotlin Framework 표면(`.reply(...).await()` 등)은 [Framework typed Session
reply](#framework-typed-session-reply)에서 별도로 다룬다.

## Raw reply 동기 one-shot

Raw ROUTER/`Received` reply builder는 HWM-managed send나 publish, request가
아니다. [분류 원칙](#분류-원칙)에서 정의한 대로 raw reply는 HWM 경로를
전혀 거치지 않는 completion lane이며, 그래서 진짜 synchronous다.
종결자는 terminal reply 또는 error reply를 HWM 없는 completion lane에 native
호출 한 번으로 제출하고 동기적으로 끝난다. HWM backpressure는 reply 결과가
아니다. `NOT_CONNECTED`, `TERMINATED`, `INVALID_ARGUMENT` 또는 다른 non-HWM
submit 실패는 즉시 언어별 `SubmitError`로 전달한다.

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

framework는 bindings가 제공하는 완료 경계를 자기 실행 모델로 변환한다.
**framework는 coroutine-mandatory다** — 모든 framework 언어는 coroutine
또는 이에 준하는 async 실행 모델만 지원하며, blocking 실행 모델을 위한
별도 표면을 두지 않는다. **bindings는 executor를 제공하지 않지만,
HWM-managed send를 위해 framework가 executor를 새로 만들 필요도 없다** —
send의 awaitable(C++는 `async()`, 다른 언어는 `submit()`이 반환하는
awaitable)은 이미 Core send-completion 통지로 완료되므로, framework는 그
awaitable을 자기 coroutine에서 직접 `await`/`co_await`하면 된다. PUB/XPUB
publish는 awaitable이 아니며 framework가 동기 `submit()`을 직접 호출한다.
request도 이미 언어 native awaitable(또는 C++의 `async()`)을 반환하므로
감쌀 필요가 없다.

### Java framework

- Java framework는 managed request의 `submit()`으로 받은
  `CompletionStage`를 handler executor, virtual thread, timeout 정책에 연결한다.
- HWM-managed send의 `submit()`이 반환하는 `CompletionStage`도 managed
  request와 동일하게 그대로 연결한다 — Core send-completion 통지가 완료를
  구동하므로 별도 executor로 감쌀 필요가 없다.
- PUB/XPUB publish는 `submit()`을 동기 호출하고 즉시 반환되는 성공 또는
  실패를 처리한다. 이를 `CompletionStage`나 coroutine suspension으로
  연결하지 않는다.
- virtual thread를 쓰는 경우에도 bindings에 virtual thread 전용 recv나 submit API를
  추가하지 않는다.

### Kotlin framework

- Kotlin wrapper는 Java `await()`를 호출하지 않는다.
- Kotlin wrapper는 Java managed request builder의 `submit()`으로 `CompletionStage`를 얻고,
  `kotlinx-coroutines-jdk8`의 `await()`로 coroutine suspension에 연결한다.
- HWM-managed send도 request와 동일하게 Java `submit()`의 `CompletionStage`를
  `await()`로 coroutine suspension에 연결한다 — `Dispatchers.IO` 같은 별도
  dispatcher로 감쌀 필요가 없다(Core send-completion 통지가 이미 완료를
  구동하므로 blocking 호출이 아니다).
- PUB/XPUB publish는 동기 `submit()`을 직접 호출하고 즉시 반환되는 성공 또는
  실패를 처리한다. publish를 coroutine suspension으로 연결하지 않는다.
- Kotlin 사용자 코드와 Kotlin sample은 Java `submit()`을 직접 호출하지 않고
  `connect().await()`, `request(...).await<T>()`, `waitFor<T>(...).await()` 같은 Kotlin
  wrapper를 사용한다.
- coroutine scope, dispatcher, cancellation 처리는 Kotlin framework가 소유한다. bindings
  라이브러리는 scope나 dispatcher를 만들지 않는다.

### C++ framework

- C++ bindings는 managed request의 `async()`로 move-only
  `async_result_t<T>`를 제공하고 사용자와 framework coroutine은 이를 직접
  `co_await`한다.
- HWM-managed send도 동일하게 `async()`가 반환하는 `async_result_t<T>`를
  framework coroutine이 직접 `co_await`한다 — Core send-completion 통지가
  완료를 구동하므로 framework가 별도 executor에서 blocking `submit()`을
  감쌀 필요가 없다. framework는 send에 `submit()`(blocking)이 아니라
  `async()`만 사용한다.
- PUB/XPUB publish는 동기 `submit()`을 직접 호출한다. `async()`나
  `co_await`를 사용하지 않는다.
- standalone coroutine은 완료가 발생한 컨텍스트(Core reply handler callback,
  Core send-completion 통지 콜백 등)에서 재개될 수 있다. 이는 바인딩이
  완료·재개를 위한 자체 스레드, dispatcher, scheduler를 생성해도 된다는
  뜻이 아니다 — 실행 자원 소유는 framework의 몫이다. Framework `task_t`
  promise는 optional continuation scheduler hook으로 현재 serial turn과
  ambient context만 handoff한다.
- bindings public API에 coroutine 전용 `request_async`, `request_coroutine`, framework
  executor 인자, framework dispatcher 인자를 추가하지 않는다.

### 다른 언어 framework

- .NET framework는 managed request가 반환한 `Task` / `ValueTask`를 그대로
  `await`한다. HWM-managed send의 `Async()`가 반환하는 `Task`/`ValueTask`도
  동일하게 그대로 `await`한다 — `Task.Run(...)` 같은 executor로 감쌀 필요가
  없다. PUB/XPUB publish는 `Submit()`을 직접 호출하고 반환된 성공 또는
  실패를 처리한다.
- Node framework는 managed request가 반환한 `Promise`를 event loop 정책에
  맞게 `await`한다. HWM-managed send의 `submit()`이 반환하는 `Promise`도
  동일하게 직접 `await`한다 — worker offload로 감쌀 필요가 없다. PUB/XPUB
  publish는 동기 `submit()`을 직접 호출한다.
- Python framework는 managed request의 `submit()` coroutine object를 직접
  await한다. HWM-managed send의 `submit()` coroutine object도 동일하게 직접
  await한다 — `loop.run_in_executor(...)` 같은 실행 경로로 감쌀 필요가 없다.
  PUB/XPUB publish는 동기 `submit()`을 직접 호출한다.
- Rust framework는 managed request의 runtime 비종속 `submit()` Future를 자기
  executor에서 poll한다. HWM-managed send의 `submit()` Future도 동일하게
  poll한다 — `spawn_blocking` 등으로 감쌀 필요가 없다. PUB/XPUB publish는
  동기 `submit()`을 직접 호출한다.

이 방식이면 bindings는 C API wrapper로서의 책임을 유지하고, framework는 자기 실행 모델에
맞는 coroutine 지원을 독립적으로 제공할 수 있다. bindings는 어느 경우에도 스레드를
소유하지 않는다.
