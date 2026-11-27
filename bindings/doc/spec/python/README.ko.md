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
- Python 표면은 공통 스펙의 Core raw 계약을 따른다.
- Python 3.9 이상 지원과 package 버전은 [배포 metadata](../../../python/pyproject.toml)를 따른다.
- Native package는 Linux x86_64와 Windows x86_64를 지원한다.

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
`create_xpub_socket()`, `create_xsub_socket()`, `create_poller()`, `create_poll_events()`,
`create_timer()`, `create_received()`다. Message는 `allocate_message(size)`와 `Message.from_(value)`로 생성한다.

`Context.shutdown()`은 context의 작업 종료를 요청하고 `Context.recalculate_auto_hwm()`은 Core에 Auto-HWM
재계산을 요청한다. Package의 `version()`, `has(capability)`, `strerror(code)`는 Core 버전·기능·오류
문자열을 반환한다.

## 소켓별 능력

소켓별 공통 계약은 [Socket Capability Matrix](../README.ko.md#socket-capability-matrix)를 따른다.

| 소켓 | 제출 | 수신 |
|---|---|---|
| PAIR | `send` | `recv_into` |
| DEALER | `send`, `request` | `recv_into` |
| ROUTER | `send(routing_id)`, `request`, `reply` | `recv_into` |
| PUB | `publish` | — |
| SUB | — | `subscribe_into` |
| XPUB | `publish` | `receive_subscription_event_into` |
| XSUB | — | `subscribe_into` |
| STREAM | `send(routing_id)` | `recv_into`, `recv_packet_into` |

모든 raw 소켓은 `bind`와 `unbind`로 endpoint를 열고 닫으며, `disconnect_rid`로 routing ID에 해당하는 peer
연결을 끊는다. STREAM을 제외한 소켓은 `connect`로 outbound 연결을 등록하고 `disconnect`로 해제한다.

`set_tls_server(cert: str, key: str, require_client_cert: bool = False)`는 bind 전에 인증서·키와 client
인증 요구를 설정한다.
`set_tls_client(ca_cert: Optional[str], hostname: Optional[str], trust_system: bool = False)`는 connect
전에 CA·서버 이름과 시스템 CA 사용 여부를 설정한다. 두 메서드는 모든 raw 소켓에 공통이다.

연결 메서드는 성공 시 `None`을 반환한다. Core의 `bind` 실패는 `BindError`, 나머지 연결 연산 실패는 `ConnectError`,
TLS 설정 실패는 `ConfigError`로 전달한다.

SUB와 XSUB는 `set_subscription(topic)`과 `unset_subscription(topic)`으로 filter를 설정·해제한다.
`subscription_at(index)`는 filter와 pattern 여부를 반환하고 해당 인덱스가 없으면 `None`을 반환한다. 바인딩은 자기 버퍼로 `zlink_subscription_at`를 한 번 부른다. 빈 filter를 포함해 OK 결과면 조회를 끝내고, 버퍼가 작다는 결과(`ENOBUFS`)일 때만 Core가 돌려준 길이로 한 번 더 부른다. 범위 밖 인덱스(`ENOENT`)는 오류가 아니라 항목 없음으로 반환하고, 그 밖의 실패는 `ConfigError`로 낸다. XPUB의
`receive_subscription_event_into(event: SubscriptionEvent, *, flags: int = 0) -> bool`는
`create_subscription_event()`로 만든 저장소를 채운다.

`options` 속성은 모든 소켓에 공통이다. `router_options`, `dealer_options`, `stream_options`는 해당 소켓에,
`pub_options`는 PUB·XPUB에, `sub_options`는 SUB·XSUB에 적용한다.

### 소켓 readiness snapshot

`RouterSocket.routes_snapshot() -> List[RouterRoute]`는 Core가 선택한 route를 routing ID마다 한 행으로
반환한다. `RouterRoute`는 `routing_id`, `route_generation`을 제공한다. Route generation은 0이 아닌 opaque
token이며 동등성만 비교한다. `PollEventFlag.POLLROUTE`는 선택 route 변경을 알린다.

연결 시도 snapshot은 outbound 연결 등록마다 한 행을 반환한다. 표면은 STREAM을 제외한 raw 소켓의
`connection_attempts_snapshot() -> List[ConnectionAttempt]`과 Poller의 `PollEventFlag.POLLCONNECT`다.
`ConnectionAttempt`는 `endpoint`, `intent_id`, `revision`, `last_failure_id`, `state`, `last_failure`를
담는다. `ConnectionFailure`의 필드는 `kind`, `stage`, `normalized_errno`, `disconnect_reason`다. 상태, 실패
원인, 단계는 각각 닫힌 `ConnectionAttemptState`, `ConnectionFailureKind`, `ConnectionFailureStage` enum으로
Core 값을 보존한다.

`endpoint`는 UTF-8 endpoint를 담은 `str`이며, `last_failure`는 복구 뒤에도 최근
실패를 보존하는 `ConnectionFailure`다. `normalized_errno` (`int`)와 `disconnect_reason` (`int`)은
진단값이다.

`intent_id`와 `revision`은 0이 아닌 `int` opaque token이다. `last_failure_id`는 같은 타입이며
실패 이력이 없을 때 0이다. 토큰은 동등성만 비교한다. 연결 실패는 snapshot 행에 담고 조회 자체의
실패만 `ConfigError`로 전달한다. 바인딩은 monitor 이력이나 timer polling으로 연결 결과를 추정하지 않는다.

성공한 snapshot은 반영한 변경의 readiness를 해제한다. Snapshot 종류마다 소켓당 observer를 하나 둔다.
[공통 snapshot 계약](../README.ko.md#socket-readiness-snapshots)이 두 readiness의 의미를 정한다.

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
명시 입력이
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

Core budget snapshot은 ABI version/size, configured/resolved memory limit,
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
- `Message.copy()`는 원본과 따로 닫을 수 있는 message를 반환한다.
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
- Runtime은 builder가 모은 모든 part를 native 배열과 count로 Core whole-message API에 한 번 제출한다.
  Receive는 재사용 가능한 native 배열, capacity와 count를 사용하고, 배열이 작으면
  필요한 크기로 늘린 뒤 소비되지 않은 같은 record를 다시 받는다.
- `RecvFlags.DONT_WAIT`를 사용한 caller-provided receive는 message가 없을 때 `False`를 반환한다.
- timer, monitor와 같은 직접 반환 control API는 pending value가 없을 때 `None`을 반환한다.
- 실제 native failure는 해당 error type으로 전달하며 no-data로 숨기지 않는다.

DEALER와 ROUTER request/reply는 Core routing metadata와 `ReplyToken`을 보존한다. ROUTER receive의
`Received.routing_id`는 routing ID이며 다른 identity type으로 변환되지 않는다.
현재 single-part accessor 이름은 구현·contract test와 같은 `single_part_or_throw()`를 사용한다.

### Poller와 monitor

`Poller`는 `add_socket`·`modify_socket`·`remove_socket`, `add_fd`·`modify_fd`·`remove_fd`,
`add_timer`·`remove_timer`로 source를 관리한다. Monitor에는
`add_monitor`·`modify_monitor`·`remove_monitor`를 사용한다. `wait(events, timeout_ms)`는
`create_poll_events(capacity)`로 만든 `PollEvents`를 채우며 `events.event(index)`는 `source_kind`, `slot`,
`revents`, `fd`를 담은 `PollEvent`를 반환한다.

[공통 MonitorStatus 계약](../README.ko.md#monitorstatus)에 따라 `MonitorStatus`는 Core ABI v4를 반영한다.
`abi_version`와 `struct_size`는 반환된 layout을 확인하는 진단값이다. `MonitorEvent`의 `event`, `value`,
`routing_id`, `local_addr`, `remote_addr`, `connection_id`, `transport_lane`, `flags`는
[공통 MonitorEvent 계약](../README.ko.md#monitorevent)의 필드 의미와 native layout을 따른다.

## Receive flow state

`ReceiveFlowState`는 `RUNNING = 0`, `PAUSED = 1`인 `IntEnum`이다.
`Socket.set_receive_flow_state(state)`는 `None`을 반환하며 실패한 native `ConfigResult`와
errno를 담은 `ConfigError`를 발생시킨다.
상태·결과·monitor 투영은 [공통 receive-flow 계약](../README.ko.md#receive-flow-projection)을 따른다.

## Error

함수군별 오류는 [공통 오류 코드 표](../README.ko.md#error-code-표)의 66개 non-OK 코드를 보존한다. 코드의
열거 타입은 함수군별 `*Result`다. `ErrorCode`는 Core 전용 errno를 열거한다.

Core result를 반환하는 호출은 Python의 대응 error에 `result`, `code`, `internal_errno`를 제공한다.
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

Python은 blocking `submit_sync()`와 결과 객체(`SendSubmission`/`RequestSubmission`: `result`와 `admitted`, request는 `reply`)를 돌려주는 `submit()`을 제공한다.
결과 접근자는 읽기 전용 property다. `submission.result`로 제출 결과를 읽고,
`await submission.admitted`로 수용 완료를, request의 `await submission.reply`로 응답을 기다린다.
Caller wait 취소는 awaitable cancellation으로 표현한다.

Native completion ID·`user_context`·raw drain은 public API에 노출하지 않는다.
제출 결과는 [공통 결과 투영](../README.ko.md#submit-result-projection)을, 완료 합류·수명과
`PollEventFlag.POLLCOMPLETION`의 진행 조건은 [비동기 실행 모델](../async-execution-model.ko.md)을 따른다.

`Poller.add_monitor(monitor: MonitorSocket, events: PollEventFlag, slot: int) -> None`,
`Poller.modify_monitor(monitor: MonitorSocket, events: PollEventFlag) -> None`,
`Poller.remove_monitor(monitor: MonitorSocket) -> None`는 socket monitor를 poller source로 등록·수정·제거한다
(공통 spec "`Poller`의 monitor source"). 기존 `add_socket/modify_socket/remove_socket`도 monitor를 수용한다.
monitor mask의 결과는 공통 spec "`Poller`의 monitor source"를 따른다.
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
class SendSubmission:
    @property
    def result(self) -> SubmitResult: ...        # OK | BACKPRESSURED, 제출 시점 스냅샷
    @property
    def admitted(self) -> Awaitable[None]: ...   # OK면 완료 상태

class RequestSubmission:
    @property
    def result(self) -> SubmitResult: ...
    @property
    def admitted(self) -> Awaitable[None]: ...
    @property
    def reply(self) -> Awaitable[list[Message]]: ...   # admitted 성공 뒤 완료

class SendOp(Protocol):
    def message(self, payload) -> "SendOp": ...
    def messages(self, *payloads) -> "SendOp": ...
    def submit(self) -> SendSubmission: ...
    def submit_sync(self) -> None: ...

class RequestOp(Protocol):
    def message(self, payload) -> "RequestOp": ...
    def messages(self, *payloads) -> "RequestOp": ...
    def timeout(self, timeout) -> "RequestOp": ...
    def submit(self) -> RequestSubmission: ...
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
        self, received: Received, *, flags: int = 0
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
