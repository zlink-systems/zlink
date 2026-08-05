[한국어](17-utilities.ko.md) | English

[Reference index](README.en.md)

# 17. Utilities

This category covers the standalone helper entry points that complement the messaging API:
atomic counters, a high-resolution stopwatch, and miscellaneous process helpers (capability
query, proxy loops, sleep, and OS thread management). None of these depend on a context or
socket. The exact signatures are owned by the
[Utilities specification](../spec/core/08-utilities.en.md).

---

## `zlink_atomic_counter_new` / `zlink_atomic_counter_destroy`

Creates an atomic counter initialized to zero, or destroys one.

```c
void *counter = zlink_atomic_counter_new();
// ...
zlink_atomic_counter_destroy(&counter);
```

**Parameters.** `new` takes no arguments. `destroy` takes `void **counter_p_` (cleared to `NULL`
after destruction).

**Return and errno.** `new` returns a counter handle, or `NULL` on failure (out of memory).
`destroy` returns nothing (`void`).

**When to use.** Create one counter per independent shared count the application needs across
threads. Never call `destroy` while another thread is operating on the same counter.

---

## `zlink_atomic_counter_set` / `zlink_atomic_counter_value`

Sets the counter to an explicit value, or reads its current value.

```c
zlink_atomic_counter_set(counter, 0);
int current = zlink_atomic_counter_value(counter);
```

**Parameters.** `set` takes the new `value_`. `value` takes only the counter handle.

**Return and errno.** `set` returns nothing. `value` returns the current value, read atomically.

**When to use.** Use `set` only during setup, before other threads begin operating on the
counter — unlike `inc`/`dec`/`value`, `set` is not safe to call concurrently with other
operations on the same counter. `value` is safe from any thread at any time.

---

## `zlink_atomic_counter_inc` / `zlink_atomic_counter_dec`

Atomically increments or decrements the counter by one.

```c
int previous = zlink_atomic_counter_inc(counter);
int still_nonzero = zlink_atomic_counter_dec(counter);
```

**Parameters.** Both take only the counter handle.

**Return and errno.** `inc` returns the value immediately *before* the increment. `dec` returns
`1` if the counter is still greater than zero after the decrement, or `0` if it reached zero —
not the numeric value.

**When to use.** Use `dec`'s zero-reaching return directly as a "last one out" signal (e.g. a
reference-count-to-zero check) without a separate `value` read — that combined check-and-decrement
is what makes it atomic as one operation.

---

## `zlink_stopwatch_start` / `zlink_stopwatch_intermediate` / `zlink_stopwatch_stop`

Starts a high-resolution stopwatch, reads elapsed time without stopping it, or stops it and reads
the total.

```c
void *watch = zlink_stopwatch_start();
// ... work ...
unsigned long partial_us = zlink_stopwatch_intermediate(watch);
// ... more work ...
unsigned long total_us = zlink_stopwatch_stop(watch);
```

**Parameters.** `start` takes no arguments. `intermediate`/`stop` take only the watch handle.

**Return and errno.** `start` returns an opaque handle, or `NULL` on failure. Both
`intermediate` and `stop` return elapsed microseconds since `start`. `stop` also releases the
handle — it must not be used afterward.

**When to use.** Call `intermediate` as many times as needed for successive readings during one
measurement window; call `stop` exactly once to finish and release the handle. Use one handle
from one thread at a time — `intermediate` must not be called concurrently with `stop` on the
same handle.

---

## `zlink_has`

Checks whether the current library build provides a named capability.

```c
bool has_tls = zlink_has("tls");
```

**Parameters.** `capability_` is a non-`NULL`, NUL-terminated string, not retained by the call.

**Return and errno.** Returns `bool` — `"tcp"` always returns `true`; `"ipc"`, `"tls"`, `"ws"`,
`"wss"` return `true` only if that capability is present in the build; any other string returns
`false`.

**When to use.** Use this at startup to branch on optional build capabilities (e.g. skip
configuring TLS options if `zlink_has("tls")` is `false`) rather than assuming every transport is
compiled in.

---

## `zlink_proxy` / `zlink_proxy_steerable`

Runs a bidirectional forwarding loop between two raw sockets, blocking the calling thread until
it ends.

```c
zlink_proxy(frontend, backend, capture); // capture may be NULL

// or, with external control:
zlink_proxy_steerable(frontend, backend, capture, control);
```

**Parameters.** `frontend_`/`backend_` are required raw socket handles the proxy forwards
multipart messages between; `capture_` is optional — if non-`NULL`, it receives a copy of every
forwarded message. `zlink_proxy_steerable` additionally takes an optional `control_` socket that
accepts `PAUSE`/`RESUME`/`TERMINATE`/`STATISTICS` commands. Every handle is borrowed — neither
function closes or takes ownership of any of them.

**Return and errno.** Both return `zlink_config_result_t` — `ZLINK_CONFIG_OK` when the proxy
loop ends normally. `ZLINK_CONFIG_INVALID_HANDLE` for a `NULL` required handle, or a non-`NULL`
handle that isn't a raw socket.

**When to use.** Use plain `zlink_proxy` for a fire-and-forget forwarding loop you'll run on a
dedicated thread with no runtime control. Use `zlink_proxy_steerable` when the application needs
to pause, resume, or cleanly terminate the loop, or pull statistics, from another thread via the
control socket — the `STATISTICS` reply follows the control socket's ordinary raw send/receive
contract. `zlink_proxy_steerable` blocks until `TERMINATE`, context termination, or an error ends
it.

---

## `zlink_sleep`

Suspends the calling thread for at least the given number of seconds.

```c
zlink_sleep(1);
```

**Parameters.** `seconds_` is the minimum sleep duration in whole seconds.

**Return and errno.** None (`void`).

**When to use.** A portable convenience wrapper — use it instead of a platform-specific sleep
call when only whole-second granularity is needed.

---

## `zlink_thread_start` / `zlink_thread_join`

Starts a new OS thread running a given function, or waits for one to finish and releases its
handle.

```c
void *thread = zlink_thread_start(worker_fn, arg);
// ...
zlink_thread_join(thread);
```

**Parameters.** `start` takes a `zlink_thread_fn *func_` and `arg_` (passed through as the
function's sole argument). `join` takes the thread handle.

**Return and errno.** `start` returns an opaque thread handle, or `NULL` on failure. `join`
returns nothing (`void`) and releases the handle — do not use it afterward.

**When to use.** Use this pair for a portable background thread instead of a platform-specific
API. Call `join` exactly once per handle, and never from the thread being joined.

---

See the [Utilities specification](../spec/core/08-utilities.en.md) for the full rationale.
