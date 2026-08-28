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
| **request** | Core-구동 비동기 완료 (C++만 예외적으로 `submit()`/`submit(callback)`/`async()` 세 terminal) |
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
| [Request 완료 표면과 C++ 세 terminal](#request-완료-표면과-c-세-terminal) | HWM-managed request의 언어별 완료 표면과 C++의 세 terminal |
| [Send·Publish·Request·Raw reply 언어별 정규 표](#sendpublishrequestraw-reply-언어별-정규-표) | 네 operation 유형 각각의 언어별 terminal과 반환 타입 |
| [Raw reply 동기 one-shot](#raw-reply-동기-one-shot) | 일곱 binding의 reply 종결자와 즉시 실패 계약 |
| [Framework typed Session reply](#framework-typed-session-reply) | raw binding reply와 별개인 awaitable Framework 계약 |
| [Framework에서 비동기 완료를 붙이는 방법](#framework에서-비동기-완료를-붙이는-방법) | 언어별 framework가 완료 경계를 자기 실행 환경으로 바꾸는 방법 |

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
  (예외: HWM-managed send는 send flag 계약을 노출하기 위해 sync/async 두
  terminal을 갖는다 — 이름을 늘린 것이 아니라 flag 있는 SYNC 경로와 flag 없는
  ASYNC 경로를 나눈 것이다. [정규 표](#sendpublishrequestraw-reply-언어별-정규-표) 참조.)
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
   framework canonical terminal이다 — framework는 비동기 실행 전용이므로
   오직 이 terminal만 사용한다.

다른 언어는 단일 terminal(`submit()`, .NET은 `Async(...)`)만 유지한다 —
이름 구분 원칙에 따라 C++만 이 세 terminal이 필요하고, 다른 언어는 그
terminal이 반환하는 awaitable이 이미 모든 소비 방식(await/join/block_on/
channel recv)을 지원하므로 별도 terminal을 늘릴 이유가 없다.

reply 완료는 Core가 구동한다. Reply handler callback은 terminal과 payload를 한 번만
인수한다. 언어 future·promise가 완료 호출 thread에서 user continuation을 inline으로
실행할 수 있으면, binding은 이미 있는 completion dispatcher에 terminal 전달을 넘겨
native callback thread 밖에서 완료한다. Request timeout은 이미 Core 소유다
(`ZLINK_REQUEST_TIMED_OUT`). 바인딩은 이 완료 표면을 위해 admission·재시도 queue,
operation별 executor나 timer를 추가하지 않는다.

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
| C++ | sync `submit()`+`flags(int)` → PAIR/STREAM `bool`, routed `void`. async `async()` → `async_result_t<T>` | `submit()` → 동기 `bool`, 실패 시 `submit_error_t`를 던진다 | `submit()`(blocking) → reply, 실패 시 예외. `submit(callback)` → 즉시 반환, completion은 callback으로 전달. `async()` → `async_result_t<T>`(framework canonical) | `submit()` → `void`, 실패 시 `submit_error_t`를 던진다 |
| .NET | sync `Submit(SendFlags)` → `void`. async `Async()` → `Task`/`ValueTask`(`<T>`) | `Submit()` → 동기 `void`, 실패 시 `ZlinkSubmitException`을 던진다 | `Async(...)` → 위와 동일 | `Submit()` → 동기 `void`, 실패 시 `ZlinkSubmitException`을 던진다 |
| Java, Kotlin | sync `submit(SendFlags)` → `void`. async `submit()` → `CompletionStage<T>`. flag 파라미터로 overload가 성립해 이름을 나누지 않는다 | `submit()` → 동기 `void`, 실패 시 `ZlinkSubmitException`을 던진다 | `submit()` → `CompletionStage<T>` (위와 동일) | `submit()` → 동기 `void`, 실패 시 `ZlinkSubmitException`을 던진다 |
| Node | sync `submit(SendFlags)` → `void`. async `submit()` → `Promise<T>`(또는 `Promise<void>`) | `submit()` → 동기 `void`, 실패 시 `SubmitError`를 던진다 | `submit()` → `Promise<T>` (위와 동일) | `submit()` → 동기 `void`, 실패 시 `SubmitError`를 던진다 |
| Python | sync `submit_blocking(*, flags)` → `None`. async `submit()` → await 가능한 coroutine object | `submit()` → 동기 `None`, 실패 시 `SubmitError`를 발생시킨다 | `submit()` → 위와 동일 | `submit()` → 동기 `None`, 실패 시 `SubmitError`를 발생시킨다 |
| Go | sync `Submit(ctx)`+builder flag → `error`(`nil` 성공). async terminal 없음(Go 관례) | `Submit(ctx)` → 동기 `error`(`nil` 성공) | `Submit(ctx)` → completion channel | `Submit(ctx)` → 동기 `error`(`nil` 성공) |
| Rust | sync `submit_blocking(SendFlags)` → `Result<(), SubmitError>`. async `submit()` → `Future<Output = Result<(), SubmitError>>`. overload 없고 `async`가 예약어라 이름을 나눈다 | `submit()` → 동기 `Result<(), SubmitError>` | `submit()` → runtime 비종속 `Future<Output = Result<Vec<message_t>, ZlinkError>>` | `submit()` → 동기 `Result<(), SubmitError>` |

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

## Framework에서 비동기 완료를 붙이는 방법

framework는 bindings가 제공하는 완료 경계를 자기 실행 환경으로 변환한다.

- **framework는 비동기 실행 전용이다.** 모든 framework 언어는 자기 실행 환경
  (코루틴·가상 스레드·이벤트 루프)의 비동기 표면만 지원하며, blocking 전용 표면을
  따로 두지 않는다.
  bindings의 sync(+flags) send terminal은 binding 표면일 뿐 framework는 async terminal만
  쓴다.
- **framework는 executor를 새로 만들지 않는다.** bindings는 framework executor를 제공하지
  않지만, framework가 만들 필요도 없다. operation별로 다음을 그대로 쓴다.

| operation | framework 처리 |
|---|---|
| HWM-managed send | send의 awaitable(정규 표의 async terminal: C++ `async()`, .NET `Async()`, 그 밖 `submit()` 등)을 자기 비동기 실행 환경에서 직접 `await`/`co_await`한다. Core send-completion 통지가 완료를 구동한다 |
| publish | awaitable이 아니다. 동기 `submit()`을 직접 호출한다 |
| request | 이미 언어 native awaitable(C++ `async()`)을 반환하므로 감쌀 필요가 없다 |

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

이 방식이면 bindings는 C API wrapper로서의 책임을 유지하고, framework는 자기 실행 환경에
맞는 비동기 실행 지원을 독립적으로 제공할 수 있다. bindings는 admission 대기·재시도를
위한 스레드나 queue를 소유하지 않는다. 언어 runtime상 필요한 기존 completion
dispatcher는 Core callback 밖에서 terminal을 전달하는 경계만 소유한다.
