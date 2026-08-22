[한국어](https://zlink-systems.github.io/zlink/ko/spec/core/01-context/) | English

[Spec Index](../README.en.md) · [Core Index](README.en.md)

# Context

A context is the top-level container that manages I/O threads and serves as
the foundation for creating sockets. Every application must create at least one
context before using any other zlink API. Contexts are thread-safe and may be
shared across threads.

## Context Option Constants

Set and query `int` options with `zlink_ctx_set` and `zlink_ctx_get`. Use
`zlink_ctx_set_data` and `zlink_ctx_get_data` for the Auto HWM byte options.

```c
typedef enum zlink_ctx_option_t
{
    ZLINK_IO_THREADS              = 1,
    ZLINK_MAX_SOCKETS             = 2,
    ZLINK_SOCKET_LIMIT            = 3,
    ZLINK_THREAD_PRIORITY         = 3,
    ZLINK_THREAD_SCHED_POLICY     = 4,
    ZLINK_MAX_MSGSZ               = 5,
    ZLINK_MSG_T_SIZE              = 6,
    ZLINK_THREAD_AFFINITY_CPU_ADD      = 7,
    ZLINK_THREAD_AFFINITY_CPU_REMOVE   = 8,
    ZLINK_THREAD_NAME_PREFIX      = 9,
    ZLINK_CTX_OPT_BLOCKY          = 10,
    ZLINK_CTX_OPT_AUTO_HWM_ENABLE = 12,
    ZLINK_CTX_OPT_AUTO_HWM_RECALC_DEBOUNCE_MS = 14,
    ZLINK_CTX_OPT_AUTO_HWM_PROFILE = 17,
    /* Value 18 is intentionally unassigned. */
    ZLINK_CTX_OPT_AUTO_HWM_MEMORY_LIMIT_BYTES = 19,
    ZLINK_CTX_OPT_AUTO_HWM_RUNTIME_MEMORY_LIMIT_BYTES = 20,
    ZLINK_CTX_OPT_AUTO_HWM_CORE_BUDGET_BYTES = 21
} zlink_ctx_option_t;
```

```c
typedef enum zlink_auto_hwm_profile_t
{
    ZLINK_AUTO_HWM_PROFILE_COMPACT = 0,
    ZLINK_AUTO_HWM_PROFILE_LOW_LATENCY = 1,
    ZLINK_AUTO_HWM_PROFILE_BALANCED = 2,
    ZLINK_AUTO_HWM_PROFILE_THROUGHPUT = 3
} zlink_auto_hwm_profile_t;
```

| Constant | Value | Description |
|----------|-------|-------------|
| `ZLINK_IO_THREADS` | 1 | Number of I/O threads in the context |
| `ZLINK_MAX_SOCKETS` | 2 | Maximum number of sockets allowed |
| `ZLINK_SOCKET_LIMIT` | 3 | Hard upper limit on socket count (read-only) |
| `ZLINK_THREAD_PRIORITY` | 3 | I/O thread scheduling priority |
| `ZLINK_THREAD_SCHED_POLICY` | 4 | I/O thread scheduling policy |
| `ZLINK_MAX_MSGSZ` | 5 | Maximum message size in bytes (`>= 0`; default `INT_MAX`) |
| `ZLINK_MSG_T_SIZE` | 6 | Size of `zlink_msg_t` in bytes (read-only) |
| `ZLINK_THREAD_AFFINITY_CPU_ADD` | 7 | Add a CPU to the I/O thread affinity set |
| `ZLINK_THREAD_AFFINITY_CPU_REMOVE` | 8 | Remove a CPU from the I/O thread affinity set |
| `ZLINK_THREAD_NAME_PREFIX` | 9 | Prefix for I/O thread names |
| `ZLINK_CTX_OPT_BLOCKY` | 10 | Controls blocking behavior on context termination (`int`; default 1) |
| `ZLINK_CTX_OPT_AUTO_HWM_ENABLE` | 12 | Whether automatic HWM policy is enabled (`0` = disabled, `1` = enabled) |
| `ZLINK_CTX_OPT_AUTO_HWM_RECALC_DEBOUNCE_MS` | 14 | Minimum debounce window in milliseconds before connection churn triggers another automatic HWM recalculation (`>= 0`) |
| `ZLINK_CTX_OPT_AUTO_HWM_PROFILE` | 17 | Automatic HWM profile (`ZLINK_AUTO_HWM_PROFILE_*`). Invalid values fail with `EINVAL`. |
| Unassigned | 18 | Not a public option. Using this value fails with `EINVAL`. |
| `ZLINK_CTX_OPT_AUTO_HWM_MEMORY_LIMIT_BYTES` | 19 | Explicit memory limit to which the profile percentage is applied (`uint64_t` bytes). `0` means this input is unset. Set and query it with `zlink_ctx_set_data()` and `zlink_ctx_get_data()`. |
| `ZLINK_CTX_OPT_AUTO_HWM_RUNTIME_MEMORY_LIMIT_BYTES` | 20 | Memory-limit hint supplied by a managed runtime (`uint64_t` bytes). `0` means no hint was detected. Set and query it with `zlink_ctx_set_data()` and `zlink_ctx_get_data()`. |
| `ZLINK_CTX_OPT_AUTO_HWM_CORE_BUDGET_BYTES` | 21 | Exact Core budget that bypasses profile calculation (`uint64_t` bytes). `0` means no manual budget is set. Set and query it with `zlink_ctx_set_data()` and `zlink_ctx_get_data()`. |

> **Note:** `ZLINK_SOCKET_LIMIT` and `ZLINK_THREAD_PRIORITY` share the enum
> value `3`. In the current public C ABI the option lookup resolves value `3`
> to the read-only `ZLINK_SOCKET_LIMIT`, so `ZLINK_THREAD_PRIORITY` cannot be
> set or queried through `zlink_ctx_set` / `zlink_ctx_get`.

## Default Values

```c
#define ZLINK_IO_THREADS_DFLT           4
#define ZLINK_MAX_SOCKETS_DFLT          4095
#define ZLINK_THREAD_PRIORITY_DFLT      -1
#define ZLINK_THREAD_SCHED_POLICY_DFLT  -1
#define ZLINK_CTX_AUTO_HWM_ENABLE_DFLT  1
#define ZLINK_CTX_AUTO_HWM_RECALC_DEBOUNCE_MS_DFLT 3000
#define ZLINK_CTX_AUTO_HWM_PROFILE_DFLT ZLINK_AUTO_HWM_PROFILE_BALANCED
#define ZLINK_CTX_AUTO_HWM_MEMORY_LIMIT_BYTES_DFLT ((uint64_t) 0)
#define ZLINK_CTX_AUTO_HWM_RUNTIME_MEMORY_LIMIT_BYTES_DFLT ((uint64_t) 0)
#define ZLINK_CTX_AUTO_HWM_CORE_BUDGET_BYTES_DFLT ((uint64_t) 0)
```

| Constant | Value | Description |
|----------|-------|-------------|
| `ZLINK_IO_THREADS_DFLT` | 4 | Default number of I/O threads |
| `ZLINK_MAX_SOCKETS_DFLT` | 4095 | Default maximum socket count |
| `ZLINK_THREAD_PRIORITY_DFLT` | -1 | Default thread priority (OS default) |
| `ZLINK_THREAD_SCHED_POLICY_DFLT` | -1 | Default scheduling policy (OS default) |
| `ZLINK_CTX_AUTO_HWM_ENABLE_DFLT` | 1 | Automatic HWM policy enabled by default. Sockets use the balanced profile unless the application disables auto-HWM or sets manual HWM values. |
| `ZLINK_CTX_AUTO_HWM_RECALC_DEBOUNCE_MS_DFLT` | 3000 | Default debounce window for automatic HWM recalculation (ms) |
| `ZLINK_CTX_AUTO_HWM_PROFILE_DFLT` | `ZLINK_AUTO_HWM_PROFILE_BALANCED` | Default automatic HWM profile |
| `ZLINK_CTX_AUTO_HWM_MEMORY_LIMIT_BYTES_DFLT` | 0 | No explicit memory limit is configured. |
| `ZLINK_CTX_AUTO_HWM_RUNTIME_MEMORY_LIMIT_BYTES_DFLT` | 0 | No runtime memory hint is available. |
| `ZLINK_CTX_AUTO_HWM_CORE_BUDGET_BYTES_DFLT` | 0 | No manual Core budget is configured. |

`SNDBUF` / `RCVBUF` default to `-1`. This leaves the OS socket buffer size to
the OS default and TCP autotuning. Auto-HWM profiles do not change those values
automatically.

## Auto HWM Memory-Budget Calculation

This section defines the public calculation and admission contract. The
[Auto HWM internals](../../internals/auto-hwm.en.md) own queue-local state,
decoder reservations, and data-path cost boundaries.

Core selects the first available input in this order:

1. A positive `ZLINK_CTX_OPT_AUTO_HWM_CORE_BUDGET_BYTES`
2. A positive `ZLINK_CTX_OPT_AUTO_HWM_MEMORY_LIMIT_BYTES`
3. A positive runtime memory hint, reduced to the smaller value when Core also
   detects a finite hard limit
4. A finite hard limit detected by Core
5. Physical memory detected by Core

A manual Core budget is used unchanged. For every other memory input, Core
applies the selected profile percentage exactly once.

This budget is the normal-state distribution basis for application directional
pipe HWMs, not a hard cap enforced against context-wide actual usage. Each pipe
checks only its own HWM together with retained-credit leases originating from
that pipe. A pipe that reaches its HWM or uses its empty-pipe oversize exception
does not reduce another pipe's HWM or stop that pipe's admission.

| Profile | Percentage | General-data minimum | General-data maximum | STREAM minimum | STREAM maximum |
|---|---:|---:|---:|---:|---:|
| Compact | 2% | 32 KiB | 1 MiB | 8 KiB | 32 KiB |
| LowLatency | 5% | 32 KiB | 2 MiB | 16 KiB | 64 KiB |
| Balanced | 10% | 64 KiB | 4 MiB | 64 KiB | 128 KiB |
| Throughput | 20% | 128 KiB | 16 MiB | 256 KiB | 512 KiB |

Only the STREAM role uses the STREAM bounds. The `none` role is excluded from
planning; every other current role uses the general-data bounds. The percentage
calculation uses the following overflow-avoiding expression:

```text
effectiveCoreBudgetBytes =
    (resolvedMemoryLimitBytes / 100) * profilePercent
  + ((resolvedMemoryLimitBytes % 100) * profilePercent) / 100
```

When Core detects a finite hard limit, an explicit memory limit or manual Core
budget above that limit fails with `EINVAL`. A runtime memory hint may be set
above it; calculation then uses the smaller of the hint and the finite hard
limit.

A successful Auto HWM option setter stores the configured value and schedules
a calculation through the normal debounce path. `zlink_ctx_get_data` returns
the stored value immediately, but the budget snapshot reports the last recorded
plan and may therefore retain the previous result until recalculation.
`zlink_ctx_auto_hwm_recalculate` records a new plan immediately.

The ABI-v1 planner uses the context physical directional queue registry. The
registry records every application ypipe direction exactly once with a stable
queue ID and generation independent of endpoint ownership. It owns the send or
receive role, profile bounds, manual HWM, current applied HWM, and accounted
bytes. Observing the same inproc ypipe from two endpoints does not count the
direction twice.

Core atomically reserves the role minima for both application directions of a
new pipe pair. If both reservations cannot be made, Core rejects the pair
before publishing the attach and does not leave a partially registered
direction. A manual direction reserves its finite manual HWM. Manual HWM `0`
remains unlimited for admission, while planning reserves the role maximum and
clears the aggregate-HWM-valid flag.

An inproc physical ypipe does not add the values held by its two endpoints.
Core resolves one final cap with the following rules, then reserves and applies
that cap once in the registry.

| Send endpoint | Receive endpoint | Final physical-ypipe cap |
|---|---|---|
| Auto | Auto | Water-filling result |
| Finite manual | Auto | Finite manual cap |
| Auto | Finite manual | Finite manual cap |
| Finite manual A | Finite manual B | `min(A, B)` |
| Unlimited manual | Finite manual | Finite manual cap |
| Finite manual | Unlimited manual | Finite manual cap |
| Unlimited manual | Auto | Automatic plan |
| Auto | Unlimited manual | Automatic plan |
| Unlimited manual | Unlimited manual | Unlimited admission; reserve the role maximum for planning |

If the budget left after manual reservations is below the sum of all automatic
minimums, Core keeps those minimums and sets the insufficient-budget flag. With
sufficient budget, it divides the remainder by the number of unique unsaturated
physical queues and repeatedly raises each queue up to its role maximum.
Division remainders are granted one byte at a time in stable queue-ID order, so
the same registry snapshot and inputs always produce the same result.

If a new explicit memory limit or manual Core budget cannot accommodate the
current manual reservations and automatic minima together, the setter fails
with `ENOBUFS` and preserves the previous configuration and plan. A new
synchronous inproc attach also fails with `ENOBUFS` when it cannot reserve its
required minima. If a runtime memory hint or detected hard limit shrinks while
the context is running, Core does not remove existing pipes or messages; it
records the new input and sets the insufficient-budget flag. A new asynchronous
network attach is not published before its reservation succeeds, and Core
terminates the failed connection attempt.

When a connection increase lowers a queue target, Core records the new target
immediately and blocks further admission while existing bytes drain below it.
When a connection decrease raises a target, Core applies the increase only
after the cooldown and only to a live queue with the same generation. A
detached queue is removed after its outstanding retained leases reach zero; it
remains a retired queue while leases are outstanding.

Core reserves multipart frames as provisional bytes before allocation and
commits or rolls them back against the same physical queue. The empty-pipe
oversize exception applies only to one complete message whose total accounted
size is known at admission: a single-part or total-known message. An
incremental multipart whose final total is unknown follows the ordinary byte
HWM from its first `MORE` frame and never acquires the exception retroactively.
Core adds neither known-total metadata nor a whole-transaction reservation for
this exception, which also does not apply to concurrent oversize messages.
Moving a message from a Core queue to a Framework job does
not reduce accounted bytes; a retained-credit lease transfers only the owner.
Releasing the lease returns read credit to the exact origin queue generation
and does not wake another queue.

```text
originQueueUsedBytes(queue) =
    physicalQueueAccountedBytes(queue)
  + applicationLeaseBytesFrom(queue)
```

Ordinary admission checks only this origin-local sum and the applied HWM of
that queue. Core does not block other queues merely because context
`current_accounted_bytes` exceeds `effective_core_budget_bytes`.

`total_planned_hwm_bytes` is the current target sum for application directions,
and `total_applied_hwm_bytes` is the HWM sum actually applied to live
application directions. A retired entry retains only outstanding leases and
deferred origin credit; it contributes neither applied capacity nor the new
water-filling denominator. `core_queue_accounted_bytes` and
`application_accounted_bytes` distinguish owners; their
`current_accounted_bytes` sum does not change during an owner transfer.

The DEALER/ROUTER completion progress lane does not apply a byte HWM, LWM,
inproc HWM boost, or the legacy 256 KiB floor, and it is excluded from the
water-filling denominator above. This lane carries only terminal replies and
error replies. It is excluded from HWM admission and Core budget reservation,
but Core observes its current and peak accounted bytes and pending-message
count separately. Those values are included in
`total_messaging_accounted_bytes` and excluded from application water-filling.

## Functions

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
option constants table above for valid option names and their semantics.
`ZLINK_CTX_OPT_AUTO_HWM_ENABLE` takes effect on existing sockets
immediately, but only for sockets that still use automatic `SNDHWM` /
`RCVHWM` values rather than manual overrides. Setting it to `0` preserves the
last HWM applied to each current pipe, excludes those pipes from subsequent
automatic recalculation, and clears the snapshot planning-active flag.
`ZLINK_CTX_OPT_AUTO_HWM_PROFILE` updates the profile used by the next
automatic HWM calculation and is safe to change while the context is live.
The profile selects the memory percentage and role byte bounds. `SNDBUF` /
`RCVBUF` default to `-1`, and auto-HWM profiles do not change these values
automatically. The three Auto HWM byte options are not accepted by
`zlink_ctx_set`; using them there fails with `EINVAL`.

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
bytes. `0` means the input is unset, not unlimited. Every other size and the
removed context option value `18` fail with `ZLINK_CONFIG_INVALID_ARGUMENT`.

`ZLINK_THREAD_NAME_PREFIX` takes a null-terminated string. Pass the string
pointer as `optval_` and `strlen(prefix) + 1` as `optvallen_`. The prefix is
bounded to at most 16 bytes (`optvallen_ <= 16`) to fit the platform
thread-name limit.

**Returns:** `ZLINK_CONFIG_OK` on success; otherwise a `zlink_config_result_t` value. `zlink_errno()` retains the detailed internal errno for diagnostics.

**Errors:**
- `EINVAL` -- unknown option or invalid value.
- `ENOBUFS` -- a new explicit memory limit or manual Core budget cannot accommodate the current manual reservations and automatic minima together.
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

**Returns:** `ZLINK_CONFIG_OK` on success; otherwise a
`zlink_config_result_t` value. `zlink_errno()` retains the detailed internal
errno for diagnostics.

**Errors:**
- `EINVAL` -- unknown option or invalid output size.
- `EFAULT` -- invalid context handle or output pointer.

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
into `*error_out_` on failure; returns the option value as the primary return
on success.

**Returns:** The option value on success, or `-1` on failure with the
`zlink_config_result_t` written through `*error_out_`. `zlink_errno()` retains
the detailed internal errno for diagnostics.

**Errors:**
- `EINVAL` -- unknown option.
- `EFAULT` -- invalid context handle; `*error_out_` is set to `ZLINK_CONFIG_INVALID_HANDLE`.

**Thread safety:** Safe to call from any thread.

**See also:** `zlink_ctx_set`, `zlink_ctx_get_data`

---

### zlink_ctx_auto_hwm_recalculate

Run the automatic HWM planner for the whole context immediately.

```c
ZLINK_EXPORT zlink_config_result_t zlink_ctx_auto_hwm_recalculate(void *context_);
```

This function forces an immediate automatic HWM refresh for every socket in
the context that still follows the automatic queue and buffer policy. Manual
overrides remain manual, and disabled automatic HWM remains disabled. Use it
after changing the Auto HWM profile or a memory-budget input to record a new
plan without waiting for the normal refresh path.

**Returns:** `ZLINK_CONFIG_OK` on success; otherwise a
`zlink_config_result_t` value. `zlink_errno()` retains the detailed internal
errno for diagnostics.

**Errors:**
- `EFAULT` -- invalid context handle.

**Thread safety:** Safe to call from any thread.

**See also:** `zlink_ctx_set`, `zlink_monitor_status`

---

### zlink_ctx_get_auto_hwm_budget_snapshot

Read the last recorded context-wide Auto HWM plan into a versioned structure.

```c
#define ZLINK_AUTO_HWM_BUDGET_SNAPSHOT_ABI_V1 1u

#define ZLINK_AUTO_HWM_BUDGET_FLAG_PLANNING_ACTIVE       (1u << 0)
#define ZLINK_AUTO_HWM_BUDGET_FLAG_INSUFFICIENT          (1u << 1)
#define ZLINK_AUTO_HWM_BUDGET_FLAG_AGGREGATE_HWM_VALID   (1u << 2)
#define ZLINK_AUTO_HWM_BUDGET_FLAG_AGGREGATE_OVERFLOW    (1u << 3)

typedef struct zlink_auto_hwm_budget_snapshot_t {
  uint32_t abi_version;
  uint32_t struct_size;
  uint64_t budget_generation;
  uint64_t measurement_epoch;
  uint64_t configured_memory_limit_bytes;
  uint64_t runtime_memory_limit_bytes;
  uint64_t resolved_memory_limit_bytes;
  uint64_t configured_core_budget_bytes;
  uint64_t effective_core_budget_bytes;
  uint64_t total_planned_hwm_bytes;
  uint64_t total_applied_hwm_bytes;
  uint64_t manual_reserved_hwm_bytes;
  uint64_t core_queue_accounted_bytes;
  uint64_t application_accounted_bytes;
  uint64_t current_accounted_bytes;
  uint64_t provisional_accounted_bytes;
  uint64_t peak_accounted_bytes;
  uint64_t completion_current_accounted_bytes;
  uint64_t completion_peak_accounted_bytes;
  uint64_t completion_pending_message_count;
  uint64_t total_messaging_accounted_bytes;
  uint64_t monitor_queue_applied_hwm_bytes;
  uint64_t monitor_queue_accounted_bytes;
  uint64_t total_instance_applied_hwm_bytes;
  uint64_t total_instance_accounted_bytes;
  uint64_t oversize_admission_count;
  uint64_t largest_oversize_message_bytes;
  uint64_t active_directional_queue_count;
  uint64_t active_completion_directional_queue_count;
  uint64_t active_send_queue_count;
  uint64_t active_receive_queue_count;
  uint64_t outstanding_application_lease_count;
  uint64_t retired_queue_count;
  uint64_t deferred_origin_credit_bytes;
  uint64_t unlimited_manual_queue_count;
  uint32_t blocked_ratio_ppm;
  uint32_t flags;
  uint64_t reserved_u64[8];
} zlink_auto_hwm_budget_snapshot_t;

ZLINK_EXPORT zlink_config_result_t
zlink_ctx_get_auto_hwm_budget_snapshot(
  void *context_,
  zlink_auto_hwm_budget_snapshot_t *snapshot_);
```

The caller zero-initializes the structure, sets `abi_version` to
`ZLINK_AUTO_HWM_BUDGET_SNAPSHOT_ABI_V1`, and sets `struct_size` to the allocated
byte size. A null snapshot or a size shorter than the two header fields fails
with `EINVAL`; an unsupported version fails with `ENOTSUP`; an invalid context
fails with `EFAULT`; and a terminating context fails with `ETERM`. On success,
Core writes only the smaller of the caller-size and Core-v1 prefixes. The returned
`struct_size` is the full Core-v1 structure size.

V1 populates only these fields:

- `abi_version` and `struct_size`
- `budget_generation` and `measurement_epoch`
- configured, runtime, and resolved memory limits, plus configured and
  effective Core budgets
- `total_planned_hwm_bytes`, `total_applied_hwm_bytes`, and
  `manual_reserved_hwm_bytes`
- `core_queue_accounted_bytes`, `application_accounted_bytes`,
  `current_accounted_bytes`, `provisional_accounted_bytes`, and
  `peak_accounted_bytes`
- `completion_current_accounted_bytes`, `completion_peak_accounted_bytes`,
  `completion_pending_message_count`, and `total_messaging_accounted_bytes`
- `monitor_queue_applied_hwm_bytes`, `monitor_queue_accounted_bytes`,
  `total_instance_applied_hwm_bytes`, and `total_instance_accounted_bytes`
- `oversize_admission_count` and `largest_oversize_message_bytes`
- `active_directional_queue_count`,
  `active_completion_directional_queue_count`, `active_send_queue_count`,
  `active_receive_queue_count`, `outstanding_application_lease_count`,
  `retired_queue_count`, `deferred_origin_credit_bytes`, and
  `unlimited_manual_queue_count`
- `blocked_ratio_ppm`
- The four `ZLINK_AUTO_HWM_BUDGET_FLAG_*` bits

Every element of `reserved_u64` is zero.

| Flag | Set when |
|---|---|
| `ZLINK_AUTO_HWM_BUDGET_FLAG_PLANNING_ACTIVE` | Auto HWM was enabled in the last recorded plan. |
| `ZLINK_AUTO_HWM_BUDGET_FLAG_INSUFFICIENT` | Manual reservations and required automatic minimums cannot fit together within the budget. |
| `ZLINK_AUTO_HWM_BUDGET_FLAG_AGGREGATE_HWM_VALID` | No manual-unlimited direction makes the aggregate HWM non-finite. |
| `ZLINK_AUTO_HWM_BUDGET_FLAG_AGGREGATE_OVERFLOW` | A planner or queue aggregate saturated past the `uint64_t` range. |

`active_directional_queue_count` and
`active_completion_directional_queue_count` count each unique physical ypipe
direction once; its reader and writer endpoints share that identity. Completion
directions are excluded from the application planning denominator and HWM
admission. `active_send_queue_count` and `active_receive_queue_count` remain
the perspective counts in the last socket plan. A new context starts with
`budget_generation == 0` and `measurement_epoch == 1`. `budget_generation`
increments whenever a new plan is recorded; `measurement_epoch` increments on
metrics reset.

Physical queue accounting adds payload bytes and `sizeof(zlink_msg_t)` for
each frame. Incomplete multipart frames appear in
`provisional_accounted_bytes`; the final frame converts the same bytes to
committed accounting without adding them twice. Reads, rollback, hiccup,
termination, and conflate replacement return the charge for the frames they
actually remove. Completion directions report separate current, peak, and
complete-message pending values.

`monitor_queue_applied_hwm_bytes` sums the currently applied HWM once for each
active physical ypipe direction owned by an open monitor. It does not add the
option copies held by both the reader and writer endpoints.
`monitor_queue_accounted_bytes` sums the accounted bytes currently held by
those same monitor directions. Neither value contributes to application
planning, `total_applied_hwm_bytes`, `current_accounted_bytes`, or
`total_messaging_accounted_bytes`; they contribute only to these instance
totals:

```text
total_instance_applied_hwm_bytes =
    total_applied_hwm_bytes + monitor_queue_applied_hwm_bytes

total_instance_accounted_bytes =
    total_messaging_accounted_bytes + monitor_queue_accounted_bytes
```

Completion queues have no HWM and therefore do not contribute to
`total_instance_applied_hwm_bytes`. A sum that exceeds the `uint64_t` range
saturates at `UINT64_MAX` and sets
`ZLINK_AUTO_HWM_BUDGET_FLAG_AGGREGATE_OVERFLOW`.

When retained-credit receive returns a physical frame, its charge moves
atomically from `core_queue_accounted_bytes` to
`application_accounted_bytes`. `current_accounted_bytes` is the saturating sum
of those fields and does not change for an ownership transfer alone.
`peak_accounted_bytes` is the largest such sum observed when a budget snapshot
or an automatic HWM recalculation samples the queues during the current
measurement epoch. It does not promise to capture a shorter-lived value between
two sampling boundaries. `outstanding_application_lease_count` is the number of public leases
not yet released. `deferred_origin_credit_bytes` is exact-origin byte credit
held by either an internal framing token or a public lease and not yet
published to the writer. `retired_queue_count` counts directional queue
generations kept after detach or generation replacement because they still
have a retained origin. Releasing an old generation changes neither accounting,
credit, nor wake state for a new generation; the final origin release removes
the retired entry.

The snapshot is a coherent registry view belonging to one
`budget_generation`. Queue counts, capacity, and accounted counters from
different generations are not mixed. Current counters may change while Core
constructs the snapshot, but the returned aggregate and component fields are
mutually consistent at the same snapshot boundary.

Registry-owned fields and sampled fields become visible at different times. The
registry updates `application_accounted_bytes`,
`outstanding_application_lease_count`, and `deferred_origin_credit_bytes`
synchronously with the call that changes them. `current_accounted_bytes`,
`provisional_accounted_bytes`, and `peak_accounted_bytes` are sampled from
per-pipe accounting when the snapshot is taken, and a lease release publishes
its credit to the owning pipe asynchronously. A snapshot taken immediately
after `zlink_hwm_budget_lease_release` on another thread may therefore still count
the released bytes. The contract guarantees exactly-once release and internal
snapshot consistency, not that a release is visible in the very next snapshot.
Poll the snapshot when a test or an operator needs to observe the settled
value.

`blocked_ratio_ppm` is calculated as follows:

```text
floor(first_blocked_admission_attempts * 1,000,000 / total_admission_attempts)
```

The ratio is zero when `total_admission_attempts` is zero.
A retry after the same submission wakes is not counted again. The numerator
contains only first attempts blocked by the target application pipe HWM; it
excludes transport-I/O waits and context-aggregate usage.

---

### zlink_ctx_reset_auto_hwm_budget_metrics

Advance the current Auto HWM measurement epoch.

```c
ZLINK_EXPORT zlink_config_result_t
zlink_ctx_reset_auto_hwm_budget_metrics(void *context_);
```

On success, `measurement_epoch` increments by one, application and completion
peaks are rebased to their current accounted bytes, and the blocked and total
admission-attempt counters, `blocked_ratio_ppm`, cumulative oversize count, and
maximum are reset to zero. Current bytes, monitor queue HWM and
accounted bytes, budget, plan, queue counts, and `budget_generation` remain
unchanged. An invalid context fails with
`EFAULT`; a terminating context fails with `ETERM`.
