한국어 | [English](https://zlink-systems.github.io/zlink/bindings/reference/rust/04-eventing/)

[레퍼런스 목차](README.ko.md)

# 04. Eventing

이 category는 socket monitoring, 재사용 가능한 poller, standalone
timer를 다룬다 — 각각 `SocketMonitor::open(...)`, `Poller::new()`,
`Timer::new()`. `Timer`는 별도 timer 모듈이 아니라 이 category의
`poller.rs`에(`Poller`와 나란히) 선언돼 있다. 정확한 signature는
[`contracts/eventing/`](../../../../bindings/rust/src/contracts/eventing/)가
소유한다.

---

## `SocketMonitor`

socket의 connection lifecycle event를 관찰하고 현재 상태를 읽는다.

```rust
let mut monitor = SocketMonitor::open(&socket)?;
let event = monitor.recv()?;
println!("{:?} {}", event.event, event.remote_addr);
let status = monitor.status()?;
```

**Options.**

| Member | 의미 |
| --- | --- |
| `open(socket: &dyn Monitorable) -> Result<Self, ConfigError>` | **event-mask 인자를 받지 않으며, 자신의 doc comment에 따르면 항상 모든 event를 구독한다**("Open a socket monitor for all events") |
| `open_with_options(socket, SocketMonitorOpenOptions) -> Result<Self, ConfigError>` | 명시적 event mask와 monitor byte HWM으로 연다 |
| `recv(&self) -> Result<MonitorEvent, RecvError>` | 다음 event를 blocking으로 가져옴 |
| `recv_with_flags(&self, flags: RecvFlags) -> Result<Option<MonitorEvent>, RecvError>` | non-blocking 변형, 대기 중인 게 없으면 `Ok(None)` |
| `status()` | 시점 스냅샷 `MonitorStatus`를 반환 |
| `close(&mut self) -> Result<(), CloseError>` | monitor를 닫음 |

**Completion result.** 모든 member는 동기다. `Monitorable`은 모든 내장
socket type이 구현하는 sealed marker trait다 — crate 사용자는 custom
타입에 대해 이걸 구현할 수 없다.

**선택 기준.** caller-driven pull loop에는 `recv`/`recv_with_flags`를 쓰고
시점 스냅샷에는 `status()`를 쓴다. Monitor 전달에는 등록형 callback이 없다.

---

## `SocketMonitorEventMask`(monitor filter option)

monitor를 event의 부분집합에 구독시키는 typed bitmask다.
`SocketMonitor::open()`은 전체 event를 선택하고,
`open_with_options(socket, SocketMonitorOpenOptions { events,
monitor_hwm_bytes })`는 명시한 mask와 byte HWM을 적용한다.

**Options.** 감싸인 `u32` 필드가 private이라 사용자는 명명된 mask를
`BitOr`/`BitOrAssign`으로 결합한다.

| Member | 의미 |
| --- | --- |
| `ALL` | `0x7FFFF` |
| `CONNECTION_READY` | `0x1000` |
| `SEND_FLOW_PAUSED` / `SEND_FLOW_RESUMED` / `FLOW_STATE_STALE` | paired DEALER/ROUTER flow-state transition |
| `bits()` | raw 값을 읽음 |
| `MONITOR_EVENT_ALL` / `MONITOR_EVENT_CONNECTION_READY` | 같은 두 상수의 최상위 편의 alias |

**Completion result.** 해당 없음 — 순수 값 타입.

**선택 기준.** 전체 event에는 `open()`을 쓰고 subscription 또는 monitor
queue byte HWM을 명시해야 할 때는 `open_with_options()`를 쓴다.

---

## `MonitorEvent`

monitor가 보고하는 socket connection-lifecycle event 하나.

```rust
if event.is_connected() { /* ... */ }
```

**Options.** 편의 predicate 메서드는 전체 lifecycle event 집합 중
일부만 다룬다 — **`ConnectDelayed`, `ConnectRetried`, `BindFailed`,
`AcceptFailed`, `CloseFailed`, `MonitorStopped`,
`HandshakeFailedNoDetail`, `HandshakeFailedProtocol`,
`HandshakeFailedAuth`, `PeerWeightChanged`엔 predicate가 없다** — 이들
중 어느 것도 caller가 대응하는 raw mask 값에 대해 `event.0`을 직접
bit-test해야 한다.

| Field | 타입 | 의미 |
| --- | --- | --- |
| `event` | `MonitorEventType`, `u64`를 감싸는 newtype — 명명된 variant를 가진 enum이 아님 | lifecycle event의 종류 |
| `value` | `u32` | event별 값 |
| `routing_id` | `Option<RoutingId>` | event가 제공할 때만 존재하는 peer routing id |
| `local_addr` / `remote_addr` | `String` | event에 결부된 local/remote 주소 |
| `is_connected()` / `is_disconnected()` / `is_listening()` / `is_accepted()` / `is_closed()` / `is_connection_ready()` | `bool` | 해당 event 종류에 대한 predicate |

**Completion result.** 해당 없음 — monitor가 전달하는 불변 값.

**선택 기준.** 다루는 6개 lifecycle transition엔 명명된 `is_*`
predicate를 쓴다. 그 외 event 종류는 `event.0`을 문서화된 bit 값과
직접 비교한다(전체 mask 표는 core의 Errors/Eventing 스펙 참고).

---

## `MonitorStatus`

`SocketMonitor::status()`/`snapshot()`이 반환하는, monitored entity의
상태와 auto-high-water-mark telemetry 시점 스냅샷. 순수 public-field
struct다.

**Options.** 인자 없음 — 모든 필드가 public이다.

| 그룹 | 필드 |
|---|---|
| ABI identity | `abi_version`, `struct_size`(`u32`) |
| Source/state | `source_kind`(`MonitorSourceKind`: `Socket`만), `state_flags`/`detail_flags`(`u32` 비트마스크), `is_ready()`/`is_closed()`(계산 method) |
| Pending count | `snd_pending_msgs`, `rcv_pending_msgs`(`u64`) |
| Auto-HWM 설정 | `auto_hwm_enabled`(`bool`), `auto_hwm_profile`/`auto_hwm_role`/`auto_hwm_policy_class`(`u32`), `auto_hwm_unit_budget_bytes`/`auto_hwm_socket_message_slots`(`u64`), `auto_hwm_size_cap`(`u32`) |
| Connection bucket | `auto_hwm_connection_bucket_enabled`(`bool`), `auto_hwm_connection_bucket_count`/`_index`/`_hwm_4k`(`u32`, bucket이 없으면 index는 `u32::MAX`), `auto_hwm_connection_bucket_hysteresis_retained`(`bool`) |
| Auto-HWM plan(byte) | `auto_hwm_effective_message_bytes`, `auto_hwm_planned_sndhwm_bytes`/`_rcvhwm_bytes`, `auto_hwm_applied_sndhwm_bytes`/`_rcvhwm_bytes`(`u64`), `auto_hwm_effective_sndbuf`/`_rcvbuf`(`i32`) |
| Auto-HWM recalc | `auto_hwm_last_recalc_ms`(`u64`), `auto_hwm_last_recalc_reason`(`AutoHwmRecalcReason`, Core category), `auto_hwm_send_blocked_ratio_ppm`(`u32`) |
| Auto-HWM deferred shrink | `auto_hwm_deferred_sndhwm_bytes`/`_rcvhwm_bytes`(`u64`, 대응하는 `auto_hwm_deferred_sndhwm_valid`/`_rcvhwm_valid` `bool`이 true일 때만 유효) |
| In-flight/과금 | `snd_bytes_in_flight`, `rcv_bytes_in_flight`, `minimum_core_message_charge_bytes`, `oversize_message_admission_count`, `oversize_message_admission_max_bytes`(`u64`) |

**Completion result.** 해당 없음 — 순수 public 필드에 더해 계산
method `is_ready()`/`is_closed()` 둘.

**선택 기준.** `state_flags`를 직접 디코딩하는 대신
`is_ready()`/`is_closed()`를 호출한다. socket의 실제 send/receive
HWM이 설정한 `CommonSocketOptions` 값(Sockets category)과 다른 이유를
진단할 땐 connection-bucket과 auto-HWM-plan 필드를 쓴다.

---

## `Poller`

socket, file descriptor, timer를 하나의 재사용 가능한 wait로
multiplex한다.

```rust
let poller = Poller::new()?;
poller.add_socket(&dealer, POLLIN, 1)?;
poller.add_timer(&timer, 2)?;
let mut events = vec![PollEvent::default(); 8];
let ready = poller.wait(&mut events, 1000)?;
```

**Options.** `Pollable`은 모든 내장 socket type이 구현하는 sealed marker
trait다 — crate 사용자는 custom 타입에 대해 이걸 구현할 수 없다.

| Member | 의미 |
| --- | --- |
| `new() -> Result<Self, ConfigError>` | poller를 생성 |
| `add_socket(&self, socket: &dyn Pollable, events: i16, slot: usize) -> Result<(), ConfigError>` | socket을 등록; `slot`은 대응하는 poll 결과로 그대로 되돌아오는 caller token |
| `add_fd(&self, fd: RawFd, events: i16, slot: usize)` | raw file descriptor를 등록, 같은 형태 |
| `add_timer(&self, timer: &Timer, slot: usize)` | timer를 socket/fd와 함께 multiplex하도록 등록 |
| `modify_socket(&self, socket, events)` / `modify_fd(&self, fd, events)` | 이미 등록된 socket/fd의 감시 event를 교체 |
| `remove_socket(&self, socket)` / `remove_fd(&self, fd)` / `remove_timer(&self, timer: &Timer)` | source 등록을 해제 |
| `wait(&self, events: &mut [PollEvent], timeout_ms: i64) -> Result<usize, RecvError>` | `timeout_ms`까지 block하며 `events.len()`까지 결과를 그 자리에 씀; 음수 timeout은 무기한 block |
| `size(&self) -> i32` | 현재 등록된 source 개수 |

**Completion result.** 등록/제거 member는 `Result<(), ConfigError>`를
반환한다. `wait`는 `Result<usize, RecvError>`(준비된 개수)를 반환하며,
`events.len()`까지 결과를 그 자리에 쓴다. `modify_socket`은
`POLLCOMPLETION`을 원자적으로 추가·제거하며 native registration 변경과
함께 completion-drain owner를 이전한다. Public poller가 그 bit를 소유하는
동안 owner는 `wait()`를 계속 호출해 completion을 drain·settle해야 한다.
동시에 blocking terminal이 필요하면 다른 execution context에서 수행한다.

**선택 기준.** 서비스 수명 전체에서 poller 하나를 쓴다. `wait` 호출마다
할당하는 대신 `Vec<PollEvent>` buffer 하나를 재사용한다.

---

## `PollEvent` / `PollItem`

`PollEvent`는 `Poller::wait`가 보고하는 준비된 source 하나다. `PollItem`
은 `Poller` 대신 standalone `poll(...)` 자유 함수(Core category)가
쓰는 raw poll descriptor다.

**Options — `PollEvent`**(`Default` 구현, 모든 필드 public).

| Field | 타입 | 의미 |
| --- | --- | --- |
| `source_kind` | `PollSourceKind`: `Socket`/`Fd`/`Timer` | source가 socket·file descriptor·timer 중 무엇인지 |
| `fd` | `RawFd` | file descriptor, `Fd` kind source에서만 채워짐 |
| `slot` | `usize` | 등록 시 제공한 caller token |
| `revents` | `i16` | `POLL*` 상수의 mask |
| `is_readable()` / `is_writable()` | `bool` | `POLLIN`/`POLLOUT`에 대한 편의 bit-test |

**Options — `PollItem`**(모든 필드 public).

| Field | 타입 | 의미 |
| --- | --- | --- |
| `fd` | `RawFd` | 이 item이 감시하는 file descriptor |
| `events` / `revents` | `i16` | 감시할/반환된 poll-event bitmask |

**Completion result.** 해당 없음 — 순수 값 타입.

**선택 기준.** `PollEvent::source_kind`/`slot`으로 분기해 각
`Poller::wait` 결과를 대응하는 socket·descriptor·timer로 연결한다.
`PollItem`은 `Poller`가 아니라 standalone `poll(...)` 함수와만 쓴다.

---

## `Timer`

interval마다 fire하며 poll하거나 await할 수 있는 timer로, `Poller`와
독립적으로 생성되지만 `Poller::add_timer`로 등록할 수 있다.

```rust
let mut timer = Timer::new()?;
timer.start(1_000_000_000, 0)?; // 나노초 단위 interval
let count = timer.recv()?;
```

**Options.**

| Member | 의미 |
| --- | --- |
| `new() -> Result<Self, ConfigError>` | timer를 생성 |
| `start(&self, interval_ns: u64, repeat_count: u64) -> Result<(), ConfigError>` | `interval_ns`마다 fire를 시작; **interval이 나노초 단위다**, `Duration`/밀리초 기반 `start`를 쓰는 다른 모든 언어와 다르다; `repeat_count == 0`은 무제한 |
| `stop(&self) -> Result<(), ConfigError>` | fire를 멈춤; `start`로 재시작 가능 |
| `recv(&self) -> Result<Option<u64>, RecvError>` | 누적 fire count, 대기 중인 게 없으면 `Ok(None)` |

**Completion result.** 모든 member는 동기다.

**선택 기준.** 만료를 pull하려면 `recv`를, socket과 함께 하나의 wait에서
multiplex하려면 `Poller::add_timer`로 등록한다. Timer 전달에는 등록형
callback이 없다.

---

## Eventing 상수

| 상수 | 사용처 | 값 |
|---|---|---|
| `POLLIN` / `POLLOUT` / `POLLCOMPLETION`(`i16`) | `Poller::add_socket`/`modify_socket`/`add_fd`/`modify_fd`, `PollItem.events`/`.revents` | `1`, `2`, `32` — **이 binding의 public contract엔 `POLLERR`/`POLLPRI` 상수가 없다**, 지금까지 다룬 다른 모든 언어와 다르다 |
| `MonitorSourceKind` | `MonitorStatus::source_kind` | `Socket` |
| `PollSourceKind` | `PollEvent::source_kind` | `Socket`, `Fd`, `Timer` |

---

[`contracts/eventing/`](../../../../bindings/rust/src/contracts/eventing/)와
[Rust 바인딩 스펙](../../spec/rust/README.ko.md)에서 전체 근거를 확인한다.
