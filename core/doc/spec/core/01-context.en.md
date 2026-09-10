---
title: "Context"
---

[한국어](https://zlink-systems.github.io/zlink/ko/spec/core/01-context/) | English

<!-- zlink-nav:start -->
[Core spec index](README.en.md) | [Previous: Public-contract governance](00-public-contract-governance.en.md) | [Next: Message](02-message.en.md)
<!-- zlink-nav:end -->

# Context

> **What this chapter defines** — the public C ABI contract for what a
> Context owns and how it is created, configured, and safely terminated.

## 1. Context overview

zlink's [Context](glossary.en.md#context) is the top-level container that
holds I/O-processing threads and sockets. Every application must create at
least one Context before using any other zlink API, and every
[socket](glossary.en.md#socket) must belong to some Context.

This document defines the contract for creating a Context, configuring it
with options, and terminating it safely. It is aimed at developers who carry
this contract into the C API and into each language binding.

The related contracts are owned by the following documents.

| Related contract | Owning document |
|---|---|
| Auto HWM budget calculation and admission, and their functions | [Auto HWM](systems/06-auto-hwm.en.md) |
| socket creation, options, and send/receive | [Sockets](socket/README.en.md) |
| message lifecycle and ownership | [Message](02-message.en.md) |

## 2. What a Context owns

A Context owns the following.

- **I/O thread pool** — the set of [I/O threads](glossary.en.md#io-thread)
  that actually handle network send and receive. The number of threads,
  their scheduling priority, and their CPU affinity are all set through
  Context options.
- **socket container** — the parent of every [socket](glossary.en.md#socket)
  created from this Context. The upper bound on the number of sockets that
  may be open at once is also a Context option.
- **shared configuration** — values that apply to the whole context, such as
  thread names, the maximum message size, and the
  [Auto HWM](glossary.en.md#auto-hwm-budget) policy that automatically sizes
  socket queues.

A Context is thread-safe. Multiple threads may share the same Context handle
at once and query or set options concurrently.

## 3. Lifecycle and shutdown

A Context's lifecycle proceeds in the order **create → use → shutdown signal
→ resource release**.

- **Create** — `zlink_ctx_new` creates a Context with default option values.
- **Shutdown signal** — `zlink_ctx_shutdown` only signals that every
  blocking operation on sockets belonging to this Context should
  immediately unwind with `ETERM`. It is a non-blocking call that does not
  release resources. A `zlink_recv` unwound this way returns
  `ZLINK_RECV_TERMINATED` and leaves receive outputs unchanged. A `zlink_send`
  unwound this way returns `ZLINK_SUBMIT_TERMINATED`, consumes every input slot
  like any other submit failure (leaving each empty and initialized), and yields completion
  ID `0`.
- **Resource release** — `zlink_ctx_term` destroys the Context. This call
  may block until every socket created within the Context has closed. Each
  Context must be terminated exactly once.

If multiple threads are using sockets concurrently, call shutdown before
term to avoid deadlock — calling term alone, without shutdown, can stall
waiting for sockets to close. Setting `ZLINK_CTX_OPT_BLOCKY` to `0` makes the
default `LINGER` value `0` for subsequently created sockets. Those sockets
close without waiting for undelivered messages, so term returns sooner. Term
itself still waits for internal cleanup to finish regardless of this option.

```mermaid
sequenceDiagram
    participant App as Application
    participant Ctx as Context
    participant S as Sockets
    App->>Ctx: zlink_ctx_new()
    Note over Ctx: create I/O thread pool
    App->>S: create and use sockets
    App->>Ctx: zlink_ctx_shutdown() (non-blocking)
    Ctx-->>S: all blocking operations return ETERM immediately
    App->>S: close each socket
    App->>Ctx: zlink_ctx_term()
    Note over Ctx: wait for all sockets to close, then destroy
```

## 4. Options

The table defines each option value, type, access API, default, and application point.
Set/get means `zlink_ctx_set`/`zlink_ctx_get`; set_data/get_data means the corresponding data APIs.

| Option (`ZLINK_` prefix) | Value·Type·Access | Initial or read-only value | Application point |
|---|---|---|---|
| `IO_THREADS` | `1`; `int`, set/get | `4` | Queries reflect the setting immediately; the pool size is fixed when the runtime first starts |
| `MAX_SOCKETS` | `2`; `int`, set/get | `4095`, reduced to the poller limit | Queries reflect the setting immediately; slot capacity is fixed when the runtime first starts |
| `SOCKET_LIMIT` | `3`; `int`, get-only | `65535`, reduced to the poller limit | Always returns the current platform hard limit |
| `THREAD_PRIORITY` | `22`; `int`, set/get | `-1` | Applies to I/O threads started after the setting |
| `THREAD_SCHED_POLICY` | `4`; `int`, set/get | `-1` | Applies to I/O threads started after the setting |
| `MSG_T_SIZE` | `6`; `int`, get-only | `64`, equal to `sizeof(zlink_msg_t)` | Returns the compile-time ABI size |
| `THREAD_AFFINITY_CPU_ADD` | `7`; `int`, set-only | Empty CPU set | Adds the CPU immediately and applies the set to I/O threads started afterward |
| `THREAD_AFFINITY_CPU_REMOVE` | `8`; `int`, set-only | Empty CPU set | Removes the CPU immediately and applies the set to I/O threads started afterward |
| `THREAD_NAME_PREFIX` | `9`; Byte string, set_data/get_data | Length `0` | Applies to I/O threads started after the setting |
| `CTX_OPT_BLOCKY` | `10`; `int`, set/get | `1` | Applies to the default `LINGER` of sockets created afterward |
| `CTX_OPT_AUTO_HWM_ENABLE` | `12`; `int`, set/get | `1` | Stores the value and schedules recalculation, including existing sockets |
| `CTX_OPT_AUTO_HWM_RECALC_DEBOUNCE_MS` | `14`; `int`, set/get | `3000` ms | Stores the value and schedules recalculation with that debounce |
| `CTX_OPT_AUTO_HWM_PROFILE` | `17`; `int`, set/get | `BALANCED` | Stores the value and schedules recalculation, including existing sockets |
| `CTX_OPT_AUTO_HWM_MEMORY_LIMIT_BYTES` | `19`; `uint64_t`, set_data/get_data | `0` | Stores the value and schedules recalculation, including existing sockets |
| `CTX_OPT_AUTO_HWM_RUNTIME_MEMORY_LIMIT_BYTES` | `20`; `uint64_t`, set_data/get_data | `0` | Stores the value and schedules recalculation, including existing sockets |
| `CTX_OPT_AUTO_HWM_CORE_BUDGET_BYTES` | `21`; `uint64_t`, set_data/get_data | `0` | Stores the value and schedules recalculation, including existing sockets |

```c
typedef enum zlink_auto_hwm_profile_t
{
    ZLINK_AUTO_HWM_PROFILE_COMPACT = 0,      // Smallest memory share — use when memory is tight
    ZLINK_AUTO_HWM_PROFILE_LOW_LATENCY = 1,  // Keeps queues short to reduce latency
    ZLINK_AUTO_HWM_PROFILE_BALANCED = 2,     // Default. Trades off memory against throughput
    ZLINK_AUTO_HWM_PROFILE_THROUGHPUT = 3    // Largest memory share — use for high-volume traffic
} zlink_auto_hwm_profile_t;
```

The exact memory share, fixed cap, and per-role bounds of each profile are
owned by [Auto HWM §2](systems/06-auto-hwm.en.md#2-auto-hwm-budget-calculation).

What budget the three Auto HWM byte options (`MEMORY_LIMIT_BYTES`,
`RUNTIME_MEMORY_LIMIT_BYTES`, and `CORE_BUDGET_BYTES`) compute and how it is
used in admission is owned by [Auto HWM](systems/06-auto-hwm.en.md).

### 4.1 Default values

```c
#define ZLINK_IO_THREADS_DFLT           4  // Default number of I/O threads
#define ZLINK_MAX_SOCKETS_DFLT          4095  // Default maximum socket count
#define ZLINK_THREAD_PRIORITY_DFLT      -1  // Default priority (OS default)
#define ZLINK_THREAD_SCHED_POLICY_DFLT  -1  // Default scheduling policy (OS default)
#define ZLINK_CTX_AUTO_HWM_ENABLE_DFLT  1  // Automatic HWM enabled by default (falls back to balanced when disabled or manual HWM is unset)
#define ZLINK_CTX_AUTO_HWM_RECALC_DEBOUNCE_MS_DFLT 3000  // Default recalculation debounce (ms)
#define ZLINK_CTX_AUTO_HWM_PROFILE_DFLT ZLINK_AUTO_HWM_PROFILE_BALANCED  // Default profile
#define ZLINK_CTX_AUTO_HWM_MEMORY_LIMIT_BYTES_DFLT ((uint64_t) 0)  // No explicit limit set
#define ZLINK_CTX_AUTO_HWM_RUNTIME_MEMORY_LIMIT_BYTES_DFLT ((uint64_t) 0)  // No runtime hint
#define ZLINK_CTX_AUTO_HWM_CORE_BUDGET_BYTES_DFLT ((uint64_t) 0)  // No manual Core budget set
```

`SNDBUF` / `RCVBUF` default to `-1`. This value means zlink does not set the
OS socket buffer size directly and instead leaves it to the OS default and
TCP autotuning. Auto-HWM profiles do not change this value automatically.

## 5. Functions

### zlink_ctx_new

Create a new zlink context.

```c
ZLINK_EXPORT void *zlink_ctx_new(void);
```

Allocates and initializes a new context with default option values. The context
manages a pool of I/O threads and serves as the foundation for creating
sockets. Every socket must be associated with a context. When the context is no
longer needed, release it with `zlink_ctx_term`.

**Returns:** A context handle on success, or `NULL` on failure (errno is set).

**Thread safety:** Safe to call from any thread. The returned context handle
may be shared across threads.

**See also:** `zlink_ctx_term`, `zlink_ctx_set`

---

### zlink_ctx_term

Terminate the context and release all associated resources.

```c
ZLINK_EXPORT zlink_close_result_t zlink_ctx_term(void *context_);
```

Destroys the context. This call may block until all sockets created within the
context have been closed. Any blocking operations on sockets belonging to the
context will return with `ETERM` after `zlink_ctx_shutdown` is called or when
all sockets are closed. Each context must be terminated exactly once.

**Returns:** `ZLINK_CLOSE_OK` on success; otherwise a `zlink_close_result_t` value. `zlink_errno()` retains the detailed internal errno for diagnostics.

**Errors:**
- `EFAULT` -- invalid context handle.
- `EINTR` -- termination was interrupted by a signal; may be retried.

**Thread safety:** Safe to call from any thread, but must be called exactly
once per context. Do not use the context handle after this call returns.

**See also:** `zlink_ctx_new`, `zlink_ctx_shutdown`

---

### zlink_ctx_shutdown

Shut down the context immediately.

```c
ZLINK_EXPORT zlink_close_result_t zlink_ctx_shutdown(void *context_);
```

Signals all blocking operations on sockets belonging to this context to return
immediately with `ETERM`. This is a non-blocking call that initiates shutdown
but does not release resources. `zlink_ctx_term` must still be called
afterwards for final cleanup. Calling shutdown before term avoids deadlocks
when sockets are used across multiple threads.

**Returns:** `ZLINK_CLOSE_OK` on success; otherwise a `zlink_close_result_t` value. `zlink_errno()` retains the detailed internal errno for diagnostics.

**Errors:**
- `EFAULT` -- invalid context handle.

**Thread safety:** Safe to call from any thread.

**See also:** `zlink_ctx_term`

---

### zlink_ctx_set

Set a context option.

```c
ZLINK_EXPORT zlink_config_result_t zlink_ctx_set(void *context_, zlink_ctx_option_t option_, int optval_);
```

Configures the context before or after sockets have been created. Refer to the
option list in §4 for valid option names and their semantics. Setting
`ZLINK_IO_THREADS` or `ZLINK_MAX_SOCKETS` succeeds at any time and is reflected
by subsequent queries, but the actual I/O thread pool and socket-slot capacity
are fixed once, using the values in effect when the context runtime first
starts. Changing either value later does not change the runtime capacity. The
runtime starts with the first socket creation, but if the control runtime is
requested earlier — for example by scheduling an Auto HWM recalculation with a
positive debounce — it starts at that point instead.
`ZLINK_CTX_OPT_AUTO_HWM_ENABLE` also applies to existing sockets: changing it
schedules an automatic recalculation with a default debounce of 3000 ms. Call
`zlink_ctx_auto_hwm_recalculate` if a new plan is needed before then. Only
sockets without manually configured `SNDHWM` or `RCVHWM` values are
recalculated under the automatic policy. Setting the option to `0` preserves
the last HWM applied to each current pipe, excludes those pipes from subsequent
automatic recalculation, and clears the snapshot planning-active flag.
`ZLINK_CTX_OPT_AUTO_HWM_PROFILE` changes the profile used by the next automatic
HWM calculation and may be adjusted safely at runtime. The profile selects the
memory percentage and per-role byte bounds. `SNDBUF` / `RCVBUF` default to
`-1`, and Auto HWM profiles do not change these values automatically. The three
Auto HWM byte options cannot be set with `zlink_ctx_set`; attempting to do so
fails with `EINVAL`. See [Auto HWM](systems/06-auto-hwm.en.md) for the contract.

**Returns:** `ZLINK_CONFIG_OK` on success; otherwise a `zlink_config_result_t` value. `zlink_errno()` retains the detailed internal errno for diagnostics.

**Errors:**
- `EINVAL` -- unknown option or invalid value.
- `EFAULT` -- invalid context handle (`ZLINK_CONFIG_INVALID_HANDLE`).

**Thread safety:** Safe to call from any thread.

**See also:** `zlink_ctx_set_data`, `zlink_ctx_get`

---

### zlink_ctx_set_data

Set a context option from a byte buffer.

```c
ZLINK_EXPORT zlink_config_result_t zlink_ctx_set_data(void *context_,
                                         zlink_ctx_option_t option_,
                                         const void *optval_,
                                         size_t optvallen_);
```

Each of the three Auto HWM byte options requires exactly `sizeof(uint64_t)`
bytes. `0` means the input is unset, not unlimited. Every other size, and any
context option value not in the enum above, fails with `ZLINK_CONFIG_INVALID_ARGUMENT`.
Setting a valid value stores it and then schedules an Auto HWM recalculation.
The setter still succeeds if the new budget cannot accommodate both the
current manual HWM and the automatic minima. In that case, the planner does
not lower the automatic minima and sets
`ZLINK_AUTO_HWM_BUDGET_FLAG_INSUFFICIENT` in the budget snapshot. See
[Auto HWM](systems/06-auto-hwm.en.md) for the contract.

`ZLINK_THREAD_NAME_PREFIX` takes a null-terminated string. Pass the string
pointer as `optval_` and `strlen(prefix) + 1` as `optvallen_`. The prefix is
bounded to at most 16 bytes (`optvallen_ <= 16`) to fit the platform
thread-name limit.

**Returns:** `ZLINK_CONFIG_OK` on success; otherwise a `zlink_config_result_t` value. `zlink_errno()` retains the detailed internal errno for diagnostics.

**Errors:**
- `EINVAL` -- unknown option or invalid value.
- `EFAULT` -- invalid context handle (`ZLINK_CONFIG_INVALID_HANDLE`).

**Thread safety:** Safe to call from any thread.

**See also:** `zlink_ctx_set`, `zlink_ctx_get_data`, `zlink_ctx_get`

---

### zlink_ctx_get_data

Get a context option into caller-provided storage.

```c
ZLINK_EXPORT zlink_config_result_t zlink_ctx_get_data(void *context_,
                                         zlink_ctx_option_t option_,
                                         void *optval_,
                                         size_t *optvallen_);
```

Each of the three Auto HWM byte options requires a `uint64_t` output buffer and
an exact `*optvallen_` of `sizeof(uint64_t)` on input. Any other size, including
a larger scratch buffer or a 4-byte one, fails with
`ZLINK_CONFIG_INVALID_ARGUMENT` and `errno == EINVAL` instead of truncating or
partially filling the value. The call writes the required `sizeof(uint64_t)` to
`*optvallen_`; a successful call leaves the same size there.

`ZLINK_THREAD_NAME_PREFIX` is also read through this function. Pass the output
buffer capacity in `*optvallen_`. If the capacity is smaller than the stored
prefix length, the call writes the required length to `*optvallen_` and fails
with `ZLINK_CONFIG_INVALID_ARGUMENT` and `EINVAL`. Otherwise it copies the
prefix bytes and updates `*optvallen_` to the copied length.

**Returns:** `ZLINK_CONFIG_OK` on success; otherwise a
`zlink_config_result_t` value. `zlink_errno()` retains the detailed internal
errno for diagnostics.

**Errors:**
- `EINVAL` -- unknown option, invalid output size, or NULL output pointer (`ZLINK_CONFIG_INVALID_ARGUMENT`).
- `EFAULT` -- invalid context handle (`ZLINK_CONFIG_INVALID_HANDLE`).

**Thread safety:** Safe to call from any thread.

**See also:** `zlink_ctx_set_data`, `zlink_ctx_get`

---

### zlink_ctx_get

Get a context option.

```c
ZLINK_EXPORT int zlink_ctx_get(void *context_, zlink_ctx_option_t option_, zlink_config_result_t *error_out_);
```

Retrieves the current value of a context option. Can be used at any time to
inspect the context configuration, including read-only options such as
`ZLINK_SOCKET_LIMIT` and `ZLINK_MSG_T_SIZE`. Writes the configuration result
(`zlink_config_result_t`) into `*error_out_` on failure; returns the option
value as the primary return on success. `error_out_` is optional: when it is
`NULL`, a failure records no result code, and only the `-1` return value and
errno are observable.

**Returns:** The option value on success, or `-1` on failure with the
`zlink_config_result_t` written through `*error_out_`. `zlink_errno()` retains
the detailed internal errno for diagnostics.

**Errors:**
- `EINVAL` -- unknown option.
- `EFAULT` -- invalid context handle; `*error_out_` is set to `ZLINK_CONFIG_INVALID_HANDLE`.

**Thread safety:** Safe to call from any thread.

**See also:** `zlink_ctx_set`, `zlink_ctx_get_data`

## 6. Implementation and contract-test verification

Verification uses only the public surface (`zlink_ctx_*` functions, option
set/get, return values, and errno). Each item below maps to a single unit
test.

**Lifecycle**
- `zlink_ctx_new` returns a non-NULL handle on success, or `NULL` with errno set on failure.
- Calling `zlink_ctx_shutdown` causes blocking operations on sockets belonging to that context to return immediately with `ETERM`.
- `zlink_ctx_term` succeeds exactly once per context and may block until every socket inside it has closed.
- Calling `zlink_ctx_term` or `zlink_ctx_shutdown` with an invalid context handle produces `EFAULT`.
- If a signal interrupts `zlink_ctx_term`, it fails with `EINTR` and may be retried.

**Options**
- `zlink_ctx_set` with an unknown option or an invalid value produces `EINVAL`; with an invalid handle it produces `EFAULT` (`ZLINK_CONFIG_INVALID_HANDLE`).
- `ZLINK_THREAD_PRIORITY` uses unique value `22` for set/get and does not change the read-only contract of `ZLINK_SOCKET_LIMIT` value `3`.
- Attempting to set any of the three Auto HWM byte options through `zlink_ctx_set` produces `EINVAL` (only `zlink_ctx_set_data` may set them).
- Querying an Auto HWM byte option through `zlink_ctx_get_data` with a size other than exactly `sizeof(uint64_t)` produces `EINVAL` and writes the required size into `*optvallen_`.
- Writing a context option value that is not in the enum through `zlink_ctx_set_data` produces `ZLINK_CONFIG_INVALID_ARGUMENT`.
- Querying `ZLINK_THREAD_NAME_PREFIX` through `zlink_ctx_get_data` with a capacity smaller than the stored prefix length produces `EINVAL` and writes the required length into `*optvallen_`.

**Thread safety**
- Every `zlink_ctx_*` function is safe to call concurrently from multiple threads. Only `zlink_ctx_term` is restricted to exactly once per context.

Verification of Auto HWM budget and admission is owned by [Auto HWM](systems/06-auto-hwm.en.md#5-implementation-and-contract-test-verification-requirements).

<!-- zlink-nav:start -->
[Core spec index](README.en.md) | [Previous: Public-contract governance](00-public-contract-governance.en.md) | [Next: Message](02-message.en.md)
<!-- zlink-nav:end -->
