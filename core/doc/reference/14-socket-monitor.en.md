[한국어](14-socket-monitor.ko.md) | English

[Reference index](README.en.md)

# 14. Socket monitor

This category covers the entry points for observing a raw socket's connection, transport,
protocol, and lifecycle state without changing routing or queue state. A monitor is one of three
event families in Core (the others are poller readiness and timer fires — see the Polling and
pollers category); it never affects the socket it observes. The exact signatures are owned by
the [Monitoring specification](../spec/core/07-monitoring.en.md).

---

## `zlink_socket_monitor_open`

Creates a monitor for a socket, starting in receive mode.

```c
zlink_socket_monitor_open_options_t options = { .events = ZLINK_EVENT_ALL };
void *monitor = zlink_socket_monitor_open(s, &options);
```

**Parameters.** `options_->events` is a bitmask of `ZLINK_SOCKET_MONITOR_EVENT_*` (aliased as the
shorter `ZLINK_EVENT_*` names) selecting which events to observe:
`CONNECTED`/`CONNECT_DELAYED`/`CONNECT_RETRIED`/`LISTENING`/`BIND_FAILED`/`ACCEPTED`/
`ACCEPT_FAILED`/`CLOSED`/`CLOSE_FAILED`/`DISCONNECTED`/`MONITOR_STOPPED`/
`HANDSHAKE_FAILED_NO_DETAIL`/`CONNECTION_READY`/`HANDSHAKE_FAILED_PROTOCOL`/
`HANDSHAKE_FAILED_AUTH`/`PEER_WEIGHT_CHANGED`, or `ALL` (`0xFFFF`) for everything; `events == 0`
selects nothing.

**Return and errno.** Returns a monitor handle on success, or `NULL` on failure with `errno` set.

**When to use.** Call this once per socket you need to observe. The monitor starts in **recv
model** — pull events with `zlink_socket_monitor_recv`, or switch to callback-only model with
`zlink_socket_monitor_handler`. Close the handle with `zlink_monitor_close` when no longer
needed.

---

## `zlink_socket_monitor_handler` / `zlink_socket_monitor_recv`

Switches a monitor to callback delivery, or pulls the next queued event — mutually exclusive
modes, like a raw socket's receive modes (Raw receive category).

```c
zlink_socket_monitor_handler(monitor, on_monitor_event, userdata);
// or, in recv model:
zlink_monitor_event_t event;
zlink_socket_monitor_recv(monitor, &event, ZLINK_RECV_FLAGS_NONE);
```

**Parameters.** `handler` takes a `zlink_monitor_handler_fn` and `userdata_`. `recv` takes an
`event_out_` output struct and `flags_` (`ZLINK_RECV_FLAGS_NONE` or `_DONTWAIT`).

**Return and errno.** `handler` returns `zlink_handler_result_t` — `ZLINK_HANDLER_OK` on success;
activating handler mode while already in the other mode returns `EBUSY`. `recv` returns
`zlink_recv_result_t` — `ZLINK_RECV_OK` on success.

**When to use.** With receive mode, event addresses and routing IDs are values inside the
caller-owned output struct; with handler mode, the callback's event pointer and contained values
are borrowed views valid only until the callback returns. `DISCONNECTED.value` is a
`zlink_disconnect_reason_t`, `HANDSHAKE_FAILED_PROTOCOL.value` is a `zlink_protocol_error_t`,
`PEER_WEIGHT_CHANGED.value` is the new weight in `0..10000`; other failure events carry the
errno for that failure. The monitor queue is bounded — when full, Core aggregates identical
high-frequency events and prioritizes connection-state/protocol-error/lifecycle events; the next
status snapshot (`zlink_monitor_status`) reflects the aggregated counts. A delayed consumer never
blocks raw-socket submission on the observed socket. Within one monitor, events queue in commit
order — no wall-clock order is guaranteed across connection I/O threads.

---

## `zlink_monitor_status`

Reads a point-in-time snapshot of the monitor's own state and the observed socket's automatic-HWM
accounting.

```c
zlink_monitor_status_t status;
zlink_monitor_status(monitor, &status);
```

**Parameters.** Only the monitor handle and a caller-owned `zlink_monitor_status_t *status_out_`.

**Return and errno.** Returns `zlink_config_result_t` — `ZLINK_CONFIG_OK` on success.
`status_out_->abi_version` is `ZLINK_MONITOR_STATUS_ABI_VERSION` (currently `2`); each
`detail_flags` bit makes exactly one row of fields valid (`SND_PENDING_MSGS`/
`RCV_PENDING_MSGS`/`AUTO_HWM_BUDGET`/`AUTO_HWM_BUFFERS` — see the Monitoring specification's
detail-bit table for the exact field list per bit); fields outside a present bit's row are zero.
`auto_hwm_connection_bucket_index` is `UINT32_MAX` when no connection bucket applies.

**When to use.** Use this for diagnostics dashboards or health checks that need current HWM
planning/application state (planned vs. applied byte HWM, in-flight bytes, oversize-admission
counters) rather than the event stream itself. Version 2's HWM fields are 64-bit bytes, not the
former 32-bit counts — the old layout is not accepted as a compatibility fallback.

---

## `zlink_monitor_close`

Closes a monitor and releases its resources.

```c
zlink_monitor_close(&monitor);
```

**Parameters.** Takes `void **monitor_p` — a pointer to the handle, which the call may clear.

**Return and errno.** Returns `zlink_close_result_t` — `ZLINK_CLOSE_OK` on success.

**When to use.** Close every monitor you open, exactly once, when done observing.

---

## `zlink_monitor_ignore_handler`

A no-op event handler that drains events without taking any action.

```c
zlink_socket_monitor_handler(monitor, zlink_monitor_ignore_handler, NULL);
```

**Parameters.** Matches the `zlink_monitor_handler_fn` signature so it can be registered
directly.

**Return and errno.** None — it neither retains nor releases the event or `userdata_`.

**When to use.** Register this when you want handler-mode semantics (so the monitor queue never
grows unbounded) but the application currently has nothing to do with the events themselves —
`event` is a borrowed view valid only for the call, same as any other handler-mode callback.

---

See the [Monitoring specification](../spec/core/07-monitoring.en.md) for the full rationale. The
[Events catalog](../spec/core/05-events.en.md) relates monitor events to poller readiness and
timer fires, the other two event families.
