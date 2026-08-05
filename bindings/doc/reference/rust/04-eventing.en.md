[한국어](04-eventing.ko.md) | English

[Reference index](README.en.md)

# 04. Eventing

This category covers socket monitoring, the reusable poller, and standalone timers —
`SocketMonitor::open(...)`, `Poller::new()`, and `Timer::new()` respectively. `Timer` is declared
in this category's `poller.rs` (alongside `Poller`), not in a separate timer module. The exact
signatures are owned by
[`contracts/eventing/`](../../../../bindings/rust/src/contracts/eventing/).

---

## `SocketMonitor`

Observes a socket's connection lifecycle events and reads its current status.

```rust
let mut monitor = SocketMonitor::open(&socket)?;
monitor.on_event(|event| println!("{:?} {}", event.event, event.remote_addr))?;
let status = monitor.status()?;
```

**Options.**

| Member | Meaning |
| --- | --- |
| `open(socket: &dyn Monitorable) -> Result<Self, ConfigError>` | **takes no event-mask parameter and always subscribes to every event**, per its own doc comment ("Open a socket monitor for all events") |
| `recv(&self) -> Result<MonitorEvent, RecvError>` | blocking pull of the next event |
| `recv_with_flags(&self, flags: RecvFlags) -> Result<Option<MonitorEvent>, RecvError>` | non-blocking variant, `Ok(None)` when nothing pending |
| `status()` / `snapshot()` | returns a `MonitorStatus` point-in-time snapshot; equivalent — `snapshot` is an alias for `status` |
| `on_event<F>(&mut self, handler: F) -> Result<(), HandlerError> where F: Fn(&MonitorEvent) + Send + 'static` | registers a passive lifecycle-event callback |
| `ignore_handler() -> fn(&MonitorEvent)` | a static no-op handler a caller can register |
| `close(&mut self) -> Result<(), CloseError>` | closes the monitor |

**Completion result.** All members are synchronous. `Monitorable` is a sealed marker trait every
built-in socket type implements; a crate consumer cannot implement it for a custom type.

**When to use.** Use `on_event` for a passive lifecycle observer registered once; use `recv`/
`recv_with_flags` for a pull-based drain loop instead. Use `status()` for a point-in-time snapshot.

---

## `SocketMonitorEventMask` (declared but unreachable as a filter)

A typed bitmask intended for subscribing a monitor to a subset of events — but **`SocketMonitor::open`
takes no such parameter**, so this type has no way to actually filter a subscription through this
binding's public contract. The underlying implementation has a `pub(crate)`
`socket_monitor_open_with_events(socket, events)` that `open()` calls internally, always passing
`SocketMonitorEventMask::ALL` — that function is not exported.

**Options.** The wrapped `u32` field is private, so a consumer cannot construct an arbitrary mask
directly, only combine the two named ones via `BitOr`/`BitOrAssign`.

| Member | Meaning |
| --- | --- |
| `ALL` | `0x7FFF` |
| `CONNECTION_READY` | `0x1000` |
| `bits()` | reads the raw value |
| `MONITOR_EVENT_ALL` / `MONITOR_EVENT_CONNECTION_READY` | top-level convenience aliases for the same two constants |

**Completion result.** N/A — a plain value type.

**When to use.** Not applicable from application code today — `SocketMonitor::open` always
subscribes to every event regardless of any `SocketMonitorEventMask` value constructed. Whether
`open` should gain an events parameter, or whether this type should be removed, is a spec-level
question outside this reference's scope.

---

## `MonitorEvent`

A single socket connection-lifecycle event reported by a monitor.

```rust
if event.is_connected() { /* ... */ }
```

**Options.** Convenience predicate methods cover only a subset of the full lifecycle event set —
**no predicate exists for `ConnectDelayed`, `ConnectRetried`, `BindFailed`, `AcceptFailed`,
`CloseFailed`, `MonitorStopped`, `HandshakeFailedNoDetail`, `HandshakeFailedProtocol`,
`HandshakeFailedAuth`, or `PeerWeightChanged`** — a caller must bit-test `event.0` against the
corresponding raw mask value directly for any of those.

| Field | Type | Meaning |
| --- | --- | --- |
| `event` | `MonitorEventType`, a newtype wrapping `u64` — not an enum with named variants | the kind of lifecycle event |
| `value` | `u32` | an event-specific value |
| `routing_id` | `Option<RoutingId>` | the peer routing id, present when the event provides one |
| `local_addr` / `remote_addr` | `String` | the local/remote address associated with the event |
| `is_connected()` / `is_disconnected()` / `is_listening()` / `is_accepted()` / `is_closed()` / `is_connection_ready()` | `bool` | predicate for that specific event kind |

**Completion result.** N/A — an immutable value delivered by the monitor.

**When to use.** Use the named `is_*` predicates for the six lifecycle transitions they cover; for
any other event kind, compare `event.0` against the documented bit value directly (see core's
Errors/Eventing spec for the full mask table).

---

## `MonitorStatus`

A point-in-time snapshot of a monitored entity's state and auto-high-water-mark telemetry, returned
by `SocketMonitor::status()`/`snapshot()`. A plain public-field struct.

**Options.** No parameters — every field is public.

| Group | Fields |
|---|---|
| ABI identity | `abi_version`, `struct_size` (`u32`) |
| Source/state | `source_kind` (`MonitorSourceKind`: only `Socket`), `state_flags`/`detail_flags` (`u32` bitmasks), `is_ready()`/`is_closed()` (computed methods) |
| Pending counts | `snd_pending_msgs`, `rcv_pending_msgs` (`u64`) |
| Auto-HWM config | `auto_hwm_enabled` (`bool`), `auto_hwm_profile`/`auto_hwm_role`/`auto_hwm_policy_class` (`u32`), `auto_hwm_unit_budget_bytes`/`auto_hwm_socket_message_slots` (`u64`), `auto_hwm_size_cap` (`u32`) |
| Connection bucket | `auto_hwm_connection_bucket_enabled` (`bool`), `auto_hwm_connection_bucket_count`/`_index`/`_hwm_4k` (`u32`, index is `u32::MAX` when no bucket applies), `auto_hwm_connection_bucket_hysteresis_retained` (`bool`) |
| Auto-HWM plan (bytes) | `auto_hwm_effective_message_bytes`, `auto_hwm_planned_sndhwm_bytes`/`_rcvhwm_bytes`, `auto_hwm_applied_sndhwm_bytes`/`_rcvhwm_bytes` (`u64`), `auto_hwm_effective_sndbuf`/`_rcvbuf` (`i32`) |
| Auto-HWM recalc | `auto_hwm_last_recalc_ms` (`u64`), `auto_hwm_last_recalc_reason` (`AutoHwmRecalcReason`, Core category), `auto_hwm_send_blocked_ratio_ppm` (`u32`) |
| Auto-HWM deferred shrink | `auto_hwm_deferred_sndhwm_bytes`/`_rcvhwm_bytes` (`u64`, valid only when the matching `auto_hwm_deferred_sndhwm_valid`/`_rcvhwm_valid` `bool` is true) |
| In-flight/charging | `snd_bytes_in_flight`, `rcv_bytes_in_flight`, `minimum_core_message_charge_bytes`, `oversize_message_admission_count`, `oversize_message_admission_max_bytes` (`u64`) |

**Completion result.** N/A — plain public fields, plus the two computed methods `is_ready()`/
`is_closed()`.

**When to use.** Call `is_ready()`/`is_closed()` instead of decoding `state_flags` directly. Use the
connection-bucket and auto-HWM-plan fields when diagnosing why a socket's effective send/receive
HWM differs from its configured `CommonSocketOptions` value (Sockets category).

---

## `Poller`

Multiplexes sockets, file descriptors, and timers on a single reusable wait.

```rust
let poller = Poller::new()?;
poller.add_socket(&dealer, POLLIN, 1)?;
poller.add_timer(&timer, 2)?;
let mut events = vec![PollEvent::default(); 8];
let ready = poller.wait(&mut events, 1000)?;
```

**Options.** `Pollable` is a sealed marker trait every built-in socket type implements — a crate
consumer cannot implement it for a custom type.

| Member | Meaning |
| --- | --- |
| `new() -> Result<Self, ConfigError>` | creates the poller |
| `add_socket(&self, socket: &dyn Pollable, events: i16, slot: usize) -> Result<(), ConfigError>` | registers a socket; `slot` is a caller token echoed back in the matching poll result |
| `add_fd(&self, fd: RawFd, events: i16, slot: usize)` | registers a raw file descriptor, same shape |
| `add_timer(&self, timer: &Timer, slot: usize)` | registers a timer to be multiplexed alongside sockets/fds |
| `modify_socket(&self, socket, events)` / `modify_fd(&self, fd, events)` | replaces the watched events for an already-registered socket/fd |
| `remove_socket(&self, socket)` / `remove_fd(&self, fd)` / `remove_timer(&self, timer: &Timer)` | unregisters the source |
| `wait(&self, events: &mut [PollEvent], timeout_ms: i64) -> Result<usize, RecvError>` | blocks up to `timeout_ms`, writing up to `events.len()` results in place; a negative timeout blocks indefinitely |
| `size(&self) -> i32` | the number of currently registered sources |

**Completion result.** Registration/removal members return `Result<(), ConfigError>`. `wait`
returns `Result<usize, RecvError>` — the ready count, writing up to `events.len()` results in
place. Modifying a registration to add or remove `POLLCOMPLETION` specifically requires
`remove_socket` + `add_socket` again rather than `modify_socket`, per its own doc comment — because
completion processing has separate ownership in Core.

**When to use.** Use one poller across a service's lifetime. Reuse one `Vec<PollEvent>` buffer
across `wait` calls rather than allocating one per wait.

---

## `PollEvent` / `PollItem`

`PollEvent` is one ready source reported by `Poller::wait`; `PollItem` is a raw poll descriptor used
by the standalone `poll(...)` free function (Core category) instead of `Poller`.

**Options — `PollEvent`** (implements `Default`, all fields public).

| Field | Type | Meaning |
| --- | --- | --- |
| `source_kind` | `PollSourceKind`: `Socket`/`Fd`/`Timer` | whether the source is a socket, file descriptor, or timer |
| `fd` | `RawFd` | the file descriptor, populated for `Fd`-kind sources |
| `slot` | `usize` | the caller token supplied at registration |
| `revents` | `i16` | a mask of `POLL*` constants |
| `is_readable()` / `is_writable()` | `bool` | convenience bit-test against `POLLIN`/`POLLOUT` |

**Options — `PollItem`** (all fields public).

| Field | Type | Meaning |
| --- | --- | --- |
| `fd` | `RawFd` | the file descriptor this item watches |
| `events` / `revents` | `i16` | watched / returned poll-event bitmask |

**Completion result.** N/A — plain value types.

**When to use.** Branch on `PollEvent::source_kind`/`slot` to route each `Poller::wait` result back
to the socket, descriptor, or timer it corresponds to. Use `PollItem` only with the standalone
`poll(...)` function, not with `Poller`.

---

## `Timer`

A timer that fires on an interval and can be polled or awaited, created independently of `Poller`
but registerable with one via `Poller::add_timer`.

```rust
let mut timer = Timer::new()?;
timer.on_fire(|_timer, count| println!("fired {count} times"))?;
timer.start(1_000_000_000, 0)?; // interval in nanoseconds
```

**Options.**

| Member | Meaning |
| --- | --- |
| `new() -> Result<Self, ConfigError>` | creates the timer |
| `start(&self, interval_ns: u64, repeat_count: u64) -> Result<(), ConfigError>` | starts firing on `interval_ns`; **the interval is nanoseconds**, unlike every other language's `Duration`/millisecond-based `start`; `repeat_count == 0` means unlimited |
| `stop(&self) -> Result<(), ConfigError>` | stops firing; restartable via `start` |
| `recv(&self) -> Result<Option<u64>, RecvError>` | the cumulative fire count, `Ok(None)` when nothing pending |
| `on_fire<F>(&mut self, handler: F) -> Result<(), HandlerError> where F: Fn(&Timer, u64) + Send + 'static` | registers a passive interval callback |

**Completion result.** All members are synchronous.

**When to use.** Use `on_fire` for a passive interval callback; use `recv` to poll expirations
instead, or register the timer with `Poller::add_timer` to multiplex it alongside sockets on one
wait.

---

## Eventing constants

| Constant | Used by | Values |
|---|---|---|
| `POLLIN` / `POLLOUT` / `POLLCOMPLETION` (`i16`) | `Poller::add_socket`/`modify_socket`/`add_fd`/`modify_fd`, `PollItem.events`/`.revents` | `1`, `2`, `32` — **no `POLLERR`/`POLLPRI` constant exists in this binding's public contract**, unlike every other language covered so far |
| `MonitorSourceKind` | `MonitorStatus::source_kind` | `Socket` |
| `PollSourceKind` | `PollEvent::source_kind` | `Socket`, `Fd`, `Timer` |

---

See [`contracts/eventing/`](../../../../bindings/rust/src/contracts/eventing/) and the
[Rust binding spec](../../spec/rust/README.en.md) for the full rationale.
