[한국어](04-eventing.ko.md) | English

[Reference index](README.en.md)

# 04. Eventing

This category covers socket monitoring, the reusable poller, and standalone timers — created via
`open_socket_monitor(...)`, `create_poller()`, and `create_timer()` respectively, all reached from
the package root (Core category). The exact signatures are owned by
[`contracts/eventing/`](../../../../bindings/python/src/zlink/contracts/eventing/).

---

## `MonitorSocket`

Observes a socket's connection lifecycle events and reads its current status.

```python
monitor = open_socket_monitor(socket)
monitor.on_event(lambda event: print(event.event, event.remote_addr))
status = monitor.status()
```

**Options.**

| Member | Meaning |
| --- | --- |
| `status()` | returns a `MonitorStatus` point-in-time snapshot |
| `close()` | closes the monitor |
| `recv(*, flags=0)` | pulls the next event; returns `MonitorEvent` or `None` when `DONT_WAIT` is set and none is available |
| `on_event(handler)` | registers a passive lifecycle-event callback, invoked on a background dispatch thread |
| `ignore_handler` | a `staticmethod` (`lambda event: None`) a caller can register to explicitly discard events |

**Completion result.** All members are synchronous. Supports both sync and async context-manager
protocols.

**When to use.** Use `on_event` for a passive lifecycle observer registered once; use `recv` for a
pull-based drain loop instead. Use `status()` for a point-in-time snapshot.

---

## `MonitorStatus`

A point-in-time snapshot of a monitored socket's state and auto-high-water-mark telemetry,
returned by `MonitorSocket.status()`. A concrete class (not a `Protocol`) with a large
keyword-only `__init__`.

**Options.** Constructor takes every field as a keyword-only argument; several accept `None`
defaults for forward-compatibility. Notable naming duplication: `auto_hwm_applied_sndhwm_bytes`
and `auto_hwm_applied_sndhwm` are separate constructor parameters, but the `__init__` falls back to
the `_bytes` value when the short-named one is omitted (same pattern for `_rcvhwm`,
`_deferred_sndhwm`, `_deferred_rcvhwm`) — a caller reading the instance sees both attribute names
holding the same value.

| Group | Attributes |
|---|---|
| ABI identity | `abi_version`, `struct_size` |
| Source/state | `source_kind`, `state_flags`/`detail_flags` (bitmasks), `is_ready()` (computed method) |
| Pending counts | `snd_pending_msgs`, `rcv_pending_msgs` |
| Auto-HWM config | `auto_hwm_enabled`, `auto_hwm_profile`/`auto_hwm_role`/`auto_hwm_policy_class`, `auto_hwm_unit_budget_bytes`/`auto_hwm_socket_message_slots`, `auto_hwm_size_cap` |
| Connection bucket | `auto_hwm_connection_bucket_enabled`, `auto_hwm_connection_bucket_count`/`_index`/`_hwm_4k`, `auto_hwm_connection_bucket_hysteresis_retained` |
| Auto-HWM plan (bytes) | `auto_hwm_effective_message_bytes`, `auto_hwm_planned_sndhwm_bytes`/`_rcvhwm_bytes`, `auto_hwm_applied_sndhwm_bytes`/`_rcvhwm_bytes` (and the aliased `auto_hwm_applied_sndhwm`/`_rcvhwm`), `auto_hwm_effective_sndbuf`/`_rcvbuf` |
| Auto-HWM recalc | `auto_hwm_last_recalc_ms`, `auto_hwm_last_recalc_reason`, `auto_hwm_send_blocked_ratio_ppm` |
| Auto-HWM deferred shrink | `auto_hwm_deferred_sndhwm_bytes`/`_rcvhwm_bytes` (and aliases), valid only when `auto_hwm_deferred_sndhwm_valid`/`_rcvhwm_valid` is true |
| In-flight/charging | `snd_bytes_in_flight`, `rcv_bytes_in_flight`, `minimum_core_message_charge_bytes`, `oversize_message_admission_count`, `oversize_message_admission_max_bytes` |

**Completion result.** N/A — plain instance attributes, plus the computed `is_ready()` method.

**When to use.** Read `is_ready()` instead of decoding `state_flags` directly. Use the
connection-bucket and auto-HWM-plan attributes when diagnosing why a socket's effective send/
receive HWM differs from its configured `CommonSocketOptions` value (Sockets category).

---

## `MonitorEvent`

A single socket connection-lifecycle event reported by a monitor. A concrete class with a
keyword-only `__init__`.

**Options.** Constructor keyword arguments are all required, no defaults; each becomes a plain
instance attribute of the same name.

| Field | Meaning |
| --- | --- |
| `event` | the kind of lifecycle event (a `MonitorEventMask` value) |
| `value` | an event-specific value such as an error code or reconnect interval |
| `routing_id` | the peer routing id, present when the event provides one |
| `local_addr` / `remote_addr` | the local/remote address associated with the event |

**Completion result.** N/A — an immutable-in-practice value delivered by the monitor (nothing
prevents mutation, but nothing in the contract calls for it).

**When to use.** Read `event` (a `MonitorEventMask` value) to branch on the lifecycle transition;
`value` carries event-specific detail (an error code or reconnect interval, for example).

---

## `Poller`

Multiplexes sockets, file descriptors, and timers on a single reusable wait.

```python
poller = create_poller()
poller.add_socket(dealer, PollEventFlag.POLLIN, slot=1)
poller.add_timer(timer, slot=2)
events = create_poll_events(8)
ready = poller.wait(events, timeout_ms=1000)
```

**Options.**

| Member | Meaning |
| --- | --- |
| `add_socket(socket, events, slot)` | registers a socket; `slot` is a caller token echoed back in the matching result |
| `add_fd(fd, events, slot)` | registers a raw file descriptor, same shape |
| `add_timer(timer, slot)` | registers a timer to be multiplexed alongside sockets/fds |
| `modify_socket(socket, events)` / `modify_fd(fd, events)` | replaces the watched events for an already-registered socket/fd |
| `remove_socket(socket)` / `remove_fd(fd)` / `remove_timer(timer)` | unregisters the source |
| `size()` | the number of currently registered sources |
| `wait(events, timeout_ms)` | blocks up to `timeout_ms`, filling `events` in place; a negative timeout blocks indefinitely |
| `close()` | closes the poller |

**Completion result.** Registration/removal members are synchronous. `wait` blocks up to
`timeout_ms`, filling `events` in place and returning the ready count. Supports both sync and async
context-manager protocols.

**When to use.** Use one poller across a service's lifetime. Reuse one `PollEvents` buffer across
`wait` calls rather than creating one per wait.

---

## `PollEvents` / `PollEvent`

`PollEvents` is a reusable buffer of poll results filled by `Poller.wait(...)`, created via
`create_poll_events(capacity)` (Core category) — the same design as java's/node's pre-allocated
result buffer. `PollEvent` is a `@dataclass(frozen=True)` materializing one result on demand.

```python
events = create_poll_events(16)
poller.wait(events, timeout_ms=500)
for i in range(events.ready_count):
    if events.has_event(i, PollEventFlag.POLLIN):
        ...
```

**Options — `PollEvents`.**

| Member | Meaning |
| --- | --- |
| `capacity` | property, the fixed capacity passed to `create_poll_events(...)` |
| `ready_count` | property, how many of the slots hold a ready event after the last `wait` |
| `source_kind(index)` | the ready source's kind at that index |
| `slot(index)` | the caller token supplied when that source was registered |
| `revents(index)` | raw poll-event bitmask for that index |
| `fd(index)` | the file descriptor at that index, populated for FD-kind sources |
| `has_event(index, event)` | convenience bit-test over `revents(index)` |
| `event(index)` | materializes a `PollEvent` for that index |

**Options — `PollEvent`** (frozen dataclass).

| Field | Meaning |
| --- | --- |
| `source_kind` | whether the source is a socket, file descriptor, or timer |
| `slot` | the caller token supplied at registration |
| `revents` | raw poll-event bitmask |
| `fd` | the file descriptor, defaults to `0` when the source isn't FD-kind |

**Completion result.** All `PollEvents` accessors are synchronous. `PollEvent` is immutable
(`frozen=True`).

**When to use.** Prefer `has_event(index, flag)` over manually bit-testing `revents(index)`. Use
`event(index)` only when a caller genuinely needs the materialized `PollEvent` — iterating the
individual accessors avoids that allocation on a hot polling loop.

---

## `Timer`

A timer that fires on an interval and can be polled or awaited.

```python
timer = create_timer()
timer.on_fire(lambda count: print(f"fired {count} times"))
timer.start(interval_ns=1_000_000_000, repeat_count=0)
```

**Options.**

| Member | Meaning |
| --- | --- |
| `start(interval_ns: int, repeat_count: int)` | starts firing on `interval_ns`; **the interval is nanoseconds**, matching rust's `Timer::start`, not the millisecond/`Duration`-based `start` every other language covered so far uses; `repeat_count == 0` means unlimited |
| `stop()` | stops firing; restartable via `start` |
| `recv()` | returns `Optional[int]` — the cumulative fire count, `None` when nothing pending |
| `on_fire(handler)` | registers a passive interval callback, invoked on a background dispatch thread — **the handler here receives only the fire count**, not `(timer, count)` the way most other languages' `on_fire`/`onFire` callbacks do |
| `close()` | closes the timer |

**Completion result.** All members are synchronous. Supports both sync and async context-manager
protocols.

**When to use.** Use `on_fire` for a passive interval callback; use `recv` to poll expirations
instead, or register the timer with `Poller.add_timer` to multiplex it alongside sockets on one
wait.

---

## Eventing enums

| Enum | Used by | Values |
|---|---|---|
| `MonitorEventMask` (`IntFlag`) | `MonitorEvent.event` | `CONNECTED`, `CONNECT_DELAYED`, `CONNECT_RETRIED`, `LISTENING`, `BIND_FAILED`, `ACCEPTED`, `ACCEPT_FAILED`, `CLOSED`, `CLOSE_FAILED`, `DISCONNECTED`, `MONITOR_STOPPED`, `HANDSHAKE_FAILED_NO_DETAIL`, `CONNECTION_READY`, `HANDSHAKE_FAILED_PROTOCOL`, `HANDSHAKE_FAILED_AUTH`, `PEER_WEIGHT_CHANGED`, `ALL` |
| `PollEventFlag` (`IntFlag`) | `Poller.add_socket`/`modify_socket`/`add_fd`/`modify_fd`, `PollEvents.has_event(...)` | `POLLIN`, `POLLOUT`, `POLLERR`, `POLLPRI`, `POLLITEMS_DFLT`(16, **not a readiness flag** — a legacy default-capacity constant, distinct in kind from the others), `POLLCOMPLETION` (reserved for binding runtime workers; application code generally uses `POLLIN`/`POLLOUT` per its own doc comment) |
| `PollSourceKind` (`IntEnum`) | `PollEvent.source_kind` | `SOCKET`, `FD`, `TIMER` |

**When to use.** Because `MonitorEventMask`/`PollEventFlag` are Python `IntFlag`, combine values
with the bitwise `|` operator directly (`MonitorEventMask.CONNECTED | MonitorEventMask.DISCONNECTED`)
— unlike languages that need varargs or an `EnumSet`.

---

See [`contracts/eventing/`](../../../../bindings/python/src/zlink/contracts/eventing/) and the
[Python binding spec](../../spec/python/README.en.md) for the full rationale.
