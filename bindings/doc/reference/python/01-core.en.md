[한국어](01-core.ko.md) | English

[Reference index](README.en.md)

# 01. Core

This category covers the context lifecycle, context options, routing identity, and the
package-root factory/utility functions. **These functions are not declared under `contracts/core/`
at all** — they live at `bindings/python/src/zlink/__init__.py`. `Context`/`ContextOptions` are
`typing.Protocol` types, not concrete classes — the actual runtime implementation lives elsewhere
and is never imported directly. The exact signatures are owned by
[`contracts/core/`](../../../../bindings/python/src/zlink/contracts/core/) and
[`__init__.py`](../../../../bindings/python/src/zlink/__init__.py).

---

## `create_context()`

Creates a messaging context — the factory and owner of sockets.

```python
with create_context() as ctx:
    ...
# or
async with create_context() as ctx:
    ...
```

**Options.** No parameters.

**Completion result.** Returns a `Context`. Supports both the sync (`with`) and async (`async
with`) context-manager protocol — closing it (by either path, or explicit `close()`) terminates
anything still open under it, including sockets created from it.

**When to use.** Call once per context the application needs; most applications need exactly one.

---

## `Context.shutdown()` / `Context.recalculate_auto_hwm()`

Interrupts blocking operations on the context's sockets without closing them, or forces an
immediate recalculation of automatic high-water marks.

```python
ctx.shutdown()
ctx.recalculate_auto_hwm()
```

**Options.** Neither takes parameters.

**Completion result.** Both are synchronous with no return value. `shutdown` interrupts blocking
calls on sockets under this context but does not close the context or its sockets.
`recalculate_auto_hwm` recomputes automatic HWM only for sockets still configured with an
`AutoHwmProfile`.

**When to use.** Call `shutdown()` before closing a context with sockets in use across multiple
threads, to avoid a thread blocking on a socket call indefinitely. Call `recalculate_auto_hwm()`
after changing the auto-HWM profile or a message-unit option, to apply new sizing immediately.

---

## `Context.options`

The context-wide options facade, read via the `options` property on `Context`.

```python
ctx.options.io_threads = 8
ctx.options.auto_hwm_profile = AutoHwmProfile.LOW_LATENCY
ctx.options.add_thread_affinity(2)
```

**Options.** Plain properties are get/set except where noted. **No `thread_priority` property
exists here**, unlike every other language covered so far.

| Member | Meaning |
| --- | --- |
| `io_threads` | I/O thread count |
| `max_sockets` | context-wide socket cap |
| `socket_limit` | read-only, build's hard cap on `max_sockets` |
| `max_message_size` | per-message size cap |
| `msg_t_size` | read-only, native message struct size, diagnostic only |
| `thread_scheduling_policy` | dispatch thread scheduling policy |
| `thread_name_prefix` | OS-visible dispatch thread name prefix |
| `auto_hwm_enabled` | whether auto-HWM sizing is active |
| `auto_hwm_recalc_debounce` | minimum interval between automatic recalculations |
| `blocky` | whether blocking calls actually block vs. fail fast |
| `auto_hwm_profile` | automatic HWM sizing profile — see Sockets category |
| `auto_hwm_msg_unit_bytes` | accounted-byte unit for auto-HWM; `0` selects the socket-type default |
| `add_thread_affinity(cpu)` | pins an I/O thread to a CPU |
| `remove_thread_affinity(cpu)` | unpins an I/O thread from a CPU |

**Completion result.** All property reads/writes and both methods are synchronous.

**When to use.** Adjust before creating sockets when the defaults don't fit the deployment. Pair an
`auto_hwm_profile`/`auto_hwm_enabled` change with `Context.recalculate_auto_hwm()` to apply it
immediately.

---

## `create_pair_socket(ctx)` / `create_dealer_socket(ctx)` / `create_router_socket(ctx)` / `create_pub_socket(ctx)` / `create_sub_socket(ctx)` / `create_xpub_socket(ctx)` / `create_xsub_socket(ctx)` / `create_stream_socket(ctx)`

Creates a socket of the given type from a context, owned by the caller.

```python
dealer = create_dealer_socket(ctx)
```

**Options.** Each factory takes the owning `Context`.

**Completion result.** Each returns its corresponding socket, supporting the context-manager
protocol. The caller owns and must close (or `with`) the returned socket independently of the
context.

**When to use.** See the Sockets category for each socket type's operations, options, and
capability roles — this entry only covers how each is created.

---

## `RoutingId`

A binary-safe value type identifying a messaging peer or route, 1 to 255 bytes.

```python
from_string = RoutingId.from_("worker-3")
from_bytes = RoutingId.from_(raw_bytes)
from_uint32 = RoutingId.from_(42)
from_uuid = RoutingId.from_(uuid.uuid4())
restored = RoutingId.from_hex(previously_printed.to_hex())
```

**Options.**

| Member | Meaning |
| --- | --- |
| `RoutingId(data)` | constructor; copies a bytes-like object of 1..255 bytes, raises `ValueError` out of range |
| `from_(value)` | static factory (named with a trailing underscore since `from` is a Python keyword); dispatches on the argument's type — `str` (UTF-8 encode), `int` (4-byte big-endian uint32, `0..4294967295`), `uuid.UUID` (16 bytes), or any bytes-like object (copied) |
| `from_hex(value: str)` | restores bytes `to_hex()` printed |
| `size` | property, byte length, 1-255 |
| `to_bytes()` | defensive copy of the bytes |
| `to_hex()` | hex encoding, round-trippable with `from_hex` |
| `__str__` | display form: printable UTF-8, then 4-byte-as-`int`, then 16-byte-as-`uuid.UUID`, then a `hex:`-prefixed fallback |
| `__bytes__` | the raw bytes, via `bytes(routing_id)` |
| `__len__` | byte length, via `len(routing_id)` |
| `__hash__` | value-based hash |
| `__eq__` | value equality; also accepts a raw bytes-like value for comparison, not just another `RoutingId` |
| `__repr__` | debug representation |

**Completion result.** Every factory and accessor is synchronous. Out-of-range length raises
`ValueError`; a malformed hex string to `from_hex` raises `TypeError`/`ValueError`.

**When to use.** Use `from_(value)` for any input type — its dispatch covers strings, ints, UUIDs,
and raw bytes in one call, unlike languages with separate overloads/factories per type. Use
`to_hex()`/`from_hex()` specifically for a durable raw-byte round trip — `str(routing_id)` is for
display only and is not guaranteed reversible.

---

## `version()` / `has(capability)` / `strerror(errnum)`

Reads the native library's build version, checks for an optional build capability, or converts a
native error code to a message.

```python
major, minor, patch = version()
has_tls = has("tls")
message = strerror(errnum)
```

**Options.**

| Member | Meaning |
| --- | --- |
| `version()` | `(major, minor, patch)` tuple |
| `has(capability: str)` | whether the named optional capability is compiled into this build — recognized names are `"tcp"`, `"ipc"`, `"tls"`, `"ws"`, `"wss"`; any other string returns `False` |
| `strerror(errnum: int)` | the message text for that native error code |

**Completion result.** All are synchronous with no error path. `version()` returns a
`(major, minor, patch)` tuple. `has` returns `bool`. `strerror` returns `str`.

**When to use.** Use `version()` to confirm the linked native library matches what the application
expects. Use `has(...)` at startup to branch on optional transports. `strerror` is for diagnostics
alongside a native error code surfaced elsewhere (Errors category).

---

## `create_atomic_counter()` / `create_stopwatch()` / `create_thread(target)`

Creates a thread-safe integer counter, a high-resolution stopwatch, or a running background
thread — three independent utility resources, all `Protocol` types declared in
`contracts/core/utilities.py`.

```python
with create_atomic_counter() as counter:
    new_value = counter.increment()

with create_stopwatch() as watch:
    partial_us = watch.intermediate()
    total_us = watch.stop()

thread = create_thread(do_work)
thread.join()
```

**Options.** None of the three factories takes parameters beyond `create_thread`'s target callable.

| Member | Meaning |
| --- | --- |
| `AtomicCounter.set(value)` | assigns the counter's value |
| `AtomicCounter.increment()` / `decrement()` | adjusts the counter by one, returning the *new* value |
| `AtomicCounter.value` | property, reads the current value |
| `Stopwatch.intermediate()` / `stop()` | both microseconds since construction; `intermediate()` callable any number of times, `stop()` called exactly once to finish |
| `Stopwatch.close()` | releases the stopwatch |
| `Thread.join()` | blocks until the task finishes — **the only member**; no `close()`, unlike every other utility type here, which supports the context-manager protocol |

**Completion result.** `AtomicCounter`/`Stopwatch` support both sync and async context-manager
protocols; `Thread` does not (it has no `close()`/`__enter__` at all).

**When to use.** Use `AtomicCounter` for a shared count safe across threads. Use `Stopwatch` for
benchmarking — call `intermediate()` any number of times, then `stop()` exactly once. Use
`create_thread` for a portable background thread instead of Python's `threading.Thread` directly,
when the zlink runtime needs to own its lifecycle.

---

## `proxy(...)` / `proxy_steerable(...)` / `sleep(seconds)` / `multipart_close(parts)`

Runs a bidirectional message-forwarding loop between two sockets (optionally steerable via a
control socket), sleeps the calling thread, or closes every message in a multipart sequence.

```python
proxy(frontend, backend, capture)  # capture may be None; blocks until context termination
proxy_steerable(frontend, backend, capture, control)
sleep(1)  # seconds, not milliseconds
multipart_close(parts)
```

**Options.**

| Member | Meaning |
| --- | --- |
| `proxy(frontend, backend, capture=None)` | `capture` is optional |
| `proxy_steerable(frontend, backend, capture, control)` | adds a required `control` socket |
| `sleep(seconds)` | blocks the calling thread; takes whole seconds directly |
| `multipart_close(parts)` | closes every message in one call |

**Completion result.** All are synchronous with no return value. `proxy`/`proxy_steerable` block
the calling thread until the context is terminated (or, for `proxy_steerable`, until a control
command or error ends the loop) — run either on a dedicated thread.

**When to use.** Use `proxy` for a simple fire-and-forget forwarding loop. Use `proxy_steerable`
when the application needs to pause/resume/terminate the loop from another thread via the control
socket. Use `multipart_close` to release every message in a received or constructed multipart
sequence in one call instead of a hand-written loop.

---

See [`contracts/core/`](../../../../bindings/python/src/zlink/contracts/core/),
[`__init__.py`](../../../../bindings/python/src/zlink/__init__.py), and the
[Python binding spec](../../spec/python/README.en.md) for the full rationale.
