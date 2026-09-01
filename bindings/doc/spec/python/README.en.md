---
title: "Python Bindings Public Contract"
---

<!-- bindings-nav:start -->
[Spec index](../README.en.md) | [Previous: Node.js](../node/README.en.md) | [Next: Go](../go/README.en.md)
<!-- bindings-nav:end -->

# Python binding Core 0.16.0 public contract

> **What this chapter defines** — the public type, ownership, and error
> contract the `zlink` Python package provides on top of Core 0.16.0 raw
> messaging.

- This document defines the Core 0.16.0 raw messaging contract the `zlink` Python package provides.
- A feature not defined by this document and the public header is not part of the Python binding contract.
- It supports Python 3.9 and later; the package version is `0.16.0`.
- The current native package target is Linux x86_64; other targets are outside this contract's supported scope until a separate candidate payload and clean-consumer verification exist.

| Section | Covers |
|---|---|
| [Scope](#scope) | The list of public Python types that represent Core resources |
| [Package surface](#package-surface) | The boundary between the public factories and the private area |
| [Byte HWM and Auto-HWM](#byte-hwm-and-auto-hwm) | Mapping between Python `int` and Core `uint64_t` byte HWM |
| [Ownership and lifetime](#ownership-and-lifetime) | Ownership/release rules for native handles, messages, and Received |
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

Core byte-HWM charge ends when ordinary `recv_into` or `subscribe_into`
dequeues the payload. `Received` and `TopicMessage` own only the Python
lifetime of native parts, routing ID, reply token, topic, and multipart
framing. Closing, leaving a context manager, or reusing storage does not
participate in Core HWM accounting. No separate retained receive, raw lease
handle, application byte capacity, allowance, or duplicate accounting state
exists in a public or internal API.

## Send/receive and no-data

- Send/request builder completion boundaries, the provisional registry, and cancellation follow the
  [async completion surface policy](../async-coroutine-policy.en.md).
- Reply and publish end with synchronous `submit()`. Only a separate `PublishOp` provides publish
  flags.
- Caller-provided receive with `RecvFlags.DONT_WAIT` returns `False` when no message is available.
- Direct-return control APIs such as timer and monitor return `None` when no value is pending.
- An actual native failure is delivered through its corresponding error type and is not hidden as
  no-data.

DEALER and ROUTER request/reply preserve Core routing metadata and `ReplyToken`.
`Received.routing_id` from ROUTER receive is a routing ID and is not converted into another identity
type. The single-part accessor is named `single_part_or_throw()`, matching the implementation and
contract tests.

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
and `FLOW_STATE_STALE_EPOCH` (`1 << 3`), the status detail bit `FLOW_STATE`
(`1 << 5`), and the five status
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

## Pull completion public contract

The Python package uses Core 0.16.0 as an exact dependency.

The Python runtime drains native completions and converts them into blocking results or awaitables.
`submit_sync()` uses Core `NONE`; `submit()` uses Core `DONTWAIT`. Completion-backed state is registered
in a provisional registry before native `FINAL` and completes exactly once after submit publication and
completion capture join. Awaitable cancellation ends only the caller wait and does not cancel the Core
operation; a late completion releases the payload and state.

`PollEventFlag.POLLCOMPLETION` is a progress event indicating that the public poller's wait thread
drained the native queue and fully processed at least one live awaitable or detached state. Under public
poller ownership, using a blocking request requires another thread to continue executing the wait loop.

Only module-private `_reply_token_from_native` creates a `ReplyToken`; public construction and
serialization are rejected. The factory fills private `_owner` and `_value` through
`object.__new__(ReplyToken)` and `object.__setattr__`. Equality and hashing use both owner identity and
the opaque value. `StreamPacket` is an empty reusable output. Publish preserves its existing flags and
synchronous submit semantics on a `PublishOp` separate from send. A token provides no raw property,
`int()` conversion, ordering, or `close()`. Concurrent recv into the same output is invalid-state.
Message references remain valid only until the next recv entry or `close()`. Before the first
bind/connect, the `recv_mode` setter accepts only `RAW` and `PACKET` and rejects `UNSPECIFIED`.

### Public interface

```python
class SendOp(Protocol):
    def message(self, payload) -> "SendOp": ...
    def messages(self, *payloads) -> "SendOp": ...
    def submit(self) -> Awaitable[None]: ...
    def submit_sync(self) -> None: ...

class RequestOp(Protocol):
    def message(self, payload) -> "RequestOp": ...
    def messages(self, *payloads) -> "RequestOp": ...
    def timeout(self, timeout) -> "RequestOp": ...
    def submit(self) -> Awaitable[list[Message]]: ...
    def submit_sync(self) -> list[Message]: ...

class ReplyOp(Protocol):
    def message(self, payload) -> "ReplyOp": ...
    def messages(self, *payloads) -> "ReplyOp": ...
    def submit(self) -> None: ...

class PublishOp(Protocol):
    def message(self, payload) -> "PublishOp": ...
    def messages(self, *payloads) -> "PublishOp": ...
    def flags(self, flags) -> "PublishOp": ...
    def submit(self) -> None: ...

@final
class ReplyToken:
    __slots__ = ("_owner", "_value")

    def __new__(cls) -> NoReturn:
        raise TypeError("ReplyToken is created by ROUTER request receive")

    def __eq__(self, other: object) -> bool: ...
    def __hash__(self) -> int: ...
    def __repr__(self) -> str: return "ReplyToken()"
    def __copy__(self) -> "ReplyToken": return self
    def __deepcopy__(self, memo) -> "ReplyToken": return self
    def __reduce_ex__(self, protocol):
        raise TypeError("ReplyToken cannot be serialized")

class StreamRecvMode(IntEnum):
    UNSPECIFIED = 0
    RAW = 1
    PACKET = 2

class StreamSocketOptions(Protocol):
    @property
    def recv_mode(self) -> StreamRecvMode: ...

    @recv_mode.setter
    def recv_mode(self, mode: StreamRecvMode) -> None: ...

class StreamPacket:
    routing_id: Optional[RoutingId]
    header: Optional[Message]
    body: Optional[Message]

    def __init__(self) -> None: ...
    @property
    def is_empty(self) -> bool: ...
    def close(self) -> None: ...
    def __enter__(self) -> "StreamPacket": ...
    def __exit__(self, exc_type, exc, tb) -> None: ...

class Received:
    routing_id: Optional[RoutingId]
    reply_token: Optional[ReplyToken]

class StreamSocket:
    def send(self, routing_id: RoutingId) -> SendOp: ...
    def recv_into(
        self, out: Received, *, flags: RecvFlags = RecvFlags.NONE
    ) -> bool: ...
    def recv_packet_into(
        self, out: StreamPacket, *, flags: RecvFlags = RecvFlags.NONE
    ) -> bool: ...
```

The operation-start signatures are PAIR `send() -> SendOp`, DEALER `send() -> SendOp` and
`request() -> RequestOp`, ROUTER `send(routing_id) -> SendOp`,
`request(routing_id) -> RequestOp`, and `reply(routing_id, token) -> ReplyOp`, and STREAM
`send(routing_id) -> SendOp`. A send factory captures the target in the builder. PUB and XPUB
`publish(topic)` return `PublishOp`. `Received.send()` returns a `SendOp` that captures the source
target, and `Received.reply()` returns a `ReplyOp` that captures the source RID and token.

The public Python surface contains no `RoutedSendOp`, `StreamSocket.send_async()` or `on_packet()`,
request callback argument, reply `_FlaggedFluentMessageOp` or flags, monitor `ignore_handler` or
`on_event`, timer `on_fire`, or pair/generation member.

Monitor provides `recv(*, flags=RecvFlags.NONE) -> Optional[MonitorEvent]`, `status()`, and `close()`.
Timer provides `start(interval_ns:int, repeat_count:int)`, `stop()`, `recv() -> Optional[int]`, and
`close()`. Monitor-event `connection_id` is used only for diagnostics and correlation, not as a
send/reply target or reconnect fence. The internal FFI enum mirror uses only
`ZLINK_OPT_PENDING_MAX_MSGS` and `ZLINK_OPT_PENDING_MAX_BYTES` and adds no public option property.

## Implementation and contract-test verification requirements

Verify the following using only public Python protocols, results, exceptions, and poller events. Each
item maps to one contract test.

**Operations and completion**

- Send/request expose only the flag-free awaitable and synchronous terminals in the Public interface
  section and retain request timeout.
- `publish(topic)` returns a separate `PublishOp` with publish flags and synchronous submit.
- Even when completion drains before submit returns, the awaitable completes exactly once after joining
  submit publication.
- After awaitable cancellation, a late completion does not complete the awaitable again and releases the
  native payload.
- A non-OK request completion exposes only a typed exception and does not expose the error payload.
- `POLLCOMPLETION` returns only after settlement or detached cleanup finishes.

**ReplyToken and STREAM**

- Public `ReplyToken()` and pickle serialization fail; `copy.copy()` and `copy.deepcopy()` return the
  same immutable valid token.
- Only tokens with the same owner and value are equal, and reply with a token from another owner fails
  before the native call.
- `recv_packet_into()` fills the output after success and leaves it empty on no-data or error. The output
  can be reused after `close()`.

**Pull eventing**

- Monitor and timer recv return no-data as `None` and expose events and fire counts without callbacks.
