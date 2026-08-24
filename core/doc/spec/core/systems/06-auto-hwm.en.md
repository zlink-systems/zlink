[한국어](https://zlink-systems.github.io/zlink/ko/spec/core/systems/06-auto-hwm/) | English

<!-- zlink-nav:start -->
[Systems Index](README.en.md) | [Previous: Per-Connection Memory](05-connection-memory.en.md) | [Next: Core Source Layout](07-core-source-layout.en.md)
<!-- zlink-nav:end -->

# Auto HWM

> **What this chapter defines** — the Auto HWM budget calculation and
> admission contract, the related context functions, and their internal
> implementation.

## 1. Overview

zlink Core automatically computes the byte ceiling each socket queue keeps
([HWM](../glossary.en.md#hwm)) from the context memory budget and divides it
among the queues, so the application does not have to set each one
individually. This automatic policy is called
[Auto HWM](../glossary.en.md#auto-hwm-budget).

This document defines the public contract for which inputs compute that
budget and how it applies to message admission, the related context
functions, and their internal implementation. The functions belong to the
context object (`zlink_ctx_*`), but this document treats Auto HWM as a
single topic and covers them together here.

The documents that own related contracts are as follows.

| Related contract | Owning document |
|---|---|
| Auto HWM option enum and defaults | [Context](../01-context.en.md) |
| Socket HWM options and observable behavior | [Socket Common](../socket/README.en.md) |
| Short definition of each term | [Core Glossary](../glossary.en.md) |

## 2. Auto HWM budget calculation

This section defines the public calculation and admission contract. The
[Internals](#4-internals) section of this document owns queue-local state,
decoder reservations, and data-path cost boundaries.

Core checks the following inputs in priority order and picks the first usable
value. The `ZLINK_CTX_OPT_AUTO_HWM_*` below are
[Context options](../01-context.en.md#4-options) the application
sets with `zlink_ctx_set_data`.

1. A positive `ZLINK_CTX_OPT_AUTO_HWM_CORE_BUDGET_BYTES`
2. A positive `ZLINK_CTX_OPT_AUTO_HWM_MEMORY_LIMIT_BYTES`
3. A positive runtime memory hint, reduced to the smaller value when Core also
   detects a finite hard limit
4. A finite hard limit detected by Core
5. Physical memory detected by Core

A manual Core budget is used unchanged. For every other memory input, Core
applies the selected profile percentage exactly once and then clamps the result
to the effective cap defined below.

This budget is the normal-state distribution basis for application directional
pipe HWMs, not a hard cap enforced against context-wide actual usage. Each pipe
checks only its own HWM together with retained-credit leases originating from
that pipe. A pipe that reaches its HWM or uses its empty-pipe oversize exception
does not reduce another pipe's HWM or stop that pipe's admission.

| Profile | Percentage | Fixed cap | General-data minimum | General-data maximum | STREAM minimum | STREAM maximum |
|---|---:|---:|---:|---:|---:|---:|
| Compact | 2% | 64 MiB | 32 KiB | 512 KiB | 8 KiB | 32 KiB |
| LowLatency | 3% | 256 MiB | 32 KiB | 2 MiB | 16 KiB | 64 KiB |
| Balanced | 5% | 512 MiB | 64 KiB | 1 MiB | 64 KiB | 128 KiB |
| Throughput | 8% | 1024 MiB | 128 KiB | 8 MiB | 256 KiB | 512 KiB |

Only the STREAM role uses the STREAM bounds. The `none` role is excluded from
planning; every other current role uses the general-data bounds, with one
exception: under the Balanced profile the `recv_ingress` role (SUB/XSUB) uses
2 MiB instead of the general-data maximum.

The percentage alone does not determine the budget. The budget is the
percentage share clamped to an **effective cap**, which is the larger of the
profile's fixed cap and the floor every active application directional queue
needs in order to reach its own role minimum. A fixed cap alone would push a
queue-heavy deployment below its own minima; a queue floor alone would let a
large host reserve gigabytes for a handful of queues.

These three expressions produce the final Core budget
(`effectiveCoreBudgetBytes`), the basis for dividing each queue's steady-state
HWM described above.

```text
percentShareBytes =                                            // memory limit × profile ratio
    (resolvedMemoryLimitBytes / 100) * profilePercent
  + ((resolvedMemoryLimitBytes % 100) * profilePercent) / 100  // integer division avoids overflow

effectiveCapBytes =                                            // larger of the two below
    max (profileFixedCapBytes,                                 //   profile fixed cap
         activeDirectionalQueueCount * perQueueMinimumBytes)   //   total for every queue to reach its minimum

effectiveCoreBudgetBytes = min (percentShareBytes, effectiveCapBytes)  // final budget = ratio value clamped by the cap
```

`perQueueMinimumBytes` is that profile's general-data role minimum.
`activeDirectionalQueueCount` is known only once the physical queue registry has
resolved every plannable application direction, so the budget is settled at that
point (context finalize). Setting a manual Core budget skips this calculation
entirely.

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

The **observable behavior** of multipart reservation, the empty-queue oversize
exception, and the retained-credit lease is owned by
[§5 Verification](#5-implementation-and-contract-test-verification); its
**implementation mechanism** is owned by [§4 Internals](#4-internals).

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

## 3. Functions

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
per-pipe accounting when the snapshot is taken. Poll the snapshot when a test
or an operator needs to observe a later settled value.

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

## 4. Internals

This document defines, for Core maintainers implementing Auto HWM memory
limits, what state to read and update on the message-processing path. The
application-visible budget calculation, HWM scope, and errors are owned by
[§2 Auto HWM budget calculation](#2-auto-hwm-budget-calculation) of this
document and by the
[Socket specification](../socket/README.en.md#transportbuffer).

### Value limited by HWM

Auto HWM divides a context memory budget among application directional queues.
A message is admitted by comparing the target physical queue's unreturned
charge with its applied HWM, not by comparing context-wide usage.

```text
frameCharge = payloadBytes + sizeof(msg_t)

outstandingCharge =
    provisionalCharge
  + committedQueueCharge
  + retainedLeaseCharge
```

The fixed `sizeof(msg_t)` component is not an allocator measurement. It keeps
empty frames from consuming zero HWM while they still occupy a queue slot and
a message object. Payload-only accounting cannot bound a stream of empty
single-part messages or empty multipart frames.

`provisionalCharge` is reserved before a decoder allocates a payload buffer.
`committedQueueCharge` belongs to frames stored by the queue. A frame retained
by the application moves to `retainedLeaseCharge`. Moving ownership does not
change the amount for which the writer has not received credit.

For example, when the HWM is 1,024 bytes and the unreturned charge is 900
bytes, ordinary admission accepts only a frame whose charge is at most 124
bytes. The state of another queue and the context-wide total do not change this
decision.

### Separation of responsibilities

| Processing location | Input | Result |
|---|---|---|
| Budget planner | Memory inputs, profile, application queue set | Target HWM per queue |
| Queue configuration | Target HWM, applied value, queue generation | HWM applied to that generation |
| Message processing | Target queue charge and frame charge | Admission or backpressure |
| Snapshot | Per-queue HWM and charge | Context query result |

The planner runs for option and queue-lifecycle changes. Context aggregates
and snapshot metrics are not message-admission inputs.

### Message-processing sequence

1. The writer or decoder adds the fixed frame cost to the payload size.
2. An overflowing sum is rejected.
3. Before allocating a payload buffer, a decoder reserves the candidate charge
   in target queue-local state. An application writer skips this step for an
   already-created message.
4. A nonzero applied HWM rejects a candidate that would make the unreturned
   charge exceed the HWM, except for the public empty-queue oversize rule.
5. Enqueue converts a provisional charge to committed state without adding it
   again.
6. Dequeue removes committed charge. A retained receive transfers the same
   charge to its lease; an ordinary receive returns byte credit to the writer.
7. Drop, allocation failure, protocol failure, and shutdown return only the
   charge that the operation owns, exactly once.

Normal frame processing reads and updates only state created with its queue.

### Multipart and oversize messages

Multipart messages accumulate each frame's charge. After reading a wire frame
length, a decoder reserves that frame before allocating its buffer. The final
frame publishes the multipart without charging preceding frames again.

An incremental multipart that reaches HWM stops before allocating its next
frame. Discarding a multipart returns every charge it reserved or committed.

An empty queue may admit one complete message whose total charge is known at
admission even when it exceeds HWM. The exception does not admit two messages
at once and does not bypass `ZLINK_OPT_MAXMSGSIZE`. The
[Socket specification](../socket/README.en.md#transportbuffer) owns
the public rule.

### Retained receive and queue generations

A retained receive lets the application keep a dequeued frame's memory. It
does not return charge at dequeue. Lease release returns credit to the writer
of the originating queue generation.

Detach and reconnect create a new generation. Releasing an old lease neither
decreases the new generation's charge nor wakes its writer. A retired
generation keeps only the return target until its final lease and reservation
finish.

### HWM changes

An increase applies to the current queue generation. A decrease below the
unreturned charge preserves admitted frames and blocks new admission. Core
applies the lower HWM after the charge reaches the target.

DEALER and ROUTER completion queues do not use application HWM so terminal and
error replies can progress. Monitor queues are excluded from application
budget distribution.

### Message-path cost limits

Send, receive, and decoder admission do not perform:

- context-wide mutex acquisition;
- global map lookup by queue or reservation ID;
- per-frame heap allocation for reservations;
- per-frame updates of context-wide current, provisional, or peak totals;
- global atomic updates for metrics not used by HWM admission;
- usage lookup for another physical queue.

A decoder or pipe stores its reservation inline: target queue reference,
generation, reserved charge, and active state. A lifecycle registry may manage
attach, detach, and retired generations, but normal frame admission does not
look it up.

### Snapshots and metrics

A snapshot composes context totals from queue-local state when requested. It
may take registry locks needed for the query, but it does not serialize every
queue through a lock shared with message processing.

Peak metrics retain the largest aggregate observed when a snapshot or an
automatic HWM recalculation collects the queue-local values. Because the hot
path does not update a context-wide total for every frame, a value that exists
only between two observation boundaries may not appear in the peak. Metrics
reset and repeated snapshot reads do not alter admission or writer credit.

### Implementation locations

| Responsibility | Implementation location |
|---|---|
| Profile bounds and per-queue HWM calculation | `auto_hwm_policy.*` |
| Context inputs, recalculation, snapshot API | `ctx_auto_hwm_*` |
| Physical queue identity and generation | `ctx_physical_queue_registry.*` |
| Queue-local charge, admission, and byte credit | `pipe.*` |
| Pre-allocation reservation | `zmp_decoder.*`, `session_base_pipe_io.cpp`, `pipe.*` |
| Retained lease release | Retained receive API and queue lifecycle code |

## 5. Implementation and contract-test verification

This section collects the items a worker verifies. They are behaviors observable
only through the **public surface** — the context options
`zlink_ctx_set_data`/`zlink_ctx_get_data`, `zlink_ctx_get_auto_hwm_budget_snapshot`,
send/recv admission results, and errno — not internal implementation, and each
maps to a single unit test.

**Options and budget**
- Setting a memory limit or manual Core budget larger than a finite hard limit that Core detected fails with `EINVAL`.
- Calling `zlink_ctx_set_data`/`zlink_ctx_get_data` for an Auto HWM byte option with a size other than exactly `sizeof(uint64_t)` fails with `EINVAL` and leaves the value unchanged.
- For the same connection layout and inputs, the snapshot's `effective_core_budget_bytes` is always identical (deterministic).

**Admission (byte accounting)**
- Frames with no payload still consume HWM — repeatedly sending empty frames still blocks admission at the HWM.
- An empty queue admits one total-known complete message even above the HWM, and rejects the second oversize message.
- An incremental multipart of unknown total size blocks from the point it exceeds the HWM, and after it is discarded the snapshot's `provisional_accounted_bytes` returns to 0.

**Credit, lease, and generation**
- After an ordinary recv the sender can send again; a retained receive keeps the snapshot's `application_accounted_bytes` until the lease is released.
- Detaching a socket while holding a lease adds no credit to a new generation (snapshot).

**HWM changes**
- Lowering the HWM keeps already-admitted frames and applies the new HWM to admission only after the backlog drains below the new target.

**Excluded queues**
- DEALER/ROUTER completion replies and monitor traffic do not change application send admission or the snapshot's `total_planned_hwm_bytes` denominator.

**Snapshot invariance**
- Calling `zlink_ctx_get_auto_hwm_budget_snapshot` or `zlink_ctx_reset_auto_hwm_budget_metrics` does not change the accept/reject outcome of the same send sequence.
- An unsupported `abi_version` fails with `ENOTSUP`; a terminating context fails with `ETERM`.
