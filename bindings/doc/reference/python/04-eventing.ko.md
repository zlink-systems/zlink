한국어 | [English](04-eventing.en.md)

[레퍼런스 목차](README.ko.md)

# 04. Eventing

이 category는 socket monitoring, 재사용 가능한 poller, standalone
timer를 다룬다 — 각각 `open_socket_monitor(...)`, `create_poller()`,
`create_timer()`로 생성되며, 모두 package 최상위(Core category)에서
도달한다. 정확한 signature는
[`contracts/eventing/`](../../../../bindings/python/src/zlink/contracts/eventing/)가
소유한다.

---

## `MonitorSocket`

socket의 connection lifecycle event를 관찰하고 현재 상태를 읽는다.

```python
monitor = open_socket_monitor(socket)
monitor.on_event(lambda event: print(event.event, event.remote_addr))
status = monitor.status()
```

**Options.**

| Member | 의미 |
| --- | --- |
| `status()` | 시점 스냅샷 `MonitorStatus`를 반환 |
| `close()` | monitor를 닫음 |
| `recv(*, flags=0)` | 다음 event를 가져옴; `MonitorEvent` 또는 `DONT_WAIT`가 설정되고 없으면 `None` 반환 |
| `on_event(handler)` | 수동적 lifecycle-event 콜백을 등록, background dispatch 스레드에서 호출됨 |
| `ignore_handler` | caller가 event를 명시적으로 버리려고 등록할 수 있는 `staticmethod`(`lambda event: None`) |

**Completion result.** 모든 member는 동기다. sync·async
context-manager 프로토콜을 둘 다 지원한다.

**선택 기준.** 한 번 등록하는 수동적 lifecycle observer엔
`on_event`를 쓴다. pull 기반 drain loop엔 대신 `recv`를 쓴다. 시점
스냅샷엔 `status()`를 쓴다.

---

## `MonitorStatus`

`MonitorSocket.status()`가 반환하는, socket의 monitored 상태와
auto-high-water-mark telemetry 시점 스냅샷. 큰 keyword-only
`__init__`을 가진 concrete class다(`Protocol`이 아님).

**Options.** 생성자가 모든 필드를 keyword-only 인자로 받는다. 여러
개는 향후 호환성을 위해 `None` 기본값을 받는다. 눈에 띄는 이름 중복:
`auto_hwm_applied_sndhwm_bytes`와 `auto_hwm_applied_sndhwm`은 별개
생성자 파라미터지만, 짧은 이름이 생략되면 `__init__`이 `_bytes` 값으로
fallback한다(`_rcvhwm`, `_deferred_sndhwm`, `_deferred_rcvhwm`도 같은
패턴) — instance를 읽는 caller는 두 attribute 이름 모두 같은 값을
가진 걸 본다.

| 그룹 | Attribute |
|---|---|
| ABI identity | `abi_version`, `struct_size` |
| Source/state | `source_kind`, `state_flags`/`detail_flags`(비트마스크), `is_ready()`(계산 method) |
| Pending count | `snd_pending_msgs`, `rcv_pending_msgs` |
| Auto-HWM 설정 | `auto_hwm_enabled`, `auto_hwm_profile`/`auto_hwm_role`/`auto_hwm_policy_class`, `auto_hwm_unit_budget_bytes`/`auto_hwm_socket_message_slots`, `auto_hwm_size_cap` |
| Connection bucket | `auto_hwm_connection_bucket_enabled`, `auto_hwm_connection_bucket_count`/`_index`/`_hwm_4k`, `auto_hwm_connection_bucket_hysteresis_retained` |
| Auto-HWM plan(byte) | `auto_hwm_effective_message_bytes`, `auto_hwm_planned_sndhwm_bytes`/`_rcvhwm_bytes`, `auto_hwm_applied_sndhwm_bytes`/`_rcvhwm_bytes`(과 alias `auto_hwm_applied_sndhwm`/`_rcvhwm`), `auto_hwm_effective_sndbuf`/`_rcvbuf` |
| Auto-HWM recalc | `auto_hwm_last_recalc_ms`, `auto_hwm_last_recalc_reason`, `auto_hwm_send_blocked_ratio_ppm` |
| Auto-HWM deferred shrink | `auto_hwm_deferred_sndhwm_bytes`/`_rcvhwm_bytes`(과 alias), `auto_hwm_deferred_sndhwm_valid`/`_rcvhwm_valid`가 true일 때만 유효 |
| In-flight/과금 | `snd_bytes_in_flight`, `rcv_bytes_in_flight`, `minimum_core_message_charge_bytes`, `oversize_message_admission_count`, `oversize_message_admission_max_bytes` |

**Completion result.** 해당 없음 — 순수 instance attribute에 더해 계산
method `is_ready()`.

**선택 기준.** `state_flags`를 직접 디코딩하는 대신 `is_ready()`를
읽는다. socket의 실제 send/receive HWM이 설정한
`CommonSocketOptions` 값(Sockets category)과 다른 이유를 진단할 땐
connection-bucket과 auto-HWM-plan attribute를 쓴다.

---

## `MonitorEvent`

monitor가 보고하는 socket connection-lifecycle event 하나.
keyword-only `__init__`을 가진 concrete class다.

**Options.** 생성자 keyword 인자는 전부 필수이며 기본값이 없다; 각각
같은 이름의 순수 instance attribute가 된다.

| Field | 의미 |
| --- | --- |
| `event` | lifecycle event의 종류(`MonitorEventMask` 값) |
| `value` | 에러 코드나 reconnect interval 같은 event별 값 |
| `routing_id` | event가 제공할 때만 존재하는 peer routing id |
| `local_addr` / `remote_addr` | event에 결부된 local/remote 주소 |

**Completion result.** 해당 없음 — monitor가 전달하는 실질적으로
불변인 값(mutation을 막는 건 없지만 contract 어디도 그걸 요구하지
않는다).

**선택 기준.** lifecycle transition에 분기하려면 `event`
(`MonitorEventMask` 값)를 읽는다. `value`는 event별 세부사항을
담는다(예: 에러 코드나 reconnect interval).

---

## `Poller`

socket, file descriptor, timer를 하나의 재사용 가능한 wait로
multiplex한다.

```python
poller = create_poller()
poller.add_socket(dealer, PollEventFlag.POLLIN, slot=1)
poller.add_timer(timer, slot=2)
events = create_poll_events(8)
ready = poller.wait(events, timeout_ms=1000)
```

**Options.**

| Member | 의미 |
| --- | --- |
| `add_socket(socket, events, slot)` | socket을 등록; `slot`은 대응하는 결과로 그대로 되돌아오는 caller token |
| `add_fd(fd, events, slot)` | raw file descriptor를 등록, 같은 형태 |
| `add_timer(timer, slot)` | timer를 socket/fd와 함께 multiplex하도록 등록 |
| `modify_socket(socket, events)` / `modify_fd(fd, events)` | 이미 등록된 socket/fd의 감시 event를 교체 |
| `remove_socket(socket)` / `remove_fd(fd)` / `remove_timer(timer)` | source 등록을 해제 |
| `size()` | 현재 등록된 source 개수 |
| `wait(events, timeout_ms)` | `timeout_ms`까지 block하며 `events`를 그 자리에서 채움; 음수 timeout은 무기한 block |
| `close()` | poller를 닫음 |

**Completion result.** 등록/제거 member는 동기다. `wait`는
`timeout_ms`까지 block하며, `events`를 그 자리에서 채우고 준비된
개수를 반환한다. sync·async context-manager 프로토콜을 둘 다
지원한다.

**선택 기준.** 서비스 수명 전체에서 poller 하나를 쓴다. `wait`
호출마다 새로 만드는 대신 `PollEvents` buffer 하나를 재사용한다.

---

## `PollEvents` / `PollEvent`

`PollEvents`는 `Poller.wait(...)`가 채우는 재사용 가능한 poll 결과
buffer로, `create_poll_events(capacity)`(Core category)로 생성된다 —
java/node의 사전할당 결과 buffer와 같은 설계다. `PollEvent`는
`@dataclass(frozen=True)`로 필요 시 결과 하나를 materialize한다.

```python
events = create_poll_events(16)
poller.wait(events, timeout_ms=500)
for i in range(events.ready_count):
    if events.has_event(i, PollEventFlag.POLLIN):
        ...
```

**Options — `PollEvents`.**

| Member | 의미 |
| --- | --- |
| `capacity` | property, `create_poll_events(...)`에 넘긴 고정 용량 |
| `ready_count` | property, 마지막 `wait` 이후 몇 개 slot이 준비된 event를 담고 있는지 |
| `source_kind(index)` | 해당 index의 준비된 source 종류 |
| `slot(index)` | 그 source 등록 시 넘긴 caller token |
| `revents(index)` | 해당 index의 raw poll-event bitmask |
| `fd(index)` | 해당 index의 file descriptor, FD kind source에서만 채워짐 |
| `has_event(index, event)` | `revents(index)`에 대한 편의 bit-test |
| `event(index)` | 해당 index의 `PollEvent`를 materialize |

**Options — `PollEvent`**(frozen dataclass).

| Field | 의미 |
| --- | --- |
| `source_kind` | source가 socket·file descriptor·timer 중 무엇인지 |
| `slot` | 등록 시 제공한 caller token |
| `revents` | raw poll-event bitmask |
| `fd` | file descriptor, source가 FD kind가 아니면 기본값 `0` |

**Completion result.** 모든 `PollEvents` accessor는 동기다.
`PollEvent`는 불변이다(`frozen=True`).

**선택 기준.** `revents(index)`를 손으로 bit-test하는 대신
`has_event(index, flag)`를 선호한다. caller가 진짜로 materialize된
`PollEvent`가 필요할 때만 `event(index)`를 쓴다 — 개별 accessor를
순회하면 hot polling loop에서 그 할당을 피할 수 있다.

---

## `Timer`

interval마다 fire하며 poll하거나 await할 수 있는 timer.

```python
timer = create_timer()
timer.on_fire(lambda count: print(f"fired {count} times"))
timer.start(interval_ns=1_000_000_000, repeat_count=0)
```

**Options.**

| Member | 의미 |
| --- | --- |
| `start(interval_ns: int, repeat_count: int)` | `interval_ns`마다 fire를 시작; **interval이 나노초 단위다**, rust의 `Timer::start`와 일치하고 지금까지 다룬 다른 모든 언어가 쓰는 밀리초/`Duration` 기반 `start`와 다르다; `repeat_count == 0`은 무제한 |
| `stop()` | fire를 멈춤; `start`로 재시작 가능 |
| `recv()` | `Optional[int]` 반환 — 누적 fire count, 대기 중인 게 없으면 `None` |
| `on_fire(handler)` | 수동적 interval 콜백을 등록, background dispatch 스레드에서 호출됨 — **여기 handler는 fire count만 받는다**, 대부분 다른 언어의 `on_fire`/`onFire` 콜백이 받는 `(timer, count)`가 아니다 |
| `close()` | timer를 닫음 |

**Completion result.** 모든 member는 동기다. sync·async
context-manager 프로토콜을 둘 다 지원한다.

**선택 기준.** 수동적 interval 콜백엔 `on_fire`를, 대신 만료를
poll하려면 `recv`를, socket과 함께 하나의 wait에서 multiplex하려면
`Poller.add_timer`로 등록한다.

---

## Eventing enum

| Enum | 사용처 | 값 |
|---|---|---|
| `MonitorEventMask`(`IntFlag`) | `MonitorEvent.event` | `CONNECTED`, `CONNECT_DELAYED`, `CONNECT_RETRIED`, `LISTENING`, `BIND_FAILED`, `ACCEPTED`, `ACCEPT_FAILED`, `CLOSED`, `CLOSE_FAILED`, `DISCONNECTED`, `MONITOR_STOPPED`, `HANDSHAKE_FAILED_NO_DETAIL`, `CONNECTION_READY`, `HANDSHAKE_FAILED_PROTOCOL`, `HANDSHAKE_FAILED_AUTH`, `PEER_WEIGHT_CHANGED`, `ALL` |
| `PollEventFlag`(`IntFlag`) | `Poller.add_socket`/`modify_socket`/`add_fd`/`modify_fd`, `PollEvents.has_event(...)` | `POLLIN`, `POLLOUT`, `POLLERR`, `POLLPRI`, `POLLITEMS_DFLT`(16, **readiness flag가 아니다** — legacy 기본 용량 상수, 다른 값들과 종류가 다름), `POLLCOMPLETION`(binding runtime worker용으로 예약됨; 자신의 doc comment에 따르면 application 코드는 대개 `POLLIN`/`POLLOUT`을 쓴다) |
| `PollSourceKind`(`IntEnum`) | `PollEvent.source_kind` | `SOCKET`, `FD`, `TIMER` |

**선택 기준.** `MonitorEventMask`/`PollEventFlag`는 Python
`IntFlag`이므로, 값을 결합할 땐 비트 `|` 연산자를 직접 쓴다
(`MonitorEventMask.CONNECTED | MonitorEventMask.DISCONNECTED`) —
varargs나 `EnumSet`이 필요한 언어와 다르다.

---

[`contracts/eventing/`](../../../../bindings/python/src/zlink/contracts/eventing/)와
[Python 바인딩 스펙](../../spec/python/README.ko.md)에서 전체 근거를 확인한다.
