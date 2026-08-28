---
title: "바인딩 routed 전송 계약과 비동기 완료 표면 정책"
---

<!-- bindings-nav:start -->
[스펙 목록](README.ko.md) | [이전: 개요](README.ko.md) | [다음: C](c/README.ko.md)
<!-- bindings-nav:end -->

# 바인딩 routed 전송 계약과 비동기 완료 표면 정책

> **이 장이 정의하는 것** — C를 제외한 언어별 바인딩이 네 operation 유형의
> 완료 표면을 어떤 **이름과 반환 타입**으로 노출하는지 정한다.

이 문서가 다루는 네 operation 유형과 완료 표면:

| operation | 완료 표면 |
|---|---|
| **HWM-managed send** (PAIR send, DEALER/ROUTER routed send, `Received.send()`) | async terminal **과** sync(+flags) terminal (Go는 관례상 sync만) |
| **publish** (PUB/XPUB) | 동기 `submit()` |
| **request** | 세 terminal — sync 반환 / sync callback(+flags) / async. 모든 바인딩 |
| **raw reply** (ROUTER/`Received`) | HWM-free 동기 one-shot |

bindings 라이브러리는 core C API 위에 언어별 완료 경계를 제공한다. 실행 환경 연결(OS 스레드·가상 스레드·코루틴·이벤트 루프)과 handler dispatcher 연결은
framework가 맡는다.

> **용어.** 이 문서가 쓰는 동기/비동기, 실행 환경(스레드·코루틴·이벤트 루프),
> awaitable·terminal의 정의는 [비동기 실행 모델과 완료 표면 용어](async-execution-model.ko.md)를
> 따른다. "비동기(asynchronous)"는 스레드 기반 비동기까지 포함하는 상위어이며,
> "코루틴"은 실제 코루틴 언어를 특정할 때만 쓴다.

**bindings 라이브러리는 admission 대기·재시도용 스레드나 queue를 소유하지
않는다.** 수용 대기와 재시도는 Core가 소유한다. operation 유형별 완료 방식은
다음과 같다.

- **HWM-managed send / routed send** — Core가 수용 대기와 재시도를 소유한다.
  - C++ blocking `submit()`과 Go `Submit(ctx)`는 Core 안에서 대기한다.
  - 그 밖의 언어별 비동기 terminal은 `zlink_send_async`를 제출한 뒤
    `zlink_send_complete_handler`가 전달하는 최종 완료로 awaitable을 끝낸다.
  - Core가 수용한 operation은 `ZLINK_SEND_ADMITTED`, `ZLINK_SEND_TIMED_OUT`,
    `ZLINK_SEND_TERMINAL` 중 하나로 정확히 한 번 완료된다. Binding은 operation
    id와 언어별 완료 객체만 연결하며 재시도하지 않는다.
  - Core의 pending-operation 상한으로 **최초 제출이 거부된 경우에만** 어플리케이션이
    재시도 여부를 정한다.
- **publish** — HWM에서 대기하지 않으므로 동기 `submit()`으로 완료한다.
- **request** — Core 자신이 완료를 구동하는 지점(reply handler callback,
  `ZLINK_REQUEST_TIMED_OUT`)이 있다. 바인딩은 그 terminal을
  suspension·callback·completion channel에 연결할 뿐 자체 재시도나 admission
  queue를 두지 않는다.
  - 완료 호출이 user continuation을 inline 실행할 수 있는 언어는 이미 있는
    completion dispatcher로 native callback thread 밖에서 완료할 수 있다. request
    전용 executor나 scheduler를 추가한다는 뜻이 아니다.
  - 언어 관용의 suspension 객체는 binding 계약에 포함한다: Python `submit()`은
    await 가능한 coroutine object를, Rust `submit()`은 runtime 비종속 `Future`를
    반환한다. 이는 새 operation 시작점이나 framework executor가 아니다.

| 절 | 다루는 내용 |
|---|---|
| [분류 원칙](#분류-원칙) | HWM 대기 가능 여부로 send/request를 ASYNC로, publish/raw reply를 SYNC로 가르는 기준 |
| [공통 원칙](#공통-원칙) | operation 시작점 이름·builder·submit 실패 표현에 대한 공통 규칙 |
| [HWM-managed send 완료 계약](#hwm-managed-send-완료-계약) | HWM-managed send(routed send 포함)의 async 완료와 PUB/XPUB publish 동기 제출 계약 |
| [Request 완료 표면과 세 terminal](#request-완료-표면과-세-terminal) | HWM-managed request의 세 terminal(sync 반환/sync callback/async)과 언어별 표면 |
| [Send·Publish·Request·Raw reply 언어별 정규 표](#sendpublishrequestraw-reply-언어별-정규-표) | 네 operation 유형 각각의 언어별 terminal과 반환 타입 |
| [Raw reply 동기 one-shot](#raw-reply-동기-one-shot) | 일곱 binding의 reply 종결자와 즉시 실패 계약 |
| [Framework의 소비 규칙 (포인터)](#framework의-소비-규칙-포인터) | framework가 이 표면을 어떻게 쓰는지는 framework 스펙이 소유한다 |

## 분류 원칙

- **HWM 대기가 발생할 수 있는 조작은 ASYNC 함수로 분류한다.** PAIR send,
  DEALER/ROUTER의 routed send, request는 HWM 대기가 발생할 *수* 있는 지점을
  지나므로 ASYNC로 분류한다. 분류 기준은 "HWM 대기가 실제로 자주 발생하는가"가
  아니라 "HWM 대기가 발생할 가능성이 있는가"다.
  이 operation 분류가 모든 언어의 terminal 반환형을 강제하지는 않는다. Go는
  `Submit(ctx) error`가 Core 안에서 대기하는 동기 terminal이다.
- **raw reply는 HWM-free이며 진짜 synchronous다.** raw ROUTER/`Received`
  reply는 HWM 경로를 전혀 거치지 않으므로 순수 동기인 completion lane이다.
- **PUB/XPUB `publish`는 이 ASYNC 분류에 포함하지 않는다.** 기본 PUB
  의미론은 lossy drop이므로 subscriber queue가 HWM에 도달해도 해당 copy를
  drop하고 publisher는 즉시 진행한다. 따라서 HWM 대기가 없고, publish의
  동기 `submit()`이 terminal이다. `ZLINK_PUB_OPT_NODROP`에서는 가득 찬
  subscriber가 동기 submit에서 즉시 `BACKPRESSURED`/`EAGAIN`을 표면화하며,
  재시도 정책은 어플리케이션이 소유한다.
- 이 분류의 근거(HWM 대기 가능성)는 표면 계층과 무관하게 성립한다. framework
  표면이 이 분류를 어떻게 따르는지는 framework 스펙이 소유한다
  ([Framework의 소비 규칙](#framework의-소비-규칙-포인터) 참조).
  publish는 위의 lossy 계약에 따라 별도로 synchronous다.
- 이 원칙이 아래 모든 절의 근거다: C++가 send에 `submit()`/`async()`(또는
  request의 세 terminal)를 노출하는 것도, 다른 언어가 send·request에서 sync/async terminal을
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
- 하나의 part만 보내는 payload도 같은 builder를 사용한다. 호출자는
  `Message(...)`를 한 번 추가한 뒤 terminal을 호출한다. `Send(Message)`,
  `Send(RoutingId, Message)`, `Publish(topic, Message)` 같은 직접 single-part
  단축 API는 공개 계약에 추가하지 않는다. 구현은 내부 one-part fast path를
  선택할 수 있지만, 호출자가 고르는 별도 API가 되어서는 안 된다.
- bindings 라이브러리는 framework coroutine scheduler, Kotlin `CoroutineScope`,
  C++ framework executor를 소유하지 않는다. **바인딩 라이브러리는 admission
  대기열이나 재시도 정책도 소유하지 않는다** — send, publish, routed send,
  request든 마찬가지다. 언어 future·promise의 inline continuation이 Core callback에
  재진입하지 않도록 이미 있는 completion dispatcher에서 terminal을 전달하는 것은
  허용하지만, operation별 executor·queue·timer를 추가하지 않는다.
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
  (예외: HWM-managed **send**와 **request**는 admission flag 계약을 노출하기 위해
  sync/async terminal을 함께 갖는다 — 이름을 늘린 것이 아니라 flag 있는 SYNC 경로와
  flag 없는 ASYNC 경로를 나눈 것이다. [정규 표](#sendpublishrequestraw-reply-언어별-정규-표) 참조.)

  **sync 종결자 naming 통일** — async 종결자가 `submit()`인 언어(Java·Node·Python·Rust)는
  그 sync 종결자를 **`submit_sync`**로 통일한다. Java는 반환 타입만 다른 overload가
  불가능하므로 어차피 이름을 나눠야 하고, 나머지도 대칭을 위해 맞춘다. **`submit_sync`는
  sync/async 축의 이름**이며([async-execution-model](async-execution-model.ko.md): sync/async는
  blocking/non-blocking과 다른 축), 실제 blocking 여부는 flag(`NONE`/`DONTWAIT`)가 정한다 —
  그래서 `submit_blocking`이 아니라 `submit_sync`다(DONTWAIT이면 non-blocking이므로).
  C++(`async()`/`submit()`)·.NET(`Async()`/`Submit()`)·Go(`Submit(ctx)`)는 async가 `submit()`이
  아니므로 각자 관례를 유지한다.
  async-classified operation의 terminal이 반환하는 awaitable은 각 언어가
  관용적인 방식(await / join / block_on / channel recv)으로 소비하고,
  synchronous publish와 raw reply의 terminal은 호출 즉시 소비한다. 같은
  원칙에 따라 Go의 send `Submit(ctx)`는 `error`를 반환하는 동기형이다.
  goroutine 안에서의 blocking 호출이 Go의 관용적 대기 방식이고
  `context.Context`는 제출 전 취소와 시한을 확인한다.
- **request builder는 모든 바인딩에서 세 terminal을 노출한다** — sync 반환(blocking,
  reply 직접 반환) · sync callback(즉시 admission 결과 + reply는 callback) · async(awaitable).
  request 제출이 send처럼 HWM admission을 지나므로, C의 `DONTWAIT` 연속 제출 모델을 각
  바인딩이 공개 표면으로 표현하려면 admission을 reply와 분리하는 sync callback 표면이
  필요하다(0.14.0). 이름은 위 naming 통일을 따른다: C++ `submit()`/`submit(callback)`/`async()`,
  .NET `Submit(SendFlags)`/`Submit(SendFlags, callback)`/`Async()`, Java·Node·Python·Rust
  `submit_sync(flags)`/`submit_sync(flags, callback)`/`submit()`, Go는 `Submit(ctx)`(+Flags)와
  completion channel로 fire-and-collect(별도 callback 없음). [정규 표](#sendpublishrequestraw-reply-언어별-정규-표) 참조.

## HWM-managed send 완료 계약

이 절은 send 계열(PAIR **send**, DEALER/ROUTER **routed send**,
`Received.send()`)의 **완료 방식과 공통 계약**을 정의한다. 언어별 terminal 이름과
반환 타입은 [정규 표](#sendpublishrequestraw-reply-언어별-정규-표)가 소유하며 여기서
반복하지 않는다.

send 계열은 HWM 대기가 발생할 수 있으므로 대부분의 binding이 **두 terminal**(sync/async)을 노출한다. **Go는 관례상 sync terminal만** 두며(정규 표 참조), 이는 계약 위반이 아니라 명시된 언어별 예외다.

- **sync terminal**: send flag를 받는다. flag 없거나 `NONE`이면 Core 내부에서
  blocking 대기하고, `DONTWAIT`면 즉시 backpressure(`BACKPRESSURED`/`EAGAIN`)를
  반환한다.
- **async terminal**: flag를 받지 않는다. `zlink_send_async` 완료 통지로 awaitable을
  끝낸다. (C++은 `submit()`(sync)/`async()`로, Go는 sync `Submit(ctx)`만 두는 등
  언어별 구성은 정규 표를 따른다.)

이름을 늘리는 것이 아니다 — flag 있는 SYNC 경로와 flag 없는 ASYNC 경로를 나눈
것이며, `send_async`·`sendCoroutine` 같은 별도 이름은 만들지 않는다.

### 완료 통지 (공통)

- async terminal은 먼저 `zlink_send_complete_handler`를 설치한다. 즉시 admission은
  operation id `0`을 반환하고 binding이 awaitable을 즉시 완료한다. HWM으로 대기하면
  nonzero operation id를 awaitable에 연결하며, `zlink_send_complete_event_t`가
  `ZLINK_SEND_ADMITTED`·`ZLINK_SEND_TIMED_OUT`·`ZLINK_SEND_TERMINAL` 중 하나로 정확히
  한 번 전달한다.
- 같은 target의 완료는 제출 순서를 유지하고, 한 socket의 completion callback은 서로
  동시에 실행되지 않는다. `ZLINK_POLLCOMPLETION` 등록은 같은 callback과 event의
  dispatch 위치를 `zlink_poller_wait` 호출 thread로 옮길 뿐 별도 완료 경로를 만들지
  않는다.
- Binding은 operation id와 언어별 완료 객체만 연결하며 재시도하지 않는다. accepted
  operation의 HWM 재시도는 Core가 소유한다. **최초 제출이 pending-operation 상한에서
  거부된 경우에만** 어플리케이션이 재시도 여부를 정한다.
- 공개 send-ready handler는 없다. `ZLINK_POLLOUT`은 동기 nonblocking send의 재시도를
  위한 readiness 값이고 accepted async operation의 완료가 아니다.
- 바인딩은 이 완료 표면을 위해 park queue, WRITABLE-callback 재시도, deadline timer를
  두지 않는다. callback 안에서 continuation이 다음 submit을 호출할 수 있는 C++ 기본
  coroutine은 pending slow path에서 continuation dispatcher를 거쳐 callback 밖에서
  재개한다.

### 구현 서술 (native 경로)

- async terminal은 native `zlink_send_async`로 제출한다 — 이 함수는 항상 nonblocking
  이고 flags 필드가 없다.
- sync terminal은 native `zlink_send_part(_rid)` + send flag로 제출한다 — flag 없으면
  Core 내부에서 blocking 대기하고, `DONTWAIT`면 즉시 `EAGAIN`(언어별 `BACKPRESSURED`)을
  반환한다.
- 따라서 `DONTWAIT`가 필요한 호출은 async terminal이 아니라 sync terminal을 쓴다.
- (native 경로가 바뀌면 이 절의 문장을 코드에 맞춘다.)

### 계약 요약 (공통)

| 항목 | 계약 |
|---|---|
| 분류 | HWM 대기 가능 — sync·async 두 terminal |
| `SNDTIMEO` | sync blocking의 대기 상한으로 Core가 사용한다 |
| `DONTWAIT` | sync terminal에서 Core가 즉시 `EAGAIN`(언어별 `BACKPRESSURED`)을 반환한다. async terminal은 flag가 없다(`zlink_send_async`에 flags 필드가 없다) |
| completion 통지 | `zlink_send_complete_handler`가 accepted operation마다 `zlink_send_complete_event_t`를 정확히 한 번 전달한다. `ZLINK_POLLCOMPLETION`은 dispatch 소유자만 바꾼다 |
| 실패 | 언어 관용의 예외 또는 `Result`/`error` 반환 |
| 백프레셔 정책 | accepted operation은 Core가 재시도한다. 최초 submit 거부만 어플리케이션 정책이다 |

### Publish 동기 제출 계약

PUB/XPUB의 `publish`는 HWM에서 대기하지 않으므로 ASYNC 분류에 포함되지 않고
**동기 `submit()`만** 제공한다. 규칙은 다음과 같다.

- **기본은 lossy.** subscriber의 queue가 HWM에 도달하면 그 subscriber에게 보내는
  copy를 drop하고 publisher는 즉시 진행한다. publisher는 HWM에서 절대 대기하지 않는다.
- **`ZLINK_PUB_OPT_NODROP`**(ZMQ `XPUB_NODROP`과 동등한 opt-in): 가득 찬 subscriber에
  대한 publish가 동기 `submit()`에서 즉시 `BACKPRESSURED`/`EAGAIN` 오류가 된다.
  재시도 여부와 정책은 어플리케이션이 소유한다.
- **async 표면 없음.** Core의 `zlink_send_async`는 PUB/XPUB에서 `ENOTSUP`을 반환한다.
- **HWM 설정의 의미.** auto-HWM budget, per-queue caps, 수동 `sndhwm`은 모두
  subscriber별 drop threshold와 memory bound로 적용되며 publisher 대기를 뜻하지 않는다.
- **terminal 형태.** 각 언어의 publish terminal과 실패 표현은 정규 표의 raw reply 열과
  같은 동기 submit 형태를 따른다.

- **C++ async 실패 매핑 (규범).** `async()`의 completion 결과가 `TIMED_OUT`
  또는 `TERMINAL`이면 `submit_error_t(submit_result_t::not_admitted,
  terminal_errno)`로 전달한다 — admission에 도달하지 못한 실패이므로
  `not_admitted`가 맞고, 원인은 `terminal_errno`가 보존한다.
- **flags는 동기 `submit()` 전용.** `DONTWAIT` 등 submit flags는 async
  terminal에서 의미가 없으므로 C++ `async()`는 non-zero flags를
  `EINVAL`로 거부한다.

## Request 완료 표면과 세 terminal

DEALER/ROUTER **request**도 [분류 원칙](#분류-원칙)에 따라 ASYNC로 분류한다.
같은 operation entrypoint가 반환하는 builder에서 terminal을 호출한다.
`requestCoroutine`, `request_async`, `submit_async` 같은 별도 이름은 만들지
않는다.

**request 제출은 send와 동일하게 HWM admission을 지난다.** request 메시지 자체가
send 경로로 전송되므로, request 제출도 admission 대기(HWM)를 만날 수 있다. 따라서
request의 sync terminal도 send와 **동일한 admission flag 계약**을 갖는다 —
`NONE`이면 admission까지 대기, `DONTWAIT`이면 즉시 `BACKPRESSURED`/`EAGAIN`. reply
완료는 admission과 별개로 Core가 구동한다(`ZLINK_REQUEST_TIMED_OUT`).

### 세 terminal (모든 바인딩)

request builder는 세 완료 표면을 노출한다. 이름은 [naming 규칙](#공통-원칙)을 따른다 —
async 종결자가 `submit()`인 언어(Java·Node·Python·Rust)는 sync를 `submit_sync`로,
C++는 `submit()`/`submit(callback)`·`async()`, .NET은 `Submit(...)`·`Async(...)`,
Go는 `Submit(ctx)`를 쓴다.

1. **sync 반환** — blocking. admission flag를 받고, Core-owned 대기 끝에 reply를
   **직접 반환**한다. `NONE`이면 admission까지 대기 후 reply를 기다린다. timeout 만료는
   `ZLINK_REQUEST_TIMED_OUT`으로 알린다. 이름: Java·Node·Python·Rust `submit_sync(flags)`,
   C++ `submit()`, .NET `Submit(SendFlags)`, Go `Submit(ctx)`(+Flags).
2. **sync callback** — 제출 즉시 **admission 결과를 동기로 반환**하고(`DONTWAIT`이면
   즉시 `BACKPRESSURED`) reply는 나중에 **callback**으로 전달한다. reqrep을 직렬화 없이
   연속 제출할 때 쓰는 표면이다. 바인딩은 재시도·스케줄링을 하지 않는다. 이름:
   Java·Node·Python·Rust `submit_sync(flags, callback)`, C++ `submit(callback)`,
   .NET `Submit(SendFlags, callback)`. Go는 관용적으로 `Submit(ctx)`가 돌려주는
   completion channel로 fire-and-collect한다(별도 callback 메서드 없음).
3. **async** — coroutine/awaitable용. 언어 awaitable(C++ `async_result_t<T>`, .NET
   `Task`, Java `CompletionStage`, Node `Promise`, Python coroutine object, Rust
   `Future`, Go completion channel)을 반환한다. flag를 받지 않는다.
   (framework가 어느 terminal을 쓰는지는 framework 스펙이 소유한다 — 아래 포인터.)

sync 반환과 sync callback은 **admission flag를 받고**, async는 받지 않는다(send와
동일). request 제출의 admission backpressure는 sync callback(또는 Go channel)으로
표현하고, reqrep의 outstanding 깊이는 코드 상한이 아니라 이 admission backpressure(HWM)가
결정한다.

reply 완료는 Core가 구동한다. Reply handler callback은 terminal과 payload를 한 번만
인수한다. 언어 future·promise가 완료 호출 thread에서 user continuation을 inline으로
실행할 수 있으면, binding은 이미 있는 completion dispatcher에 terminal 전달을 넘겨
native callback thread 밖에서 완료한다. Request timeout은 이미 Core 소유다
(`ZLINK_REQUEST_TIMED_OUT`). 바인딩은 이 완료 표면을 위해 admission·재시도 queue,
operation별 executor나 timer를 추가하지 않는다.

| 구분 | bindings 완료 표면 |
|---|---|
| 분류 | ASYNC (HWM 대기 가능) |
| terminal (모든 바인딩) | **sync 반환**(blocking, reply 직접 반환) / **sync callback**(즉시 admission 결과, reply는 callback) / **async**(awaitable) |
| 언어별 이름 | C++ `submit()`/`submit(callback)`/`async()` · .NET `Submit(SendFlags)`/`Submit(SendFlags, cb)`/`Async()` · Java·Node·Python·Rust `submit_sync(flags)`/`submit_sync(flags, cb)`/`submit()` · Go `Submit(ctx)`(+Flags)+channel |
| reply 전달 | sync 반환의 반환값, callback 인자, suspension 성공 값, 또는 channel completion |
| submit flags | **sync 반환·sync callback은 admission flag를 받는다**(`NONE` admission 대기 / `DONTWAIT` 즉시 backpressure, send와 동일). async terminal은 받지 않는다 |
| timeout | builder의 `timeout(...)` 단계. 만료는 Core가 `ZLINK_REQUEST_TIMED_OUT`으로 통지한다 |
| submit 실패 | failed task/future/promise, error result, 예외 (C++ `submit()`은 던지는 예외) |
| reply 실패 | 같은 완료 표면의 실패 |
| 재개 컨텍스트 | 언어 binding의 completion 컨텍스트. Inline continuation 언어는 native reply callback 밖의 기존 completion dispatcher에서 완료할 수 있으며, 이후 실행 모델 연결은 framework 몫이다 |

## Send·Publish·Request·Raw reply 언어별 정규 표

아래 표는 네 operation 유형 — HWM-managed **send**(PAIR send와
DEALER/ROUTER routed send 포함), **publish**, **request**, **raw reply** —
각각에 대해 언어별 terminal과 반환/완료 표현을 정리한다. HWM-managed send는
HWM 대기가 발생할 수 있으므로 **async terminal과 sync(+flags) terminal 두
표면**을 갖는다 — async는 대기를 비동기로 다루고, sync는 send flag로 대기 여부를
정한다(기본 blocking, `DONTWAIT`로 즉시 backpressure). request는 [분류 원칙]
(#분류-원칙)에 따라 ASYNC, publish와 raw reply는 SYNC로 분류한다. framework는 이
표면을 감싸서 다른 실행 모델을 제공할 수 있지만, bindings public API 이름을 늘리면
안 된다. **HWM-managed send에 sync terminal을 더하는 것은 이름을 늘리는 것이
아니라 이미 있는 flag 계약(SYNC nonblocking send)을 각 언어 표면에 노출하는
것이다.**

표의 send 셀은 **언어별로 다른 것 — terminal 이름과 반환 타입 — 만** 담는다.
모든 언어에 공통인 규칙은 다음과 같으며 셀에 반복하지 않는다.

- **sync terminal**: send flag를 받는다. flag 없거나 `NONE`이면 기본 blocking(HWM
  대기), `DONTWAIT`면 즉시 backpressure(`BACKPRESSURED`/`EAGAIN`).
- **async terminal**: flag를 받지 않는다. HWM 대기와 재시도는 Core가 소유한다.
- 실패는 언어 관용대로 전달한다(예외 또는 `Result`/`error` 반환).

`Received.send()`가 반환하는 SendOp도 HWM-managed send이므로 이 표의 send 계약을
그대로 따른다. `reply()`는 send 계열이 아니다 — HWM에 걸리지 않고 별도 completion
lane으로 전송되므로 raw reply 열의 동기 terminal 하나만 가지며 send flag가 없다.

| Binding | HWM-managed send | Publish | Request | Raw reply |
|---|---|---|---|---|
| C | 해당 없음 — C ABI는 builder 정책을 적용하지 않으며 `core/include/zlink.h`의 함수형 계약을 따른다 | 해당 없음 | 해당 없음 | 해당 없음 |
| C++ | sync `submit()`+`flags(int)` → PAIR/STREAM `bool`, routed `void`. async `async()` → `async_result_t<T>` | `submit()` → 동기 `bool`, 실패 시 `submit_error_t`를 던진다 | `submit()`(blocking) → reply, 실패 시 예외. `submit(callback)` → 즉시 반환, completion은 callback으로 전달. `async()` → `async_result_t<T>` | `submit()` → `void`, 실패 시 `submit_error_t`를 던진다 |
| .NET | sync `Submit(SendFlags)` → `void`. async `Async()` → `Task`/`ValueTask`(`<T>`) | `Submit()` → 동기 `void`, 실패 시 `ZlinkSubmitException`을 던진다 | sync 반환 `Submit(SendFlags)` → reply. sync callback `Submit(SendFlags, callback)` → 즉시 admission. async `Async(ct)` → `Task`. 실패 시 `ZlinkSubmitException` | `Submit()` → 동기 `void`, 실패 시 `ZlinkSubmitException`을 던진다 |
| Java, Kotlin | sync `submit_sync(SendFlags)` → `void`. async `submit()` → `CompletionStage<T>` | `submit()` → 동기 `void`, 실패 시 `ZlinkSubmitException`을 던진다 | sync 반환 `submit_sync(SendFlags)` → reply. sync callback `submit_sync(SendFlags, callback)` → 즉시 admission. async `submit()` → `CompletionStage<T>`. 실패 시 `ZlinkSubmitException` | `submit()` → 동기 `void`, 실패 시 `ZlinkSubmitException`을 던진다 |
| Node | sync `submit_sync(SendFlags)` → `void`. async `submit()` → `Promise<T>`(또는 `Promise<void>`) | `submit()` → 동기 `void`, 실패 시 `SubmitError`를 던진다 | sync 반환 `submit_sync(SendFlags)` → reply. sync callback `submit_sync(SendFlags, callback)` → 즉시 admission. async `submit()` → `Promise<T>`. 실패 시 `SubmitError` | `submit()` → 동기 `void`, 실패 시 `SubmitError`를 던진다 |
| Python | sync `submit_sync(*, flags)` → `None`. async `submit()` → await 가능한 coroutine object | `submit()` → 동기 `None`, 실패 시 `SubmitError`를 발생시킨다 | sync 반환 `submit_sync(*, flags)` → reply. sync callback `submit_sync(*, flags, callback)` → 즉시 admission. async `submit()` → coroutine object. 실패 시 `SubmitError` | `submit()` → 동기 `None`, 실패 시 `SubmitError`를 발생시킨다 |
| Go | sync `Submit(ctx)`+builder `Flags` → `error`(`nil` 성공). async terminal 없음(Go 관례) | `Submit(ctx)` → 동기 `error`(`nil` 성공) | `Flags(...).Submit(ctx)` → admission 결과(`error`) + reply는 completion channel. channel로 fire-and-collect(별도 callback 없음) | `Submit(ctx)` → 동기 `error`(`nil` 성공) |
| Rust | sync `submit_sync(SendFlags)` → `Result<(), SubmitError>`. async `submit()` → `Future<Output = Result<(), SubmitError>>` | `submit()` → 동기 `Result<(), SubmitError>` | sync 반환 `submit_sync(SendFlags)` → reply. sync callback `submit_sync(SendFlags, callback)` → 즉시 admission. async `submit()` → runtime 비종속 `Future<Output = Result<Vec<message_t>, ZlinkError>>` | `submit()` → 동기 `Result<(), SubmitError>` |

Kotlin이 Java binding을 직접 사용할 때도 위 Java 열의 계약을 그대로 따른다.
Kotlin Framework 표면(`.reply(...).await()` 등)은 framework 스펙이 소유한다
([Framework의 소비 규칙](#framework의-소비-규칙-포인터) 참조).

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
그대로 사용한다. Kotlin Framework `reply(...).await()`는 framework 스펙이 소유하는 별개 API다.

## Framework의 소비 규칙 (포인터)

Framework가 이 문서의 binding 표면(send·publish·request·raw reply terminal) 중
무엇을 어떤 규칙으로 소비하는지 — async terminal 전용 원칙, sync terminal이
정당한 예외(즉시 backpressure 관찰, 공개 동기 계약의 구현), typed Session reply
표면 — 는 framework 스펙
[제출과 완료 §15 「Binding send terminal 소비」](../../../framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md#15-binding-send-terminal-소비-구현)가
소유한다. 이 문서는 binding이 무엇을 제공하는지만 정의한다.
