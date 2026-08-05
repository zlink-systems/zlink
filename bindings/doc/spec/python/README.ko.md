---
title: "Python 바인딩 공개 계약"
---

<!-- bindings-nav:start -->
[스펙 목록](../README.ko.md) | [이전: Node.js](../node/README.ko.md) | [다음: Go](../go/README.ko.md)
<!-- bindings-nav:end -->

# Python binding Core 11 공개 계약

> **이 장이 정의하는 것** — `zlink` Python package가 Core 11 raw messaging 위에 제공하는
> 공개 타입·소유권·오류 계약.

- 이 문서는 `zlink` Python package가 제공하는 Core 11 raw messaging 계약을 정의한다.
- 현재 구현과 공개 header에 없는 기능은 이 문서의 계약이 아니다.
- Python 3.9 이상을 지원하며, 현재 candidate package version은 `11.2.0`이다.
- 현재 native package target은 Linux x86_64이며, 다른 target은 별도 candidate payload와 clean consumer 검증 전까지 이 계약의 지원 범위가 아니다.

| 절 | 다루는 내용 |
|---|---|
| [범위](#범위) | Core 자원을 표현하는 Python 공개 type 목록 |
| [Package 표면](#package-표면) | public factory와 private 영역 경계 |
| [소유권과 수명](#소유권과-수명) | native handle·message·Received의 소유·해제 규칙 |
| [Callback 표면](#callback-표면) | 공개 callback 경로와 노출하지 않는 primitive |
| [송수신과 no-data](#송수신과-no-data) | submit·no-data 표현과 native failure 전달 |
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

## Callback 표면

Core FFI의 `zlink_recv_handler()`와
`zlink_router_completion_control_handler()`는 Python package가 직접 노출하지 않는
private 구현 primitive다. Python의 공개 callback 표면은 STREAM packet의 `on_packet`,
send readiness의 `on_send_ready`, monitor event의 `on_event`, 그리고 ROUTER request
completion을 전달하는 `request(...)` callback 경로로 고정한다. raw receive callback이나
completion-control handler를 등록하는 별도 public method는 제공하지 않는다.

## 송수신과 no-data

- 송신 builder는 message part를 추가한 뒤 `submit()`한다. blocking send는 socket option과 Core의 timeout 계약을 따른다.
- `RecvFlags.DONT_WAIT`를 사용한 caller-provided receive는 message가 없을 때 `False`를 반환한다.
- timer, monitor와 같은 직접 반환 control API는 pending value가 없을 때 `None`을 반환한다.
- 실제 native failure는 해당 error type으로 전달하며 no-data로 숨기지 않는다.

DEALER와 ROUTER request/reply는 Core routing metadata와 request sequence를 보존한다. ROUTER receive의
`Received.routing_id`는 raw routing id이며 다른 identity type으로 변환되지 않는다.
현재 single-part accessor 이름은 구현·contract test와 같은 `single_part_or_throw()`를 사용한다.
이름 변경은 별도 draft 승인 뒤에만 수행한다.

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
