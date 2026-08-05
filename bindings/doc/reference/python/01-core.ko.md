한국어 | [English](01-core.en.md)

[레퍼런스 목차](README.ko.md)

# 01. Core

이 category는 context 수명주기, context option, routing identity,
package 최상위 factory/utility 함수를 다룬다. **이 함수들은
`contracts/core/` 아래에 전혀 선언돼 있지 않다** —
`bindings/python/src/zlink/__init__.py`에 있다. `Context`/`ContextOptions`
는 `typing.Protocol` 타입이지 concrete class가 아니다 — 실제 runtime
구현은 다른 곳에 있으며 직접 import되지 않는다. 정확한 signature는
[`contracts/core/`](../../../../bindings/python/src/zlink/contracts/core/)와
[`__init__.py`](../../../../bindings/python/src/zlink/__init__.py)가
소유한다.

---

## `create_context()`

메시징 context를 생성한다 — socket의 factory이자 소유자.

```python
with create_context() as ctx:
    ...
# 또는
async with create_context() as ctx:
    ...
```

**Options.** 인자 없음.

**Completion result.** `Context`를 반환한다. sync(`with`)와
async(`async with`) context-manager 프로토콜 둘 다 지원한다 — 어느
경로로든(또는 명시적 `close()`) 닫으면 그 하위에 아직 열려 있는
모든 것(context에서 생성된 socket 포함)이 종료된다.

**선택 기준.** application이 필요로 하는 context마다 한 번 호출한다 —
대부분의 application은 정확히 하나가 필요하다.

---

## `Context.shutdown()` / `Context.recalculate_auto_hwm()`

context의 socket이 닫히지 않은 채 blocking operation을 인터럽트하거나,
automatic high-water mark의 즉시 재계산을 강제한다.

```python
ctx.shutdown()
ctx.recalculate_auto_hwm()
```

**Options.** 둘 다 인자 없음.

**Completion result.** 둘 다 반환값 없이 동기다. `shutdown`은 이
context 하위 socket의 blocking 호출을 인터럽트하지만 context나 그
socket을 닫지 않는다. `recalculate_auto_hwm`은 아직 `AutoHwmProfile`이
설정된 socket에 대해서만 automatic HWM을 재계산한다.

**선택 기준.** 여러 스레드에서 socket을 쓰는 중인 context를 닫기
전엔 `shutdown()`을 호출해 socket 호출을 기다리는 스레드가 무기한
block되는 걸 막는다. auto-HWM profile이나 message-unit option을 바꾼
후엔 새 sizing을 즉시 적용하려고 `recalculate_auto_hwm()`을 호출한다.

---

## `Context.options`

`Context`의 `options` property로 읽는 context-wide option facade.

```python
ctx.options.io_threads = 8
ctx.options.auto_hwm_profile = AutoHwmProfile.LOW_LATENCY
ctx.options.add_thread_affinity(2)
```

**Options.** 별도 표기 없으면 순수 property는 get/set이다. **지금까지
다룬 다른 모든 언어와 달리 여기엔 `thread_priority` property가 없다.**

| Member | 의미 |
| --- | --- |
| `io_threads` | I/O thread 개수 |
| `max_sockets` | context 전체 socket 상한 |
| `socket_limit` | 읽기 전용, `max_sockets`의 빌드상 하드 상한 |
| `max_message_size` | 메시지 하나의 크기 상한 |
| `msg_t_size` | 읽기 전용, native message struct 크기, 진단 전용 |
| `thread_scheduling_policy` | dispatch thread scheduling policy |
| `thread_name_prefix` | OS에 보이는 dispatch thread 이름 접두사 |
| `auto_hwm_enabled` | auto-HWM sizing 활성 여부 |
| `auto_hwm_recalc_debounce` | 자동 재계산 사이 최소 간격 |
| `blocky` | blocking 호출이 실제로 block하는지 fail fast하는지 |
| `auto_hwm_profile` | automatic HWM sizing profile — Sockets category 참고 |
| `auto_hwm_msg_unit_bytes` | auto-HWM용 accounted-byte 단위; `0`은 socket-type 기본값 선택 |
| `add_thread_affinity(cpu)` | I/O thread를 CPU에 고정 |
| `remove_thread_affinity(cpu)` | I/O thread의 CPU 고정을 해제 |

**Completion result.** 모든 property 읽기/쓰기와 두 method는 동기다.

**선택 기준.** 기본값이 배포 환경에 맞지 않을 때 socket 생성 전에
조정한다. `auto_hwm_profile`/`auto_hwm_enabled` 변경은
`Context.recalculate_auto_hwm()`과 짝지어 즉시 적용한다.

---

## `create_pair_socket(ctx)` / `create_dealer_socket(ctx)` / `create_router_socket(ctx)` / `create_pub_socket(ctx)` / `create_sub_socket(ctx)` / `create_xpub_socket(ctx)` / `create_xsub_socket(ctx)` / `create_stream_socket(ctx)`

context로부터 주어진 타입의 socket을 생성한다, caller가 소유.

```python
dealer = create_dealer_socket(ctx)
```

**Options.** 각 factory는 소유 `Context`를 받는다.

**Completion result.** 각각 대응하는 socket을 반환하며,
context-manager 프로토콜을 지원한다. caller가 소유하며 context와
독립적으로 반드시 close(또는 `with`)해야 한다.

**선택 기준.** 각 socket type의 operation·option·역할은 Sockets
category를 참고한다 — 이 항목은 각각이 어떻게 생성되는지만 다룬다.

---

## `RoutingId`

메시징 peer나 route를 식별하는 1~255바이트의 binary-safe value type.

```python
from_string = RoutingId.from_("worker-3")
from_bytes = RoutingId.from_(raw_bytes)
from_uint32 = RoutingId.from_(42)
from_uuid = RoutingId.from_(uuid.uuid4())
restored = RoutingId.from_hex(previously_printed.to_hex())
```

**Options.**

| Member | 의미 |
| --- | --- |
| `RoutingId(data)` | 생성자; 1..255바이트의 bytes-like 객체를 복사, 범위를 벗어나면 `ValueError` |
| `from_(value)` | static factory(`from`이 Python 키워드라 뒤에 underscore가 붙음); 인자 타입에 따라 분기 — `str`(UTF-8 인코딩), `int`(4-byte big-endian uint32, `0..4294967295`), `uuid.UUID`(16바이트), 또는 임의 bytes-like 객체(복사됨) |
| `from_hex(value: str)` | `to_hex()`가 출력한 byte를 복원 |
| `size` | property, byte 길이, 1-255 |
| `to_bytes()` | bytes의 방어적 복사 |
| `to_hex()` | hex 인코딩, `from_hex`와 왕복 가능 |
| `__str__` | 표시 형태: printable UTF-8, 그 다음 4-byte를 `int`로, 그 다음 16-byte를 `uuid.UUID`로, 마지막 `hex:` 접두 fallback |
| `__bytes__` | `bytes(routing_id)`를 통한 raw bytes |
| `__len__` | `len(routing_id)`를 통한 byte 길이 |
| `__hash__` | 값 기반 hash |
| `__eq__` | 값 동등성; 다른 `RoutingId`뿐 아니라 raw bytes-like 값도 비교에 받아들임 |
| `__repr__` | 디버그 표현 |

**Completion result.** 모든 factory·accessor는 동기다. 범위를 벗어난
길이는 `ValueError`를 던진다. `from_hex`에 잘못된 hex 문자열을 주면
`TypeError`/`ValueError`를 던진다.

**선택 기준.** 임의 입력 타입엔 `from_(value)`를 쓴다 — 하나의
호출로 문자열, int, UUID, raw byte를 다 다루며, 타입별 별개 overload/
factory를 쓰는 언어와 다르다. 내구성 있는 raw-byte round trip
전용으로 `to_hex()`/`from_hex()`를 쓴다 — `str(routing_id)`는 표시
전용이며 가역성이 보장되지 않는다.

---

## `version()` / `has(capability)` / `strerror(errnum)`

native library의 빌드 버전을 읽거나, 선택적 빌드 역할을 확인하거나,
native error code를 메시지로 변환한다.

```python
major, minor, patch = version()
has_tls = has("tls")
message = strerror(errnum)
```

**Options.**

| Member | 의미 |
| --- | --- |
| `version()` | `(major, minor, patch)` tuple |
| `has(capability: str)` | 지정한 선택적 역할이 이 빌드에 컴파일돼 있는지 — 인식하는 이름은 `"tcp"`, `"ipc"`, `"tls"`, `"ws"`, `"wss"`; 다른 문자열은 `False`를 반환한다 |
| `strerror(errnum: int)` | 해당 native error code의 메시지 텍스트 |

**Completion result.** 모두 동기이며 error 경로가 없다. `version()`은
`(major, minor, patch)` tuple을 반환한다. `has`는 `bool`을 반환한다.
`strerror`는 `str`을 반환한다.

**선택 기준.** 링크된 native library 버전이 application이 기대하는
버전과 일치하는지 확인하려면 `version()`을 쓴다. 기동 시점에
`has(...)`로 선택적 transport에 분기한다. `strerror`는 다른 곳(Errors
category)에서 드러난 native error code와 함께 진단용으로 쓴다.

---

## `create_atomic_counter()` / `create_stopwatch()` / `create_thread(target)`

thread-safe 정수 counter, 고해상도 stopwatch, 실행 중인 background
thread를 생성한다 — 세 개의 독립된 utility resource, 모두
`contracts/core/utilities.py`에 선언된 `Protocol` 타입.

```python
with create_atomic_counter() as counter:
    new_value = counter.increment()

with create_stopwatch() as watch:
    partial_us = watch.intermediate()
    total_us = watch.stop()

thread = create_thread(do_work)
thread.join()
```

**Options.** 세 factory 중 `create_thread`의 target callable 외엔
인자를 받지 않는다.

| Member | 의미 |
| --- | --- |
| `AtomicCounter.set(value)` | counter 값을 지정 |
| `AtomicCounter.increment()` / `decrement()` | counter를 1 조정, *새* 값을 반환 |
| `AtomicCounter.value` | property, 현재 값을 읽음 |
| `Stopwatch.intermediate()` / `stop()` | 생성 이후 경과 마이크로초, 둘 다; `intermediate()`는 몇 번이든 호출, `stop()`은 정확히 한 번 호출해 종료 |
| `Stopwatch.close()` | stopwatch를 해제 |
| `Thread.join()` | task가 끝날 때까지 block — **유일한 member**; `close()`가 없다, context-manager 프로토콜을 지원하는 여기 다른 모든 utility 타입과 다르다 |

**Completion result.** `AtomicCounter`/`Stopwatch`는 sync·async
context-manager 프로토콜을 둘 다 지원한다. `Thread`는 그렇지 않다
(`close()`/`__enter__`가 아예 없다).

**선택 기준.** 스레드 전체에서 안전한 공유 count엔 `AtomicCounter`를
쓴다. 벤치마킹엔 `Stopwatch`를 쓴다 — `intermediate()`는 몇 번이든
호출하고, `stop()`은 정확히 한 번 호출한다. zlink 런타임이 수명주기를
소유해야 할 땐 Python의 `threading.Thread`를 직접 쓰는 대신
`create_thread`를 쓴다.

---

## `proxy(...)` / `proxy_steerable(...)` / `sleep(seconds)` / `multipart_close(parts)`

두 socket 사이의 양방향 message-forwarding loop을
실행하거나(선택적으로 control socket을 통해 조종 가능), 호출 스레드를
sleep하거나, multipart sequence의 모든 메시지를 닫는다.

```python
proxy(frontend, backend, capture)  # capture는 None 가능; context 종료까지 block
proxy_steerable(frontend, backend, capture, control)
sleep(1)  # 초 단위, 밀리초 아님
multipart_close(parts)
```

**Options.**

| Member | 의미 |
| --- | --- |
| `proxy(frontend, backend, capture=None)` | `capture`는 선택 사항 |
| `proxy_steerable(frontend, backend, capture, control)` | 필수 `control` socket을 더함 |
| `sleep(seconds)` | 호출 스레드를 block; 초 단위를 직접 받는다 |
| `multipart_close(parts)` | 모든 메시지를 한 번에 닫음 |

**Completion result.** 모두 반환값 없이 동기다. `proxy`/
`proxy_steerable`은 context가 종료될 때까지(또는 `proxy_steerable`의
경우 control 명령이나 에러가 loop을 끝낼 때까지) 호출 스레드를
block한다 — 둘 중 하나를 전용 스레드에서 실행한다.

**선택 기준.** 단순한 fire-and-forget forwarding loop엔 `proxy`를,
application이 다른 스레드에서 control socket을 통해 loop을
일시정지·재개·종료해야 할 땐 `proxy_steerable`을 쓴다. 수신되거나
구성된 multipart sequence의 모든 메시지를 손으로 짠 loop 대신 한
호출로 해제하려면 `multipart_close`를 쓴다.

---

[`contracts/core/`](../../../../bindings/python/src/zlink/contracts/core/),
[`__init__.py`](../../../../bindings/python/src/zlink/__init__.py),
[Python 바인딩 스펙](../../spec/python/README.ko.md)에서 전체 근거를 확인한다.
