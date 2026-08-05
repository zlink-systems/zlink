[한국어](01-core.ko.md) | English

[Reference index](README.en.md)

# 01. Core

This category covers the context lifecycle, context options, routing identity, and the crate-root
free functions. **Unlike every other wrapper binding covered so far, these free functions are not
declared under `contracts/core/`** — they live at the crate root in
[`src/lib.rs`](../../../../bindings/rust/src/lib.rs). `Stopwatch`/`AtomicCounter`/`Thread` are
declared in `contracts/core/utilities.rs`. The exact signatures are owned by
[`contracts/core/`](../../../../bindings/rust/src/contracts/core/) and
[`src/lib.rs`](../../../../bindings/rust/src/lib.rs).

---

## `Context::new()`

Creates a messaging context — the factory and owner of sockets.

```rust
let ctx = Context::new()?;
```

**Options.** No parameters.

**Completion result.** Returns `Result<Context, ConfigError>` — **context creation itself is
fallible here**, unlike every other language covered so far, where the equivalent factory has no
error path in its public signature. `Context` is `Send`/`Sync` and may be shared across threads
(for example via `std::sync::Arc`); the context terminates when the last owning `Context` value is
dropped, so an owner must stay alive while another thread creates or uses sockets from it.

**When to use.** Call once per context the application needs; most applications need exactly one.
Keep an owning `Context` (or an `Arc<Context>`) alive for as long as any thread is creating or
using sockets from it.

---

## `Context::shutdown()` / `Context::recalculate_auto_hwm()`

Interrupts blocking operations on the context's sockets without closing them, or forces an
immediate recalculation of automatic high-water marks.

```rust
ctx.shutdown()?;
ctx.recalculate_auto_hwm()?;
```

**Options.** Neither takes parameters.

**Completion result.** `shutdown()` returns `Result<(), CloseError>`; `recalculate_auto_hwm()`
returns `Result<(), ConfigError>`. `shutdown` interrupts blocking calls on sockets under this
context but does not drop the context or its sockets. `recalculate_auto_hwm` recomputes automatic
HWM only for sockets still configured with an `AutoHwmProfile`.

**When to use.** Call `shutdown()` before dropping a context with sockets in use across multiple
threads, to avoid a thread blocking on a socket call indefinitely. Call `recalculate_auto_hwm()`
after changing the auto-HWM profile or a message-unit option, to apply new sizing immediately.

---

## `Context::options()`

Reads the context-wide options facade, whose properties govern I/O threads and the defaults every
socket created from the context inherits.

```rust
let options = ctx.options();
options.set_io_threads(8)?;
options.set_auto_hwm_profile(AutoHwmProfile::LowLatency)?;
options.add_thread_affinity(2)?;
```

**Options.** Every getter/setter below returns `Result<T, ConfigError>`.

| Member | Meaning |
| --- | --- |
| `io_threads()` / `set_io_threads(i32)` | I/O thread count |
| `max_sockets()` / `set_max_sockets(i32)` | context-wide socket cap |
| `socket_limit()` | read-only, build's hard cap on `max_sockets` |
| `thread_priority()` / `set_thread_priority(i32)` | dispatch thread priority |
| `thread_scheduling_policy()` / `set_thread_scheduling_policy(i32)` | dispatch thread scheduling policy |
| `max_message_size()` / `set_max_message_size(i32)` | per-message size cap |
| `msg_t_size()` | read-only, native message struct size, diagnostic only |
| `blocky()` / `set_blocky(bool)` | whether blocking calls actually block vs. fail fast |
| `thread_name_prefix()` / `set_thread_name_prefix(&str)` | OS-visible dispatch thread name prefix |
| `auto_hwm_enabled()` / `set_auto_hwm_enabled(bool)` | whether auto-HWM sizing is active |
| `auto_hwm_recalc_debounce()` / `set_auto_hwm_recalc_debounce(Duration)` | minimum interval between automatic recalculations |
| `auto_hwm_profile()` / `set_auto_hwm_profile(AutoHwmProfile)` | automatic HWM sizing profile — see Sockets category |
| `auto_hwm_msg_unit_bytes()` / `set_auto_hwm_msg_unit_bytes(u64)` | accounted-byte unit for auto-HWM; `0` selects the socket-type default |
| `add_thread_affinity(i32)` | setter-only, pins an I/O thread to a CPU |
| `remove_thread_affinity(i32)` | setter-only, unpins an I/O thread from a CPU |

**Completion result.** Every getter/setter is synchronous, returning `Result<_, ConfigError>` (every
option access can fail, unlike languages where the getter/setter throws only on a genuinely
exceptional error).

**When to use.** Adjust before creating sockets when the defaults don't fit the deployment. Pair an
`auto_hwm_profile`/`auto_hwm_enabled` change with `Context::recalculate_auto_hwm()` to apply it
immediately.

---

## `Context::pair_socket()` / `dealer_socket()` / `router_socket()` / `pub_socket()` / `sub_socket()` / `xpub_socket()` / `xsub_socket()` / `stream_socket()`

Creates a socket of the given type from a context, owned by the caller.

```rust
let dealer = ctx.dealer_socket()?;
```

**Options.** None of the eight factory methods takes parameters.

**Completion result.** Each returns `Result<SocketType, ConfigError>` — **socket creation itself is
fallible**, unlike dotnet/java/node/cpp, where the equivalent factory has no error path in its
public signature.

**When to use.** See the Sockets category for each socket type's operations, options, and
capability roles — this entry only covers how each is created.

---

## `RoutingId`

A binary-safe value type identifying a messaging peer or route, 1 to 255 bytes.

```rust
let from_string: RoutingId = "worker-3".into();
let from_bytes: RoutingId = raw_bytes.as_slice().into();
let from_uint32: RoutingId = 42u32.into();
let restored = RoutingId::from_hex(&previously_printed.to_hex())?;
```

**Options.** Rust `From<T>` trait implementations rather than static factory methods — the
idiomatic conversion mechanism, reached via `.into()` or `RoutingId::from(...)`. **Every `From`
conversion and `from_hex` panics** on empty or over-length input rather than returning a `Result`.

| Member | Meaning |
| --- | --- |
| `From<&[u8]>` / `From<&[u8; N]>` | copies the full slice/fixed-size array as-is |
| `From<&str>` | UTF-8 encode |
| `From<u32>` | 4-byte big-endian |
| `From<[u8; 16]>` | 16-byte, e.g. UUID bytes |
| `from_hex(&str)` | restores bytes `to_hex()` printed; panics on malformed input |
| `try_from_hex(&str) -> Result<Self, ConfigError>` | the non-panicking alternative for hex decoding specifically |
| `MAX_LEN` | `usize` constant, `255` |
| `as_bytes()` | defensive copy of the bytes |
| `size()` | byte length, 1-255 |
| `is_empty()` | whether `size()` is zero |
| `to_hex()` | hex encoding, round-trippable with `from_hex`/`try_from_hex` |
| `Display` | formats as printable UTF-8, then 4-byte-as-`u32`, then 16-byte-as-UUID-format, then a `hex:`-prefixed fallback |
| `PartialEq`/`Eq`/`Hash`/`Copy`/`Clone` | derived value semantics |

**Completion result.** Every `From` conversion and `from_hex` is synchronous and panics on invalid
input. Only `try_from_hex` returns `Result<Self, ConfigError>` instead of panicking.

**When to use.** Use the `From`/`.into()` conversions when the input is already known-valid (for
example, a compile-time string literal); use `try_from_hex` instead of `from_hex` whenever the hex
string comes from outside the program and might be malformed, since `from_hex`/every `From`
conversion panics rather than returning an error.

---

## `version()` / `has(capability)` / `strerror(errnum)`

Reads the native library's build version, checks for an optional build capability, or converts a
native error code to a message.

```rust
let (major, minor, patch) = version();
let has_tls = has("tls");
let message = strerror(errnum);
```

**Options.**

| Member | Meaning |
| --- | --- |
| `version()` | `(i32, i32, i32)` tuple of `{major, minor, patch}` |
| `has(capability: &str)` | whether the named optional capability is compiled into this build — recognized names are `"tcp"`, `"ipc"`, `"tls"`, `"ws"`, `"wss"`; any other string returns `false` |
| `strerror(errnum: i32)` | the message text for that native error code |

**Completion result.** All are synchronous with no error path. `version()` returns `(i32, i32,
i32)`. `has` returns `bool`. `strerror` returns `&'static str`.

**When to use.** Use `version()` to confirm the linked native library matches what the application
expects. Use `has(...)` at startup to branch on optional transports. `strerror` is for diagnostics
alongside a native error code surfaced elsewhere (Errors category).

---

## `Stopwatch::start()` / `AtomicCounter::new()` / `Thread::start(task)`

Creates a high-resolution stopwatch, a thread-safe integer counter, or a running background
thread — three independent utility resources, all declared in `contracts/core/utilities.rs`.

```rust
let mut watch = Stopwatch::start()?;
let partial_us = watch.intermediate();
let total_us = watch.stop();

let counter = AtomicCounter::new()?;
let new_value = counter.increment();

let mut thread = Thread::start(|| do_work())?;
thread.join();
```

**Options.**

| Member | Meaning |
| --- | --- |
| `Stopwatch::start()` | no parameters |
| `Stopwatch.intermediate()` / `stop()` | both `u64` microseconds since construction, `&mut self`; `stop` invalidates the handle |
| `Stopwatch.close()` | releases the stopwatch |
| `AtomicCounter::new()` | no parameters |
| `AtomicCounter.set(i32)` | assigns the counter's value |
| `AtomicCounter.increment()` / `decrement()` | adjusts the counter by one, returning the counter's *new* value |
| `AtomicCounter.value()` | reads the current value |
| `AtomicCounter.close()` | releases the counter |
| `Thread::start<F>(task: F)` where `F: FnOnce() + Send + 'static` | runs `task` immediately on the new thread |
| `Thread.join(&mut self)` | blocks until the task finishes; re-raises the task's panic via `resume_unwind` if it panicked |
| `Thread.close()` | releases the thread handle |

**Completion result.** All three constructors return `Result<Self, ConfigError>`. Each type also
implements `Drop`, calling `close()` automatically if not already closed.

**When to use.** Use `AtomicCounter` for a shared count safe across threads. Use `Stopwatch` for
benchmarking — call `intermediate()` any number of times, then `stop()` exactly once. Use `Thread`
for a portable background thread instead of `std::thread` directly, when the zlink runtime needs
to own its lifecycle and re-propagate a task panic through `join()`.

---

## `proxy(...)` / `proxy_steerable(...)` / `sleep(seconds)` / `multipart_close(parts)` / `poll(items, timeout_ms)`

Runs a bidirectional message-forwarding loop between two pollable sources (optionally steerable via
a control source), sleeps the calling thread, closes every message in a multipart slice, or waits
on a fixed array of raw poll items.

```rust
proxy(&frontend, &backend, Some(&capture))?;
proxy_steerable(&frontend, &backend, Some(&capture), &control)?;
sleep(1); // seconds, not milliseconds
multipart_close(&mut parts);
let ready = poll(&mut items, 1000)?;
```

**Options.**

| Member | Meaning |
| --- | --- |
| `proxy(frontend: &dyn Pollable, backend: &dyn Pollable, capture: Option<&dyn Pollable>)` | `capture` is optional; all three take `&dyn Pollable` trait objects rather than concrete socket types — any built-in socket type implements the sealed `Pollable` trait (Eventing category) |
| `proxy_steerable(..., control: &dyn Pollable)` | adds a required `control` source, same `Pollable` shape |
| `sleep(seconds: i32)` | blocks the calling thread; takes whole seconds directly |
| `multipart_close(parts: &mut [Message])` | closes every part in one call |
| `poll(items: &mut [PollItem], timeout_ms: i64)` | a standalone one-shot poll helper distinct from `Poller` (Eventing category); fills `revents` on each `PollItem` in place |

**Completion result.** `proxy`/`proxy_steerable` return `Result<(), ConfigError>` — **fallible here**,
unlike other languages where the equivalent call has no error path and simply blocks until
termination. `sleep`/`multipart_close` have no return value. `poll` returns `Result<i32, RecvError>`
(the ready count).

**When to use.** Use `proxy` for a simple fire-and-forget forwarding loop on its own thread. Use
`proxy_steerable` when the application needs to pause/resume/terminate the loop from another thread
via the control source. Use `multipart_close` to release every `Message` in a received or
constructed multipart slice in one call. Use the standalone `poll(...)` for an ad hoc wait across a
small fixed set of raw file descriptors, and `Poller` instead when the watched set changes over
time or sockets/timers need to be multiplexed (Eventing category).

---

See [`contracts/core/`](../../../../bindings/rust/src/contracts/core/),
[`src/lib.rs`](../../../../bindings/rust/src/lib.rs), and the
[Rust binding spec](../../spec/rust/README.en.md) for the full rationale.
