---
title: "Python 바인딩 공개 계약"
---

<!-- bindings-nav:start -->
[스펙 목록](../README.ko.md) | [이전: Node.js](../node/README.ko.md) | [다음: Go](../go/README.ko.md)
<!-- bindings-nav:end -->

# Python binding Core 공개 계약

> **이 장이 정의하는 것** — `zlink` Python package가 Core raw messaging 위에 제공하는
> 공개 타입·소유권·오류 계약.

- 이 문서는 `zlink` Python package가 제공하는 Core raw messaging 계약을 정의한다.
- 이 문서와 공개 header가 정의하지 않는 기능은 Python binding 계약이 아니다.
- Python 3.9 이상 지원과 package 버전은 [배포 metadata](../../../python/pyproject.toml)를 따른다.
- 현재 native package target은 Linux x86_64이며, 다른 target은 별도 candidate payload와 clean consumer 검증 전까지 이 계약의 지원 범위가 아니다.

| 절 | 다루는 내용 |
|---|---|
| [범위](#범위) | Core 자원을 표현하는 Python 공개 type 목록 |
| [Package 표면](#package-표면) | public factory와 private 영역 경계 |
| [Byte HWM과 Auto-HWM](#byte-hwm과-auto-hwm) | Python `int`와 Core `uint64_t` byte HWM의 매핑 |
| [소유권과 수명](#소유권과-수명) | native handle·message·Received의 소유·해제 규칙 |
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

[HWM](../../../../core/doc/spec/core/glossary.ko.md#hwm)(queue의 byte 보관량을 제한하는 기준)의 계산과 queue admission은 Core가 담당한다. Python의
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
`int`/boolean으로 제공한다. `application_accounted_bytes`와 위 세 owner-lifecycle
필드는 ABI 예약 필드이며 항상 `0`이다. Reset은 current·pending·queue count를 유지하고
두 peak를 current로 재기준화하며 epoch counter를 0으로 만든 뒤
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

`Received`와 `TopicMessage`는 native part, routing ID, reply token, topic과 multipart
framing을 보존하고 `close()`, context manager 종료 또는 저장소 재사용으로 정리한다.
수신 회계와 결과 수명의 경계는 [공통 수신 ownership 계약](../README.ko.md#receive-ownership)을 따른다.

## 송수신과 no-data

- Send·request builder의 완료 경계는 [비동기 완료 표면 정책](../async-coroutine-policy.ko.md)을,
  완료 합류와 cancellation은 [비동기 실행 모델](../async-execution-model.ko.md)을 따른다.
- Reply와 publish는 synchronous `submit()`으로 끝난다. Publish flags는 별도 `PublishOp`만
  제공한다.
- `RecvFlags.DONT_WAIT`를 사용한 caller-provided receive는 message가 없을 때 `False`를 반환한다.
- timer, monitor와 같은 직접 반환 control API는 pending value가 없을 때 `None`을 반환한다.
- 실제 native failure는 해당 error type으로 전달하며 no-data로 숨기지 않는다.

DEALER와 ROUTER request/reply는 Core routing metadata와 `ReplyToken`을 보존한다. ROUTER receive의
`Received.routing_id`는 routing ID이며 다른 identity type으로 변환되지 않는다.
현재 single-part accessor 이름은 구현·contract test와 같은 `single_part_or_throw()`를 사용한다.

## Receive flow state

`ReceiveFlowState`는 `RUNNING = 0`, `PAUSED = 1`인 `IntEnum`이다.
`Socket.set_receive_flow_state(state)`는 `None`을 반환하며 실패한 native `ConfigResult`와
errno를 담은 `ConfigError`를 발생시킨다.
상태·결과·monitor 투영은 [공통 receive-flow 계약](../README.ko.md#receive-flow-projection)을 따른다.

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

## Pull completion 공개 계약

Python package 정보는 [배포 metadata](../../../python/pyproject.toml)를, Core ABI 버전은 [Core release metadata](../../../../VERSION)를 따른다.

Python은 blocking `submit_sync()`와 awaitable을 반환하는 `submit()`을 제공한다.
Caller wait 취소는 awaitable cancellation으로 표현한다.

Native completion ID·`user_context`·raw drain은 public API에 노출하지 않는다.
제출 결과는 [공통 결과 투영](../README.ko.md#submit-result-projection)을, 완료 합류·수명과
`PollEventFlag.POLLCOMPLETION`의 진행 조건은 [비동기 실행 모델](../async-execution-model.ko.md)을 따른다.

`Poller.add_monitor(monitor: MonitorSocket, events: PollEventFlag, slot: int) -> None`,
`Poller.modify_monitor(monitor: MonitorSocket, events: PollEventFlag) -> None`,
`Poller.remove_monitor(monitor: MonitorSocket) -> None`는 socket monitor를 poller source로 등록·수정·제거한다
(공통 spec "`Poller`의 monitor source"). 기존 `add_socket/modify_socket/remove_socket`도 monitor를 수용한다.
monitor에는 `PollEventFlag.POLLIN`만 유효하고 다른 bit가 있으면 typed `ConfigResult.INVALID_ARGUMENT`로 거절한다.
ready 뒤 `monitor.recv(RecvFlags.DONT_WAIT)`로 drain하며 poll event는 socket과 같은 slot/source kind로 보고한다.

`ReplyToken`은 module-private `_reply_token_from_native`만 만들며 public construction과
serialization을 거부한다. Factory는 `object.__new__(ReplyToken)`과 `object.__setattr__`로
private `_owner`·`_value`를 채운다. Equality와 hash는 owner identity와 opaque value를 함께 사용한다.
`StreamPacket`은 empty reusable output이다. Publish는 send와 별도 `PublishOp`에서 기존 flags와
synchronous submit 의미를 유지한다.
Token은 raw property, `int()` conversion, ordering과 `close()`를 제공하지 않는다.
같은 output의 concurrent recv는 invalid-state다. Message reference는 다음 recv 진입이나
`close()` 전까지만 유효하다. `recv_mode` setter는 첫 bind/connect 전에 `RAW`·`PACKET`만 받고
`UNSPECIFIED`를 거부한다.

### Public interface

```python
class SendOp(Protocol):
    def message(self, payload) -> "SendOp": ...
    def messages(self, *payloads) -> "SendOp": ...
    def submit(self) -> Awaitable[None]: ...
    def submit_sync(self) -> None: ...

class RequestOp(Protocol):
    def message(self, payload) -> "RequestOp": ...
    def messages(self, *payloads) -> "RequestOp": ...
    def timeout(self, timeout) -> "RequestOp": ...
    def submit(self) -> Awaitable[list[Message]]: ...
    def submit_sync(self) -> list[Message]: ...

class ReplyOp(Protocol):
    def message(self, payload) -> "ReplyOp": ...
    def messages(self, *payloads) -> "ReplyOp": ...
    def submit(self) -> None: ...

class PublishOp(Protocol):
    def message(self, payload) -> "PublishOp": ...
    def messages(self, *payloads) -> "PublishOp": ...
    def flags(self, flags) -> "PublishOp": ...
    def submit(self) -> None: ...

@final
class ReplyToken:
    __slots__ = ("_owner", "_value")

    def __new__(cls) -> NoReturn:
        raise TypeError("ReplyToken is created by ROUTER request receive")

    def __eq__(self, other: object) -> bool: ...
    def __hash__(self) -> int: ...
    def __repr__(self) -> str: return "ReplyToken()"
    def __copy__(self) -> "ReplyToken": return self
    def __deepcopy__(self, memo) -> "ReplyToken": return self
    def __reduce_ex__(self, protocol):
        raise TypeError("ReplyToken cannot be serialized")

class StreamRecvMode(IntEnum):
    UNSPECIFIED = 0
    RAW = 1
    PACKET = 2

class StreamSocketOptions(Protocol):
    @property
    def recv_mode(self) -> StreamRecvMode: ...

    @recv_mode.setter
    def recv_mode(self, mode: StreamRecvMode) -> None: ...

class StreamPacket:
    routing_id: Optional[RoutingId]
    header: Optional[Message]
    body: Optional[Message]

    def __init__(self) -> None: ...
    @property
    def is_empty(self) -> bool: ...
    def close(self) -> None: ...
    def __enter__(self) -> "StreamPacket": ...
    def __exit__(self, exc_type, exc, tb) -> None: ...

class Received:
    routing_id: Optional[RoutingId]
    reply_token: Optional[ReplyToken]

class StreamSocket:
    def send(self, routing_id: RoutingId) -> SendOp: ...
    def recv_into(
        self, out: Received, *, flags: RecvFlags = RecvFlags.NONE
    ) -> bool: ...
    def recv_packet_into(
        self, out: StreamPacket, *, flags: RecvFlags = RecvFlags.NONE
    ) -> bool: ...
```

Operation 시작 signature는 PAIR `send() -> SendOp`, DEALER `send() -> SendOp`·
`request() -> RequestOp`, ROUTER `send(routing_id) -> SendOp`·
`request(routing_id) -> RequestOp`·`reply(routing_id, token) -> ReplyOp`, STREAM
`send(routing_id) -> SendOp`다. Send factory는 target을 builder에 capture한다. PUB·XPUB의
`publish(topic)`은 `PublishOp`를 반환한다.
`Received.send()`는 source target을 capture한 `SendOp`, `Received.reply()`는 source RID와 token을
capture한 `ReplyOp`를 반환한다.

Public Python surface에는 `RoutedSendOp`, `StreamSocket.send_async()`·`on_packet()`, request
callback 인자, reply의 `_FlaggedFluentMessageOp`·flags, monitor `ignore_handler`·`on_event`, timer
`on_fire`, pair/generation member가 없다.

Monitor는 `recv(*, flags=RecvFlags.NONE) -> Optional[MonitorEvent]`·`status()`·`close()`를,
timer는 `start(interval_ns:int, repeat_count:int)`·`stop()`·`recv() -> Optional[int]`·`close()`를
제공한다.
Monitor event의 `connection_id`는 진단과 correlation에만 사용하며 send·reply target이나
reconnect fence로 사용하지 않는다.
Internal FFI enum mirror는 `ZLINK_OPT_PENDING_MAX_MSGS`와
`ZLINK_OPT_PENDING_MAX_BYTES`만 사용하며 public option property를 추가하지 않는다.

## 구현 및 contract test 검증 요구

Public Python protocol, result·exception과 poller event만으로 다음을 확인한다. 각 항목은 contract
test 하나로 이어진다.

**Operation과 완료**

- Send/request는 §Public interface의 flag 없는 awaitable·sync terminal만 제공하고 request timeout은
  유지한다.
- `publish(topic)`은 별도 `PublishOp`를 반환하고 publish flags와 synchronous submit을 제공한다.
- 완료·cancellation·poller의 공통 관측은
  [실행 모델 검증 요구](../async-execution-model.ko.md#7-구현-및-contract-test-검증-요구)를 따른다.

**ReplyToken과 STREAM**

- Public `ReplyToken()`과 pickle serialization은 실패하고 `copy.copy()`·`copy.deepcopy()`는 같은
  immutable valid token을 반환한다.
- 같은 owner·value token만 같으며 다른 owner token의 reply는 native 호출 전에 실패한다.
- `recv_packet_into()`는 성공 뒤 output을 채우고 no-data·오류 때 empty로 유지하며 `close()` 뒤
  재사용할 수 있다.

**Pull eventing**

- Monitor·timer recv는 no-data를 `None`으로 반환하고 callback 없이 event와 fire count를 관찰한다.
