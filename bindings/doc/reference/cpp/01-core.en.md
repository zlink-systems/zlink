[한국어](01-core.ko.md) | English

[Reference index](README.en.md)

# 01. Core

This category covers the context lifecycle, context options, routing identity, and free
utility/capability functions. Socket creation happens via each concrete socket type's own
constructor (Sockets category), not a factory method here — unlike dotnet's `IContext.CreateXxx()`
methods. The exact signatures are owned by
[`Contracts/Core/`](../../../../bindings/cpp/include/zlink/Contracts/Core/).

---

## `context_t`

A messaging context: the factory and owner of sockets, and the prerequisite for constructing any
socket type. Move constructible/assignable; copy is deleted — a context has exactly one owner at a
time.

```cpp
zlink::context_t ctx;
zlink::context_t ctx_with_threads (zlink::io_thread_count_t::value (4));
```

**Options.**

| Member | Meaning |
| --- | --- |
| `context_t()` | constructs with the default I/O thread count |
| `explicit context_t(io_thread_count_t)` | constructs with an explicit I/O thread count instead of the default |
| `valid()` | whether this context is still usable — `false` after `term()` |
| `shutdown()` | interrupts blocking operations on sockets under this context without closing them |
| `term()` | terminates the context and releases its native resources |
| `options()` | returns `context_options_t` (below), the context-wide option facade |
| `recalculate_auto_hwm()` | forces an immediate recalculation of automatic high-water marks, instead of waiting for the normal debounce interval |

**Completion result.** All synchronous, no return value except `valid()`/`options()`. The
destructor calls `term()` if not already terminated.

**When to use.** One `context_t` per context the application needs; most applications need exactly
one. Call `shutdown()` before destruction when sockets are in use across multiple threads.

---

## `context_options_t`

The typed options facade reached via `ctx.options()`. Getters have no suffix; setters take the new
value.

```cpp
ctx.options ().io_threads (zlink::io_thread_count_t::value (8));
ctx.options ().auto_hwm_profile (zlink::auto_hwm_profile::low_latency);
ctx.options ().add_thread_affinity (zlink::cpu_index_t::value (2));
```

**Options.**

| Member | Type | Meaning |
| --- | --- | --- |
| `io_threads()` | `io_thread_count_t` | I/O thread count |
| `max_sockets()` | `socket_count_t` | context-wide socket cap |
| `max_msg_size()` | `byte_size_t` | per-message size cap |
| `thread_priority()` | `std::optional<thread_priority_t>` | dispatch thread priority |
| `thread_scheduling_policy()` | `thread_scheduling_policy_t` | dispatch thread scheduling policy |
| `thread_name_prefix()` | `std::string` | OS-visible dispatch thread name prefix |
| `blocky()` | `bool` | whether blocking calls actually block vs. fail fast |
| `auto_hwm_enabled()` | `bool` | whether auto-HWM sizing is active |
| `auto_hwm_recalc_debounce()` | `std::chrono::milliseconds` | minimum interval between automatic recalculations |
| `auto_hwm_profile()` | `zlink::auto_hwm_profile` | automatic HWM sizing profile — see the Sockets category |
| `auto_hwm_msg_unit_bytes()` | `byte_count_t` | accounted-byte unit for auto-HWM sizing |
| `socket_limit()` | `socket_count_t` | build's hard cap on `max_sockets` (read-only) |
| `msg_t_size()` | `byte_size_t` | native message struct size, diagnostic only (read-only) |
| `add_thread_affinity(cpu_index_t)` | — | pins an I/O thread to a CPU (setter-only) |
| `remove_thread_affinity(cpu_index_t)` | — | unpins an I/O thread from a CPU (setter-only) |

**Completion result.** Every getter/setter is synchronous.

**When to use.** Adjust before constructing sockets when the defaults don't fit the deployment.
Pair an `auto_hwm_profile`/`auto_hwm_enabled` change with `context_t::recalculate_auto_hwm()` to
apply it immediately.

---

## Strongly-typed option value wrappers

Small value-type wrappers used throughout `context_options_t` and socket options instead of raw
`int`/`uint32_t`, each constructed via a static `value(...)` factory — the wrapper exists
specifically so a mismatched unit doesn't compile.

**Options.**

| Type | Wraps | Meaning |
| --- | --- | --- |
| `io_thread_count_t` | `int` via `::value(int)`/`.value()` | argument to `context_t`'s constructor and `context_options_t::io_threads` |
| `socket_count_t` | `int` via `::value(int)`/`.value()` | `context_options_t::max_sockets`/`socket_limit` |
| `worker_count_t` | `int` via `::value(int)`/`.value()` | worker-thread counts used by higher socket-option facades (Sockets category) |
| `thread_priority_t` | `int` via `::value(int)`/`.value()` | `context_options_t::thread_priority` |
| `cpu_index_t` | `int` via `::value(int)`/`.value()` | `context_options_t::add_thread_affinity`/`remove_thread_affinity` |
| `socket_backlog_t` | `int` via `::value(int)`/`.value()` | `common_socket_options_t::backlog` (Sockets category) |
| `byte_size_t` | `int64_t` via `::bytes(int64_t)`/`.bytes()` | plain byte-size options such as `max_msg_size` |
| `byte_count_t` (Core) | `uint64_t` via `::bytes(uint64_t)`/`.bytes()` | lossless byte count used by HWM and byte-budget options |
| `peer_weight_t` | `uint32_t` via `::value(uint32_t)` | load-balancing weight (Sockets category); throws `std::invalid_argument` outside 0-100 |

**Completion result.** All factories and accessors are `noexcept` except `peer_weight_t::value`,
which validates its range.

**When to use.** Construct these at the call site (`io_thread_count_t::value(4)`) rather than
passing a bare integer.

---

## `routing_id_t`

A binary-safe value type identifying a messaging peer or route, 1 to 255 bytes.

```cpp
auto from_string = zlink::routing_id_t::from (std::string ("worker-3"));
auto from_bytes = zlink::routing_id_t::from (raw_bytes);
auto from_uint = zlink::routing_id_t::from (uint32_t{42});
auto restored = zlink::routing_id_t::from_hex (previously_printed.to_hex ());
```

**Options.**

| Member | Meaning |
| --- | --- |
| `routing_id_t(const uint8_t *bytes_, size_t size_)` | constructor from a raw byte pointer and length |
| `from(const uint8_t*, size_t)` / `from(const std::vector<uint8_t>&)` | copy raw bytes as-is |
| `from(const std::string&)` | copy raw bytes, not UTF-8-validated |
| `from(uint32_t)` | encode as 4-byte big-endian |
| `from(const std::array<uint8_t, 16>&)` | copy a 16-byte value, e.g. a GUID's raw bytes |
| `from_hex(const std::string&)` | restore bytes `to_hex()` previously printed |
| `data()` | pointer to the underlying bytes |
| `size()` | byte length, 1-255 |
| `to_bytes()` | owned copy of the bytes as `std::vector<uint8_t>` |
| `to_string()` | display form: printable UTF-8, then 4-byte-as-uint32, then 16-byte-as-GUID, then a `hex:`-prefixed fallback |
| `to_hex()` | hex encoding, round-trippable with `from_hex` |
| `operator==`/`!=` | value equality |
| `std::hash<routing_id_t>` | specialization enabling use as a key in unordered containers |

**Completion result.** Every factory and accessor is synchronous. Empty input, input over 255
bytes, or a null pointer with nonzero size throws `std::invalid_argument`; a malformed hex string
to `from_hex` throws the same.

**When to use.** `from(const std::string&)` for a human-assigned identity, `from(uint32_t)`/the
16-byte array overload for a numeric or GUID-shaped identity, raw byte overloads when the identity
is already binary. `to_hex()`/`from_hex()` for a durable round trip — `to_string()` is
display-only.

---

## `zlink::version` / `zlink::error_text` / `zlink::has`

Reads the native library's build version, converts a native error code to a message, or checks for
an optional build capability.

```cpp
int major, minor, patch;
zlink::version (major, minor, patch);
const char *message = zlink::error_text (errnum);
bool has_tls = zlink::has ("tls");
```

**Options.**

| Member | Meaning |
| --- | --- |
| `version(int &major_, int &minor_, int &patch_)` | writes the linked native library's major, minor, and patch version numbers into `major_`/`minor_`/`patch_` |
| `error_text(int errnum_) noexcept` | returns the message text for native error code `errnum_`, as a `const char*` the caller must not modify or free |
| `has(const std::string &capability_)` | whether the named optional capability is compiled into this build — recognized names are `"tcp"`, `"ipc"`, `"tls"`, `"ws"`, `"wss"`; any other string returns `false` |

**Completion result.** All three are synchronous, non-throwing.

**When to use.** `version()` to confirm a dynamically-loaded native library matches expectations.
`has(...)` at startup to branch on optional transports.

---

## `stopwatch_t` / `atomic_counter_t` / `thread_t`

A high-resolution stopwatch, a thread-safe integer counter, and a running background thread —
three independent utility resources with the same RAII shape: default-constructible, move-only,
`valid() const noexcept`, `close()` (the destructor calls `close()` if not already closed).

```cpp
zlink::stopwatch_t watch;
uint64_t partial_us = watch.intermediate ();
uint64_t total_us = watch.stop ();

zlink::atomic_counter_t counter;
int new_value = counter.increment ();

zlink::thread_t worker ([] { do_work (); });
worker.join ();
```

**Options.**

| Member | Meaning |
| --- | --- |
| `stopwatch_t::intermediate()` | elapsed microseconds since construction, callable any number of times |
| `stopwatch_t::stop()` | elapsed microseconds since construction, called exactly once to finish |
| `atomic_counter_t::set(int)` | assigns the counter's value |
| `atomic_counter_t::increment()` / `decrement()` | adjusts the counter by one, returning the *new* value |
| `atomic_counter_t::value() const` | reads the current value |
| `thread_t(std::function<void()> task_)` | constructs and immediately runs `task_` on a new thread |
| `thread_t::join()` | blocks until the task finishes |

**Completion result.** All synchronous.

**When to use.** `atomic_counter_t` for a shared count safe across threads. `stopwatch_t` for
benchmarking. `thread_t` for a portable background thread instead of a platform-specific API.

---

See [`Contracts/Core/`](../../../../bindings/cpp/include/zlink/Contracts/Core/) and the
[C++ binding spec](../../spec/cpp/README.en.md) for the full rationale.
