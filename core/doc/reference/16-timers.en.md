[한국어](16-timers.ko.md) | English

[Reference index](README.en.md)

# 16. Timers

This category covers the entry points for a standalone generic timer: nanosecond-resolution
periodic or one-shot scheduling, consumed synchronously or by callback, and optionally
integrated into a poller (`zlink_poller_add_timer`/`zlink_poller_remove_timer`, Polling and
pollers category). A timer fire is one of Core's three event families (the others are socket
monitor events and poller readiness — Socket monitor and Polling and pollers categories). The
exact signatures are owned by the [Utilities specification](../spec/core/08-utilities.en.md).

---

## `zlink_timer_new` / `zlink_timer_destroy`

Creates a standalone timer handle, or destroys one.

```c
void *timer = zlink_timer_new();
// ...
zlink_timer_destroy(&timer);
```

**Parameters.** `new` takes no arguments. `destroy` takes `void **timer_p_` (cleared to `NULL`
after destruction).

**Return and errno.** `new` returns a timer handle, or `NULL` on failure with `errno` set.
`destroy` returns `zlink_close_result_t` — `ZLINK_CLOSE_OK` on success; it stops the timer first
if running.

**When to use.** Create one timer handle per independent schedule the application needs. Destroy
it exactly once, and never while another thread is using the same handle.

---

## `zlink_timer_start` / `zlink_timer_stop`

Arms a timer to fire on an interval, or disarms it.

```c
zlink_timer_start(timer, /*interval_ns=*/100_000_000, /*repeat_count=*/0);
// ...
zlink_timer_stop(timer);
```

**Parameters.** `interval_ns_` is the fire interval in nanoseconds. `repeat_count_` is the number
of fires before the timer stops itself (`0` means indefinite, until `stop` is called explicitly).

**Return and errno.** Both return `zlink_config_result_t` — `ZLINK_CONFIG_OK` on success.

**When to use.** Use a finite `repeat_count_` for a bounded one-shot or fixed-count schedule that
needs no explicit stop; use `0` for an ongoing periodic timer and call `stop` when done. Neither
call is safe to make concurrently with other operations on the same timer.

---

## `zlink_timer_recv` / `zlink_timer_handler`

Consumes timer fires synchronously, or attaches a callback — mutually exclusive, like other
Core receive/callback pairs (Raw receive category).

```c
uint64_t fire_count;
zlink_timer_recv(timer, &fire_count);
// or:
zlink_timer_handler(timer, on_timer_fire, userdata);
```

**Parameters.** `recv` takes an output `fire_count_out_` (the cumulative fire count). `handler`
takes a `zlink_timer_handler_fn` (receiving the timer handle, cumulative fire count, and
`userdata_`) — `NULL` is invalid.

**Return and errno.** `recv` returns `zlink_recv_result_t` — `ZLINK_RECV_OK` on success,
`ZLINK_RECV_NO_DATA` (`EAGAIN`) when the timer has stopped with no fire event left to receive.
`handler` returns `zlink_handler_result_t` — `ZLINK_HANDLER_OK` on success,
`ZLINK_HANDLER_INVALID_ARGUMENT`/`EINVAL` for a `NULL` handler. After attaching a handler,
`zlink_timer_recv` on the same timer returns `ZLINK_RECV_BUSY`.

**When to use.** Use `recv` for a synchronous, poll-style wait on a fire; use `handler` for
push-style delivery. Neither call is safe to make concurrently with other operations on the same
timer. To integrate a timer into an existing event loop alongside sockets and FDs instead of
either of these, use `zlink_poller_add_timer` (Polling and pollers category) and drain with
`zlink_timer_recv` once the poller reports it ready.

---

See the [Utilities specification](../spec/core/08-utilities.en.md) for the full rationale.
