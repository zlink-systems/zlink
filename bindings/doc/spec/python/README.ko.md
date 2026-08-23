---
title: "Python 바인딩 공개 계약"
---

<!-- bindings-nav:start -->
[스펙 목록](../README.ko.md) | [이전: Node.js](../node/README.ko.md) | [다음: Go](../go/README.ko.md)
<!-- bindings-nav:end -->

# Python binding Core 0.13.0 공개 계약

> **이 장이 정의하는 것** — `zlink` Python package가 Core 0.13.0 raw messaging 위에 제공하는
> 공개 타입·소유권·오류 계약.

- 이 문서는 `zlink` Python package가 제공하는 Core 0.13.0 raw messaging 계약을 정의한다.
- 현재 구현과 공개 header에 없는 기능은 이 문서의 계약이 아니다.
- Python 3.9 이상을 지원하며, 현재 candidate package version은 `0.13.0`이다.
- 현재 native package target은 Linux x86_64이며, 다른 target은 별도 candidate payload와 clean consumer 검증 전까지 이 계약의 지원 범위가 아니다.

| 절 | 다루는 내용 |
|---|---|
| [범위](#범위) | Core 자원을 표현하는 Python 공개 type 목록 |
| [Package 표면](#package-표면) | public factory와 private 영역 경계 |
| [Byte HWM과 Auto-HWM](#byte-hwm과-auto-hwm) | Python `int`와 Core `uint64_t` byte HWM의 매핑 |
| [소유권과 수명](#소유권과-수명) | native handle·message·Received의 소유·해제 규칙 |
| [Callback 표면](#callback-표면) | 공개 callback 경로와 노출하지 않는 primitive |
| [송수신과 no-data](#송수신과-no-data) | submit·no-data 표현과 native failure 전달 |
| [Receive flow state](#receive-flow-state) | receive-flow 상태 타입, setter와 monitor 표면 |
| [Error](#error) | `ZlinkError` 계열과 result 필드 |
| [Python version과 type package](#python-version과-type-package) | 지원 Python 버전과 type check 대상 |
| [관련 문서](#관련-문서) | guide·Core spec·internals 링크 |

## 범위

Python binding은 다음 Core 자원을 Python object와 Protocol로 표현한다.

| 영역 | 공개 개념 |
|---|---|
| Core | `Context`, `ContextOptions`, `Message`, `Received`, `RoutingId` |
| Socket | PAIR, DEALER, ROUTER, STREAM, PUB, SUB, XPUB, XSUB |
| Eventing | `MonitorSocket`, `MonitorEvent`, `MonitorStatus`, `Poller`, `PollEvents`, `Timer` |
| Utility | `AtomicCounter`, `Stopwatch`, `Thread`, `proxy`, `sleep` |
| Result | `SubmitResult`, `RequestResult`, `RecvResult`, `ConfigResult`와 대응 error |

Socket은 Core의 raw endpoint와 message routing 의미를 그대로 보존한다. Python binding은 Core 내부
handle, FFI symbol, native struct를 public type으로 노출하지 않는다.

## Package 표면

사용자는 `zlink` package root의 factory와 contract type을 사용한다. 구현 모듈을 직접 import하지
않으며 `_native`와 `_runtime`은 private 영역이다. package root에는 Core raw contract에 속하지 않는
별도 domain type이나 compatibility alias가 없다.

주요 factory는 `create_context()`, `create_pair_socket()`, `create_dealer_socket()`,
`create_router_socket()`, `create_stream_socket()`, `create_pub_socket()`, `create_sub_socket()`,
`create_poller()`, `create_timer()`, `create_received()`와 `create_message` 계열이다.
정확한 Python signature는 같은 디렉터리의 contract module과 public header를 함께 기준으로 한다.

## Byte HWM과 Auto-HWM

HWM의 계산과 queue admission은 Core가 담당한다. Python의
`send_high_water_mark`와 `receive_high_water_mark`는 byte 단위의 `int`이며
음수가 아닌 `uint64_t` 범위만 허용한다. 바인딩은 값을 정확히 8-byte option으로
Core에 전달하고 getter도 Core의 64-bit 값을 Python `int`로 반환한다. 값 `0`은
무제한이다.

Context는 byte 단위 `core_hwm_memory_limit_bytes`, `core_hwm_budget_bytes`와
`core_hwm_profile`을 Core에 그대로 전달한다. Profile 비율 계산과 physical directional
queue별 분배는 Core가 정확히 한 번 수행한다. Caller가 방향별 HWM을 설정하면 그 방향은
manual override가 되어 Auto-HWM 재계산에서 제외된다.
Context는 `core_hwm_budget_snapshot()`과 `reset_core_hwm_budget_metrics()`도 제공한다.
입력 우선순위는 수동 Core budget, 명시 memory limit, 명확한 별도 VM hard limit을 얻을 수
있을 때의 runtime hint, Core fallback 순서다. 앞의 두 값을 지정하면 runtime hint를 자동
감지하지 않는다. Binding은 hint와 Core hard limit을 직접 결합하지 않는다. 명시 입력이
Core가 감지한 finite hard limit보다 크면 `EINVAL`에 대응하는 기존 config error를 그대로
전달하고 clamp하지 않는다.

실제 pipe에 쌓인 accounted byte가 applied HWM에 도달하면 Core가
backpressure를 결정한다. Python 바인딩은 message 수를 다시 세지 않으며 native
result를 기존 submit·error 계약으로 전달한다.
`monitor_open(events=..., monitor_hwm_bytes=...)`는 `uint64_t` 범위의 음수가 아닌
Python `int`를 받는다. `0`은 Core monitor 기본값을 선택하고, 양수는 변환 없이
전달한다. Message-count alias나 변환은 없다. `MonitorStatus`의 planned,
applied, deferred HWM과 in-flight 사용량은 byte 단위의 Python `int`다. Pending message
count는 표시용 진단이고 `snd_pending_bytes`와 `rcv_pending_bytes`는 별도 byte 값이다.
slot·message-unit·size-cap·connection-bucket property는 제공하지 않는다.

Core budget snapshot은 ABI version/size, configured/runtime/resolved memory limit,
configured/effective budget, planned/applied/manual-reserved HWM, Core queue/application/current/
peak/provisional accounted byte, completion current/peak/pending과 total messaging byte,
monitor/instance aggregate, application/completion queue count,
`outstanding_application_lease_count`, `retired_queue_count`, `deferred_origin_credit_bytes`,
oversize·blocked·aggregate flag, `budget_generation`과 `measurement_epoch`을 Python
`int`/boolean으로 제공한다. Reset은 current·pending·queue count와 위 세 owner-lifecycle
gauge를 유지하고 두 peak를 current로 재기준화하며 epoch counter를 0으로 만든 뒤
`measurement_epoch`을 증가시킨다. ABI version/size 불일치는 unsupported error다.

## 소유권과 수명

- `Context`가 native context를 소유하며 `close()` 또는 context manager 종료로 해제한다.
- socket, monitor, poller와 timer는 각각 만든 native handle을 소유한다. `close()` 성공 뒤 handle을
  사용하는 호출은 허용하지 않는다.
- `Message.from_(value)`는 caller 값에서 독립된 native message를 만든다. send submit 성공 뒤
  message part의 native 소유권은 Core send 경로로 이동한다.
- `Received`는 caller가 만든 수신 저장 공간이다. `recv_into(received)` 성공 시 parts와 routing
  metadata가 `Received`에 기록되고, `close()` 또는 context manager 종료 시 native parts를 해제한다.
- `Received`의 `parts`가 제공하는 native view는 owner가 열린 동안만 유효하다. 다른 수명으로 넘겨야
  하면 `to_bytes()` 또는 `to_bytes_list()`로 값을 복사한다.
- callback을 등록하면 callback과 필요한 Python 참조는 native callback 등록보다 먼저 해제되지 않는다.
  callback 예외는 binding의 callback error policy에 따라 전달된다.

일반 `recv_into`와 `subscribe_into`는 Core에서 part를 dequeue할 때 queue credit을 즉시
반환한다. Framework backend만 `recv_retained_into`와 `subscribe_retained_into`를
명시적으로 선택한다. 두 retained 경로는 일반 경로와 같은 `Received`/`TopicMessage`,
routing id, request sequence, topic과 multipart framing을 보존하면서 caller-visible
physical part마다 Core credit 하나를 aggregate owner가 private하게 보유한다.

Retained 결과의 `close()`, context manager 종료 또는 같은 저장소의 다음 retained
수신 시작은 native part와 모든 credit을 정확히 한 번 반환한다. 따라서 재사용 수신이
no-data로 끝나도 이전 결과는 남지 않는다. Framework의 drop·cancel·error 정상 경로도
소유한 aggregate를 명시적으로 닫아야 하며, Python reference count/GC cleanup은 누락 방지
fallback이다. 개별 `ReceivedMessage`와 public API에는 raw lease handle, 별도 application
capacity, allowance나 중복 accounting 상태를 노출하지 않는다.

## Callback 표면

Core FFI의 `zlink_recv_handler()`는 Python package가 직접 노출하지 않는 private 구현
primitive다. Python의 공개 callback 표면은 STREAM packet의 `on_packet`,
send readiness의 `on_send_ready`, monitor event의 `on_event`로 고정한다. Raw receive나
routed request completion callback을 등록하는 별도 public method는 제공하지 않는다.

## 송수신과 no-data

- PAIR·PUB·STREAM 송신과 ROUTER reply 같은 unrelated 동기 builder는 message part를 추가한 뒤
  `submit()`한다. Raw ROUTER/`Received` reply의 `submit()`은 `None`을 반환하는 동기
  one-shot이며 terminal reply 또는 error reply를 HWM 없는 completion lane에 native 호출
  한 번으로 제출한다. HWM backpressure는 reply 결과가 아니며 `NOT_CONNECTED`,
  `TERMINATED`, `INVALID_ARGUMENT`와 그 밖의 non-HWM submit 실패는 즉시 `SubmitError`로
  발생시킨다. Blocking operation은 socket option과 Core timeout 계약을 따른다.
- DEALER·ROUTER routed send와 request builder의 유일한 terminal은 `submit()`이다.
  `await dealer.send().message(message).submit()`과
  `reply = await dealer.request().message(request).submit()`처럼 사용하며, `submit()`은 await 가능한
  coroutine object를 즉시 반환한다. 이 메서드는 반환 전에 native blocking submit을 실행하지 않는다.
  Socket runtime은 operation을 받기 전에 Core routed-target readiness handler를 장기 등록한다.
  Operation은 최초 시도 전에 정확한 `(socket, RID, transport pair ID, generation)` key,
  coroutine completion과 complete record를 pending에 넣고 같은 target에 `DONTWAIT`로
  시도한다. Callback은 그 key만 ready로 표시하며 native retry는 callback 밖의 pump가
  수행한다. Pair generation이 다른 event는 stale wake로 무시한다. 같은 native handle의
  outbound 경로는 첫 part부터 `FINAL`까지 한 attempt만 보호하는 짧은 gate를 공유하고
  readiness 대기 전에 반환한다. Coroutine cancellation은 pending operation과 request
  correlation을 정확히 한 번 종료한다. 다른 RID의 submit과 Python event loop는 이 대기
  때문에 막히지 않는다.
  Request timeout은 최초 admission 대기와 reply 대기를 포함하는 하나의 absolute deadline이며 retry로 연장하지 않는다.
  같은 routed operation에 flags, callback, blocking terminal이나 `submit_async()`를 함께 제공하지 않는다.
- `RecvFlags.DONT_WAIT`를 사용한 caller-provided receive는 message가 없을 때 `False`를 반환한다.
- timer, monitor와 같은 직접 반환 control API는 pending value가 없을 때 `None`을 반환한다.
- 실제 native failure는 해당 error type으로 전달하며 no-data로 숨기지 않는다.

DEALER와 ROUTER request/reply는 Core routing metadata와 request sequence를 보존한다. ROUTER receive의
`Received.routing_id`는 raw routing id이며 다른 identity type으로 변환되지 않는다.
현재 single-part accessor 이름은 구현·contract test와 같은 `single_part_or_throw()`를 사용한다.
이름 변경은 별도 draft 승인 뒤에만 수행한다.

## Receive flow state

이 바인딩은 Core의 receive-flow 상태를 `ReceiveFlowState` `IntEnum`으로 노출한다.
`RUNNING = 0`, `PAUSED = 1`이며 설정은 `Socket.set_receive_flow_state(state)`다. 반환값은
`None`이고 Python 에러 정책을 따른다. 0이 아닌 native 결과는 해당 `ConfigResult`와 native
errno를 담은 `ConfigError`를 발생시키므로, completion lane이 없는 socket은
`ConfigResult.NOT_SUPPORTED`를 담은 `ConfigError`를 발생시킨다. 이미 유지하는 상태를 다시
설정하면 정상 반환한다.

관측 표면은 C 계약을 따르며 상수와 metric 이름은 C 계층이 확정한다. Monitor event
`SEND_FLOW_PAUSED`, `SEND_FLOW_RESUMED`, `FLOW_STATE_STALE`(`1 << 16`, `1 << 17`,
`1 << 18`, 전체 mask `0x7FFFF`), event flag `SEND_FLOW_WRITABLE`(`1 << 1`),
`FLOW_STATE_STALE_GENERATION`(`1 << 2`), `FLOW_STATE_STALE_EPOCH`(`1 << 3`), status detail
bit `FLOW_STATE`(`1 << 5`), status field 5개 `flow_paused_connections`,
`flow_pause_applied_total`, `flow_resume_applied_total`, `flow_state_stale_total`,
`flow_pause_duration_ms`를 이 언어의 이름 규칙으로 투영한다.

Flow-state frame은 Core 안에 머문다. 바인딩은 setter를 호출하고 monitor event와 snapshot
field를 읽을 뿐, flow-state frame을 직접 encode, decode, 송신 또는 수신하지 않는다.

## Error

Core result를 반환하는 호출은 Python의 대응 error에 `result`, `code`, `native_errno`를 제공한다.
입력 형식 오류는 호출 전에 검사할 수 있지만, native operation failure를 일반 `ValueError`로 바꾸지
않는다. `SubmitError`, `RequestError`, `RecvError`, `BindError`, `ConnectError`, `ConfigError`,
`CloseError`와 `HandlerError`는 `ZlinkError` 계열이다.

## Python version과 type package

공개 annotation은 Python 3.9 parser와 runtime에서 해석할 수 있는 표현을 사용한다. package root에는
`py.typed`가 포함된다. public contract type check는 `pyrightconfig.json`이 지정한 Python 3.9 target과
`src/zlink/contracts`를 대상으로 한다.

## 관련 문서

- 사용 방법은 [Python guide](../../guide/python/index.ko.md)를 따른다.
- Core 함수와 layout의 기준은 repository의 `core/include/zlink.h`와 Core spec이다.
- 구현 상세와 callback/native lifetime 설명은 이 문서가 아니라 internals 문서의 대상이다.
