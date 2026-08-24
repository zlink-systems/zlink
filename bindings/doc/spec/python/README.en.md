---
title: "Python Bindings Public Contract"
---

<!-- bindings-nav:start -->
[Spec index](../README.en.md) | [Previous: Node.js](../node/README.en.md) | [Next: Go](../go/README.en.md)
<!-- bindings-nav:end -->

# Python binding Core 0.13.1 public contract

> **What this chapter defines** — the public type, ownership, and error
> contract the `zlink` Python package provides on top of Core 0.13.1 raw
> messaging.

- This document defines the Core 0.13.1 raw messaging contract the `zlink` Python package provides.
- A feature not in the current implementation and public header is not part of this contract.
- It supports Python 3.9 and later; the package version is `0.13.1`.
- The current native package target is Linux x86_64; other targets are outside this contract's supported scope until a separate candidate payload and clean-consumer verification exist.

| Section | Covers |
|---|---|
| [Scope](#scope) | The list of public Python types that represent Core resources |
| [Package surface](#package-surface) | The boundary between the public factories and the private area |
| [Byte HWM and Auto-HWM](#byte-hwm-and-auto-hwm) | Mapping between Python `int` and Core `uint64_t` byte HWM |
| [Ownership and lifetime](#ownership-and-lifetime) | Ownership/release rules for native handles, messages, and Received |
| [Callback surface](#callback-surface) | The public callback paths, and the primitives that stay unexposed |
| [Send/receive and no-data](#sendreceive-and-no-data) | How submit and no-data are represented, and how native failures are delivered |
| [Receive flow state](#receive-flow-state) | The receive-flow state type, setter, and monitor surface |
| [Error](#error) | The `ZlinkError` family and its result fields |
| [Python version and the type package](#python-version-and-the-type-package) | The supported Python version and the type-check target |
| [Related documents](#related-documents) | Links to the guide, the Core spec, and internals |

## Scope

The Python binding represents the following Core resources as Python
objects and Protocols.

| Area | Public concepts |
|---|---|
| Core | `Context`, `ContextOptions`, `Message`, `Received`, `RoutingId` |
| Socket | PAIR, DEALER, ROUTER, STREAM, PUB, SUB, XPUB, XSUB |
| Eventing | `MonitorSocket`, `MonitorEvent`, `MonitorStatus`, `Poller`, `PollEvents`, `Timer` |
| Utility | `AtomicCounter`, `Stopwatch`, `Thread`, `proxy`, `sleep` |
| Result | `SubmitResult`, `RequestResult`, `RecvResult`, `ConfigResult`, and their matching errors |

A socket preserves Core's raw endpoint and message-routing semantics
as-is. The Python binding does not expose Core's internal handles, FFI
symbols, or native structs as public types.

## Package surface

Users work with the factories and contract types at the `zlink` package
root. They do not import implementation modules directly, and `_native`
and `_runtime` are private areas. The package root has no separate domain
type or compatibility alias outside the Core raw contract.

The main factories are `create_context()`, `create_pair_socket()`,
`create_dealer_socket()`, `create_router_socket()`,
`create_stream_socket()`, `create_pub_socket()`, `create_sub_socket()`,
`create_poller()`, `create_timer()`, `create_received()`, and the
`create_message` family. The exact Python signatures are governed jointly
by the contract module in the same directory and the public header.

## Byte HWM and Auto-HWM

Core owns HWM calculation and queue admission. Python
`send_high_water_mark` and `receive_high_water_mark` are byte-valued `int`
properties and accept only the non-negative `uint64_t` range. The binding
passes each value to Core as an exact 8-byte option, and a getter returns
Core's 64-bit value as a Python `int`. `0` means unlimited.

The context passes byte-valued `core_hwm_memory_limit_bytes` and
`core_hwm_budget_bytes`, plus `core_hwm_profile`, unchanged to Core. Core applies
the profile ratio and distributes the result across physical directional
queues exactly once. Setting a directional HWM makes that direction a manual
override and excludes it from automatic HWM recalculation.
Context also provides `core_hwm_budget_snapshot()` and
`reset_core_hwm_budget_metrics()`. Input precedence is manual Core budget,
explicit memory limit, a runtime hint only when a distinct VM hard limit is
unambiguous, then Core fallback. Setting either of the first two values disables
automatic runtime-hint detection. The binding does not combine the hint with
Core's hard limit. If an explicit input exceeds a finite hard limit Core
detected, the binding preserves the existing configuration error corresponding
to `EINVAL` and does not clamp the value.

Core decides backpressure when the accounted bytes retained by a pipe reach
the applied HWM. The Python binding does not recount messages and passes the
native result through the existing submit and error contract.
`monitor_open(events=..., monitor_hwm_bytes=...)` accepts a non-negative
`uint64_t`-range Python `int`. Zero selects the Core monitor default; a positive
value is forwarded unchanged, with no message-count alias or conversion. Planned,
applied, and deferred HWM and in-flight usage in `MonitorStatus` are byte-valued
Python `int` fields. Pending-message counts remain display diagnostics; no
slot, message-unit, size-cap, or connection-bucket property is exposed.
`snd_pending_bytes` and `rcv_pending_bytes` are separate byte values.

The Core budget snapshot projects ABI version/size, configured/runtime/resolved
memory limits, configured/effective budgets, planned/applied/manual-reserved
HWM, Core-queue/application/current/peak/provisional accounted bytes,
completion current/peak/pending and total-messaging values, monitor/instance
aggregates, application/completion queue counts,
`outstanding_application_lease_count`, `retired_queue_count`,
`deferred_origin_credit_bytes`, oversize/blocked/aggregate flags,
`budget_generation`, and `measurement_epoch` as Python `int`/boolean values.
`application_accounted_bytes` and those three owner-lifecycle fields are
ABI-reserved and always zero. Reset preserves current, pending, and queue-count
gauges, rebases both peaks to current, clears epoch counters, and increments
`measurement_epoch`. An ABI version/size mismatch is an unsupported error.

## Ownership and lifetime

- `Context` owns the native context and releases it via `close()` or context-manager exit.
- A socket, monitor, poller, or timer owns the native handle it creates. A call that uses the handle after a successful `close()` is not allowed.
- `Message.from_(value)` creates a native message independent of the caller's value. After a successful send submit, native ownership of the message part moves to the Core send path.
- `Received` is a receive storage the caller creates. On a successful `recv_into(received)`, the parts and routing metadata are recorded into `Received`; native parts are released on `close()` or context-manager exit.
- The native view `Received`'s `parts` provides is valid only while its owner stays open. If it must outlive that, copy the value with `to_bytes()` or `to_bytes_list()`.
- Once a callback is registered, the callback and any Python references it needs are not released before the native callback registration is. A callback exception is delivered per the binding's callback error policy.

Core byte-HWM charge ends when ordinary `recv_into` or `subscribe_into`
dequeues the payload. `Received` and `TopicMessage` own only the Python
lifetime of native parts, routing id, request sequence, topic, and multipart
framing. Closing, leaving a context manager, or reusing storage does not
participate in Core HWM accounting. No separate retained receive, raw lease
handle, application byte capacity, allowance, or duplicate accounting state
exists in a public or internal API.

## Callback surface

Core FFI's `zlink_recv_handler()` is a private implementation primitive the
Python package does not expose directly. Python's public
callback surface is fixed to `on_packet` for a STREAM packet and `on_event`
for a monitor event. It provides no separate public method to register a raw
receive or routed request completion callback. There is no public
`on_send_ready`. HWM-managed send completion is delivered only through the
awaitable `submit()` already returns (Core's `zlink_send_complete_handler`
notification), never through a separate readiness callback.

## Send/receive and no-data

The bindings own zero threads, queues, or retry anywhere in this surface
(`bindings/doc/spec/async-coroutine-policy.ko.md`).

- Unrelated synchronous builders such as PUB and STREAM sends and a
  ROUTER reply add message parts, then call `submit()`. Raw ROUTER/`Received`
  reply `submit()` is a synchronous one-shot returning `None` and submits a
  terminal reply or error reply to the HWM-free completion lane with one native
  call. HWM backpressure is not a reply result; `NOT_CONNECTED`, `TERMINATED`,
  `INVALID_ARGUMENT`, and other non-HWM submit failures immediately raise
  `SubmitError`. `publish()` (PUB/XPUB) is likewise synchronous-only: PUB
  semantics are lossy by default (a full subscriber queue silently drops that
  subscriber's copy; the publisher never waits), `ZLINK_PUB_OPT_NODROP`
  surfaces an immediate `SubmitError`/`BACKPRESSURED` instead, and
  `zlink_send_async` returns `ENOTSUP` for PUB/XPUB — there is no publish
  awaitable.
- HWM-managed send — PAIR `send()` and DEALER/ROUTER routed `send()` — and
  `request()` are ASYNC because both can pass through Core's HWM admission
  queue. The sole terminal on these builders is `submit()`. Use
  `await pair.send().message(message).submit()`,
  `await dealer.send().message(message).submit()`, and
  `reply = await dealer.request().message(request).submit()`. `submit()`
  immediately returns an awaitable coroutine object and does not perform a
  native blocking submit before returning.
  - **send** hands the complete record to Core in one
    `zlink_send_async(socket, parts, count, options, &op_id)` call
    (`core/include/zlink/socket/api.h`). Completion is exactly-once, driven
    by the single `zlink_send_complete_handler` the socket installs at
    construction, and can run inline (Core admitted immediately, so the
    awaitable is already resolved and the awaiter never suspends), on Core's
    own async mailbox thread, on Core's deadline thread on timeout, or on the
    closing thread during close. The binding correlates a completion to its
    awaitable through an opaque token carried in
    `zlink_send_async_options_t.userdata` — not through the Core-assigned
    `op_id`, which is only known after the call returns and may arrive too
    late for an inline completion. There is no per-op Python timer; the
    deadline is the Core-side `zlink_send_async_options_t.timeout_ms` field
    (the same pattern `_runtime/eventing/timer.py` already uses for
    Core-owned timing). Cancelling the awaitable maps to
    `zlink_send_async_cancel`.
  - **request** submits once through Core's routed request entry point
    (`zlink_dealer_request_transport_pair_part` /
    `zlink_router_request_transport_pair_part`) and is completed purely by
    Core's reply callback — there is no admission ticket and no
    binding-owned polling thread driving completion. A submit that Core
    cannot admit immediately (e.g. `BACKPRESSURED`) raises `SubmitError`
    right away instead of being queued and retried by the binding; a reply
    timeout is Core's own `ZLINK_REQUEST_TIMED_OUT` deadline. ROUTER resolves
    an exact transport-pair target via `zlink_select_routed_submit_target`
    before submitting; DEALER lets Core commit one selection at submit time.
  - Coroutine cancellation terminally resolves the pending operation exactly
    once. Waiting on one target's send or request blocks neither another
    target's submit nor the Python event loop — Core's own per-target queue
    orders admission, not a Python-side wait. The same routed operation
    exposes no flags, callback, blocking terminal, or `submit_async()`
    compatibility terminal.
- A caller-provided receive using `RecvFlags.DONT_WAIT` returns `False` when there is no message.
- A direct-return control API such as a timer or monitor returns `None` when there is no pending value.
- An actual native failure is delivered as its matching error type, never hidden as no-data.

DEALER and ROUTER request/reply preserve Core's routing metadata and
request sequence. A ROUTER receive's `Received.routing_id` is the raw
routing id and is never converted to a different identity type. The
current single-part accessor name matches the implementation and contract
tests: `single_part_or_throw()`. A name change happens only after a
separate draft is approved.

## Receive flow state

The binding exposes the Core receive-flow state as the `ReceiveFlowState`
`IntEnum` with `RUNNING = 0` and `PAUSED = 1`.
`Socket.set_receive_flow_state(state)` sets it. It returns `None` and follows
the Python error policy: a non-zero native result raises `ConfigError` carrying
the matching `ConfigResult` and the native errno, so a socket without a
completion lane raises `ConfigError` with `ConfigResult.NOT_SUPPORTED`. Setting
the state the socket already holds returns normally.

The observation surface follows the C contract, so the constant and metric
names are fixed by the C layer: the monitor events `SEND_FLOW_PAUSED`,
`SEND_FLOW_RESUMED`, and `FLOW_STATE_STALE` (`1 << 16`, `1 << 17`, `1 << 18`,
with the full mask `0x7FFFF`), the event flags `SEND_FLOW_WRITABLE` (`1 << 1`),
`FLOW_STATE_STALE_GENERATION` (`1 << 2`), and `FLOW_STATE_STALE_EPOCH`
(`1 << 3`), the status detail bit `FLOW_STATE` (`1 << 5`), and the five status
fields `flow_paused_connections`, `flow_pause_applied_total`,
`flow_resume_applied_total`, `flow_state_stale_total`, and
`flow_pause_duration_ms`, projected with this language's naming convention.

Flow-state frames stay inside Core. The binding calls the setter, reads the
monitor events and the snapshot fields, and never encodes, decodes, sends, or
receives a flow-state frame itself.

## Error

A call that returns a Core result gives its matching Python error `result`,
`code`, and `native_errno`. An input-format error can be checked before the
call, but a native operation failure is never turned into a plain
`ValueError`. `SubmitError`, `RequestError`, `RecvError`, `BindError`,
`ConnectError`, `ConfigError`, `CloseError`, and `HandlerError` are all in
the `ZlinkError` family.

## Python version and the type package

Public annotations use a form the Python 3.9 parser and runtime can
resolve. The package root includes `py.typed`. Public contract type
checking targets the Python 3.9 target `pyrightconfig.json` specifies, and
`src/zlink/contracts`.

## Related documents

- Usage follows the [Python guide](../../guide/python/index.en.md).
- The reference for Core functions and layout is the repository's `core/include/zlink.h` and the Core spec.
- Implementation detail and callback/native lifetime explanations belong to the internals documents, not this one.
